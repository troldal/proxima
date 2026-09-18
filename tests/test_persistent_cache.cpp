// Replies that outlive the process. The file format on its own, then the part
// that has to be right: what a persistent key must be qualified by.

#include <doctest/doctest.h>

#include "kernel/persistent_cache.hpp"

#include <proxima/config.hpp>
#include <proxima/assumptions.hpp>
#include <proxima/errors.hpp>
#include <proxima/expr.hpp>
#include <proxima/functions.hpp>
#include <proxima/kernel.hpp>
#include <proxima/ops.hpp>
#include <proxima/symbol.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using proxima::Expr;
using proxima::Symbol;
using proxima::detail::PersistentCache;

namespace {

/// A directory of its own per test, removed first so a rerun starts clean.
std::filesystem::path scratch(const std::string &name) {
    const std::filesystem::path path
        = std::filesystem::temp_directory_path() / "proxima_cache_tests" / name;
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
    return path;
}

proxima::detail::Reply valued(const std::string &value) {
    proxima::detail::Reply reply;
    reply.ok = true;
    reply.value = value;
    return reply;
}

std::size_t file_count(const std::filesystem::path &directory) {
    std::error_code ec;
    std::size_t count = 0;
    for (const auto &entry : std::filesystem::directory_iterator(directory, ec)) {
        count += entry.is_regular_file() ? std::size_t{1} : std::size_t{0};
    }
    return count;
}

} // namespace

// --- Maxima-free ----------------------------------------------------------

TEST_CASE("an entry survives being written and read back") {
    const auto directory = scratch("roundtrip");
    {
        const PersistentCache cache(directory, "stamp");
        REQUIRE(cache.usable());
        cache.insert("2+2", valued("4"));
    }

    // A different object entirely, as a later process would be.
    const PersistentCache reopened(directory, "stamp");
    const auto found = reopened.find("2+2");
    REQUIRE(found.has_value());
    CHECK(found->ok);
    CHECK(found->value == "4");
}

TEST_CASE("values containing newlines and quotes survive") {
    // Entries are length-prefixed rather than delimited, so nothing needs
    // escaping and nothing can be read short.
    const auto directory = scratch("awkward");
    const PersistentCache cache(directory, "stamp");

    proxima::detail::Reply reply;
    reply.ok = false;
    reply.value = "";
    reply.reason = "line one\nline \"two\"\n\nand a trailing newline\n";
    cache.insert("q", reply);

    const auto found = cache.find("q");
    REQUIRE(found.has_value());
    CHECK_FALSE(found->ok);
    CHECK(found->reason == reply.reason);
}

TEST_CASE("a different stamp is a different cache") {
    // The stamp carries the Maxima version, this library's version and the
    // assumption state. Change any of them and the old answer must not be
    // visible, because it was computed under conditions that no longer hold.
    const auto directory = scratch("stamp");

    const PersistentCache under_one(directory, "maxima=5.50");
    under_one.insert("sqrt(x^2)", valued("abs(x)"));
    REQUIRE(under_one.find("sqrt(x^2)").has_value());

    const PersistentCache under_another(directory, "maxima=5.51");
    CHECK_FALSE(under_another.find("sqrt(x^2)").has_value());

    // And they coexist rather than overwriting one another.
    under_another.insert("sqrt(x^2)", valued("x"));
    CHECK(under_one.find("sqrt(x^2)")->value == "abs(x)");
    CHECK(under_another.find("sqrt(x^2)")->value == "x");
}

TEST_CASE("a corrupt or truncated entry is ignored, not misread") {
    const auto directory = scratch("corrupt");
    const PersistentCache cache(directory, "stamp");
    cache.insert("q", valued("42"));

    // Truncate every file in the directory to its first line.
    for (const auto &entry : std::filesystem::directory_iterator(directory)) {
        std::ofstream out(entry.path(), std::ios::binary | std::ios::trunc);
        out << "proxima-cache-1\n";
    }
    CHECK_FALSE(cache.find("q").has_value());
}

TEST_CASE("a length larger than the file is a miss, not an exception") {
    // Fields are length-prefixed, and the length used to be trusted: a corrupt
    // or hostile entry claiming 18446744073709551615 bytes made the reader try
    // to allocate that much, and std::length_error escaped eval_pure — not an
    // proxima::Error, and not a cache miss.
    const auto directory = scratch("huge_length");
    const PersistentCache cache(directory, "stamp");
    cache.insert("q", valued("42"));
    REQUIRE(file_count(directory) == 1);
    const std::filesystem::path entry
        = std::filesystem::directory_iterator(directory)->path();

    SUBCASE("in the first field") {
        std::ofstream out(entry, std::ios::binary | std::ios::trunc);
        out << "proxima-cache-1\n18446744073709551615\n";
    }
    SUBCASE("after a key that matches") {
        // The key is the stamp, a blank line, then the question.
        const std::string key = "stamp\n\nq";
        std::ofstream out(entry, std::ios::binary | std::ios::trunc);
        out << "proxima-cache-1\n"
            << key.size() << '\n' << key << "1\n1" << "18446744073709551615\n";
    }
    SUBCASE("or just longer than what is there") {
        std::ofstream out(entry, std::ios::binary | std::ios::trunc);
        out << "proxima-cache-1\n1000\nshort";
    }

    std::optional<proxima::detail::Reply> found;
    CHECK_NOTHROW(found = cache.find("q"));
    CHECK_FALSE(found.has_value());
}

TEST_CASE("a file from an unknown writer is ignored") {
    const auto directory = scratch("foreign");
    const PersistentCache cache(directory, "stamp");
    cache.insert("q", valued("42"));

    for (const auto &entry : std::filesystem::directory_iterator(directory)) {
        std::ofstream out(entry.path(), std::ios::binary | std::ios::trunc);
        out << "some-other-format-9\nwhatever\n";
    }
    CHECK_FALSE(cache.find("q").has_value());
}

TEST_CASE("a temporary is named for its writer, not only its order") {
    // Named by an in-process counter alone, the first write in *every* process
    // was <entry>.tmp0, so two processes writing one entry at once shared a
    // file: one truncated the other's half-written content, and a corrupt entry
    // could be renamed into place. The name now carries a per-process token.
    const std::filesystem::path target
        = std::filesystem::path("somewhere") / "0123456789abcdef.reply";
    const std::filesystem::path first = proxima::detail::temporary_path_for(target);
    const std::filesystem::path second = proxima::detail::temporary_path_for(target);

    CHECK(first != second);
    CHECK(first.parent_path() == target.parent_path());

    const std::string prefix = "0123456789abcdef.reply.tmp-";
    const std::string a = first.filename().generic_string();
    const std::string b = second.filename().generic_string();
    CAPTURE(a);
    CAPTURE(b);
    REQUIRE(a.rfind(prefix, 0) == 0);
    REQUIRE(b.rfind(prefix, 0) == 0);
    REQUIRE(a.size() > prefix.size() + 17);

    // <token>-<n>: sixteen hex digits naming this process, the same on every
    // call, then the count.
    const std::string token = a.substr(prefix.size(), 16);
    CHECK(token.find_first_not_of("0123456789abcdef") == std::string::npos);
    CHECK(b.substr(prefix.size(), 16) == token);
    CHECK(a[prefix.size() + 16] == '-');
}

TEST_CASE("another writer's temporaries are left alone") {
    // Half-written files of another process, beside the entry this one writes:
    // one under the old naming, which this process could have picked too, and
    // two under the new naming with a token that is not ours.
    //
    // The cross-process guarantee itself rests on the token, checked above;
    // this checks that insert touches nothing but its own file.
    const auto directory = scratch("collide");
    const PersistentCache cache(directory, "stamp");
    cache.insert("q", valued("42"));
    REQUIRE(file_count(directory) == 1);
    const std::filesystem::path entry
        = std::filesystem::directory_iterator(directory)->path();
    std::filesystem::remove(entry);

    const std::string partial = "half-written by someone else";
    std::vector<std::filesystem::path> foreign;
    for (const char *suffix :
         {".tmp0", ".tmp-ffffffffffffffff-0", ".tmp-ffffffffffffffff-1"}) {
        std::filesystem::path path = entry;
        path += suffix;
        std::ofstream out(path, std::ios::binary);
        out << partial;
        foreign.push_back(path);
    }

    cache.insert("q", valued("42"));

    for (const std::filesystem::path &path : foreign) {
        CAPTURE(path.filename().generic_string());
        std::ifstream in(path, std::ios::binary);
        std::string contents;
        std::getline(in, contents);
        CHECK(contents == partial);
    }
    REQUIRE(cache.find("q").has_value());
    CHECK(cache.find("q")->value == "42");
    // The entry and the three foreign files: no temporary of ours left behind.
    CHECK(file_count(directory) == 4);
}

namespace {

/// Total size of the entries in `directory`, temporaries excluded.
std::uintmax_t entry_bytes(const std::filesystem::path &directory) {
    std::uintmax_t total = 0;
    for (const auto &entry : std::filesystem::directory_iterator(directory)) {
        if (entry.path().extension() == ".reply") {
            total += entry.file_size();
        }
    }
    return total;
}

/// Sets an entry's modification time `minutes` into the past, standing in for
/// the passage of time without waiting for it.
void age(const PersistentCache &cache, const std::string &source, int minutes) {
    std::filesystem::last_write_time(cache.entry_path(source),
                                     std::filesystem::file_time_type::clock::now()
                                         - std::chrono::minutes{minutes});
}

} // namespace

TEST_CASE("the cache directory is held under its limit, oldest entries first") {
    // There used to be no limit at all: a cache directory grew for ever.
    const auto directory = scratch("limit");
    const PersistentCache cache(directory, "stamp", 10'000);
    const std::string payload(1000, 'v'); // About 1 KB an entry.

    for (int i = 0; i < 30; ++i) {
        const std::string source = "q" + std::to_string(i);
        cache.insert(source, valued(payload));
        age(cache, source, 30 - i); // Each written a minute after the last.
    }

    CHECK(entry_bytes(directory) <= 10'000);
    CHECK(cache.find("q29").has_value());
    CHECK(cache.find("q28").has_value());
    CHECK_FALSE(cache.find("q0").has_value());
    CHECK_FALSE(cache.find("q20").has_value());
}

TEST_CASE("reading a recently used entry does not write to it") {
    // A hit used to rewrite the file's modification time every time: a write
    // to disk on every read. Within the hour it is left alone.
    const auto directory = scratch("quiet-read");
    const PersistentCache cache(directory, "stamp", 1'000'000);
    cache.insert("q", valued("42"));
    age(cache, "q", 5);
    const auto before = std::filesystem::last_write_time(cache.entry_path("q"));

    REQUIRE(cache.find("q").has_value());
    CHECK(std::filesystem::last_write_time(cache.entry_path("q")) == before);
}

TEST_CASE("reading an entry back keeps it from eviction") {
    // Four entries of about 1 KB under a 4.5 KB limit; a fifth goes over it,
    // and the sweep keeps three quarters of the limit, about three entries.
    const auto directory = scratch("recency");
    const PersistentCache cache(directory, "stamp", 4500);
    const std::string payload(1000, 'v');

    const std::vector<std::string> sources{"a", "b", "c", "d"};
    for (std::size_t i = 0; i < sources.size(); ++i) {
        cache.insert(sources[i], valued(payload));
        // Hours rather than minutes: a read only refreshes an entry whose
        // last use is an hour old or more. a oldest, d newest.
        age(cache, sources[i], 60 * (10 - static_cast<int>(i)));
    }
    REQUIRE(file_count(directory) == 4);

    // a is the oldest write, but the most recent read.
    REQUIRE(cache.find("a").has_value());
    cache.insert("e", valued(payload));

    CHECK(cache.find("a").has_value());
    CHECK(cache.find("e").has_value());
    CHECK(cache.find("d").has_value());
    CHECK_FALSE(cache.find("b").has_value());
    CHECK_FALSE(cache.find("c").has_value());
}

TEST_CASE("a sweep clears orphaned temporaries and leaves a writer's fresh ones") {
    const auto directory = scratch("orphans");
    std::filesystem::create_directories(directory);
    const std::filesystem::path orphan = directory / "0123456789abcdef.reply.tmp-ffffffffffffffff-0";
    const std::filesystem::path fresh = directory / "fedcba9876543210.reply.tmp-ffffffffffffffff-1";
    for (const auto &path : {orphan, fresh}) {
        std::ofstream(path, std::ios::binary) << "half-written by someone else";
    }
    std::filesystem::last_write_time(orphan, std::filesystem::file_time_type::clock::now()
                                                 - std::chrono::hours{2});

    // The first write under a limit scans the directory, which sweeps it.
    const PersistentCache cache(directory, "stamp", 1'000'000);
    cache.insert("q", valued("42"));

    CHECK_FALSE(std::filesystem::exists(orphan));
    CHECK(std::filesystem::exists(fresh));
    CHECK(cache.find("q").has_value());
}

TEST_CASE("a limit of zero keeps everything") {
    const auto directory = scratch("unlimited");
    const PersistentCache cache(directory, "stamp", 0);
    const std::string payload(1000, 'v');
    for (int i = 0; i < 20; ++i) {
        cache.insert("q" + std::to_string(i), valued(payload));
    }
    CHECK(file_count(directory) == 20);
}

TEST_CASE("the hash is stable, and not std::hash") {
    // A cache on disk outlives the build that wrote it, and
    // std::hash<std::string> differs between standard libraries. These values
    // are FNV-1a and must not drift.
    CHECK(proxima::detail::stable_hash("") == "cbf29ce484222325");
    CHECK(proxima::detail::stable_hash("a") == "af63dc4c8601ec8c");
    CHECK(proxima::detail::stable_hash("foobar") == "85944171f73967e8");
    CHECK(proxima::detail::stable_hash("a").size() == 16);
}

// --- Against a real kernel -------------------------------------------------

TEST_SUITE("maxima") {

TEST_CASE("an answer survives the kernel that computed it") {
    const auto directory = scratch("across_kernels");
    proxima::Config config;
    config.cache_directory = directory;

    const Symbol x("x");
    const Expr question = pow(Expr(x) + 1, 12);

    {
        proxima::Kernel first(config);
        static_cast<void>(proxima::expand(question, first));
        CHECK(file_count(directory) > 0);
    }

    // A second kernel, a fresh Maxima process, and an empty in-memory cache.
    proxima::Kernel second(config);
    const Expr answer = *proxima::expand(question, second);

    // The count is what makes this conclusive: an answer computed afresh would
    // land in the in-memory cache too, so the presence of an entry proves
    // nothing on its own. Only persistent_hits distinguishes the two.
    CHECK(second.cache_stats().persistent_hits == 1);
    CHECK(answer == *proxima::expand(question, second));
}

TEST_CASE("an assumption is part of the key, not an afterthought") {
    // The soundness case for the whole feature. sqrt(x^2) is abs(x) normally
    // and x under assume(x > 0). If the assumption were left out of the key, a
    // process that had made it would poison the answer for one that had not —
    // and unlike the in-memory cache, there is no moment at which the second
    // process could be told to discard anything.
    const auto directory = scratch("assumption_key");
    proxima::Config config;
    config.cache_directory = directory;

    const Symbol x("assumption_key_probe");
    const Expr root = proxima::sqrt(pow(Expr(x), 2));

    {
        proxima::Kernel first(config);
        CHECK(*proxima::simplify(root, {proxima::assuming(gt(Expr(x), Expr(0))), first}) == Expr(x));
    }

    // A different process would see only the directory. This kernel makes no
    // assumption, so it must not be handed the assuming kernel's answer.
    proxima::Kernel plain(config);
    CHECK(*proxima::simplify(root, plain) == proxima::abs(Expr(x)));
    CHECK(plain.cache_stats().persistent_hits == 0);

    SUBCASE("while one asking under the same assumptions is answered from disk") {
        // Written in another order: the key is the canonical form, so it is
        // the same key.
        const Symbol y("assumption_key_other");
        {
            proxima::Kernel writer(config);
            const auto both = proxima::assuming(gt(Expr(x), Expr(0))).with(gt(Expr(y), Expr(0)));
            CHECK(*proxima::simplify(root, {both, writer}) == Expr(x));
        }
        proxima::Kernel reader(config);
        const auto same = proxima::assuming(gt(Expr(y), Expr(0))).with(gt(Expr(x), Expr(0)));
        CHECK(*proxima::simplify(root, {same, reader}) == Expr(x));
        CHECK(reader.cache_stats().persistent_hits == 1);
    }
}

TEST_CASE("a statement switches persistence off for that kernel") {
    // Nothing in a statement says what it changed in Maxima, so no key can
    // describe the session after it.
    const auto directory = scratch("raw_eval");
    proxima::Config config;
    config.cache_directory = directory;

    proxima::Kernel kernel(config);
    const Symbol x("x");

    static_cast<void>(proxima::expand(pow(Expr(x) + 1, 5), kernel));
    const std::size_t before = file_count(directory);
    CHECK(before > 0);
    CHECK(kernel.persistence_active());

    static_cast<void>(kernel.tell(proxima::Statement::text("raw_eval_probe: 7")));
    CHECK_FALSE(kernel.persistence_active());
    static_cast<void>(proxima::expand(pow(Expr(x) + 1, 6), kernel));
    // Still working, just no longer writing entries it could not honestly key.
    CHECK(file_count(directory) == before);

    // And no longer reading them either: asking again for something already on
    // disk goes to Maxima rather than to a key that no longer describes this
    // session.
    static_cast<void>(proxima::expand(pow(Expr(x) + 1, 5), kernel));
    CHECK(kernel.cache_stats().persistent_hits == 0);

    SUBCASE("until a restart discards the change") {
        // There used to be no way back short of a new Kernel.
        kernel.restart();
        CHECK(kernel.persistence_active());
        // The statement's binding went with the old process, which is exactly
        // what makes the disk answers trustworthy again. Asking is a question,
        // so it leaves persistence on — where the raw eval this used to be
        // switched it off again.
        CHECK(kernel.ask(proxima::Query::text("is(raw_eval_probe = 7)")) == Expr::symbol("false"));
        CHECK(kernel.persistence_active());
        static_cast<void>(proxima::expand(pow(Expr(x) + 1, 5), kernel));
        CHECK(kernel.cache_stats().persistent_hits == 1);
    }
}

TEST_CASE("a kernel restarted after dying resumes persistence") {
    // Recovery starts a fresh process, which discards what any statement
    // changed just as restart() does. Persistence used to stay off regardless.
    const auto directory = scratch("recovered");
    proxima::Config config;
    config.cache_directory = directory;

    proxima::Kernel kernel(config);
    static_cast<void>(kernel.tell(proxima::Statement::text("recovered_probe: 7")));
    REQUIRE_FALSE(kernel.persistence_active());

    CHECK_THROWS_AS(static_cast<void>(kernel.tell(proxima::Statement::text("quit()"))),
                    proxima::KernelError);
    CHECK(kernel.persistence_active());
}

TEST_CASE("persistence is off unless a directory is asked for") {
    // A library should not start writing files somewhere on its own.
    proxima::Config config;
    CHECK(config.cache_directory.empty());

    proxima::Kernel kernel(config);
    const Symbol x("x");
    CHECK_NOTHROW(static_cast<void>(proxima::expand(pow(Expr(x) + 1, 3), kernel)));
}

} // TEST_SUITE("maxima")
