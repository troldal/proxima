// Replies that outlive the process. The file format on its own, then the part
// that has to be right: what a persistent key must be qualified by.

#include <doctest/doctest.h>

#include "kernel/persistent_cache.hpp"

#include <mx/config.hpp>
#include <mx/context.hpp>
#include <mx/expr.hpp>
#include <mx/functions.hpp>
#include <mx/kernel.hpp>
#include <mx/ops.hpp>
#include <mx/symbol.hpp>

#include <filesystem>
#include <fstream>
#include <string>

using mx::Expr;
using mx::Symbol;
using mx::detail::PersistentCache;

namespace {

/// A directory of its own per test, removed first so a rerun starts clean.
std::filesystem::path scratch(const std::string &name) {
    const std::filesystem::path path
        = std::filesystem::temp_directory_path() / "mx_cache_tests" / name;
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
    return path;
}

mx::Reply valued(const std::string &value) {
    mx::Reply reply;
    reply.ok = true;
    reply.value = value;
    return reply;
}

std::size_t fileCount(const std::filesystem::path &directory) {
    std::error_code ec;
    std::size_t count = 0;
    for (const auto &entry : std::filesystem::directory_iterator(directory, ec)) {
        count += entry.is_regular_file() ? 1 : 0;
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

    mx::Reply reply;
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

    const PersistentCache underOne(directory, "maxima=5.50");
    underOne.insert("sqrt(x^2)", valued("abs(x)"));
    REQUIRE(underOne.find("sqrt(x^2)").has_value());

    const PersistentCache underAnother(directory, "maxima=5.51");
    CHECK_FALSE(underAnother.find("sqrt(x^2)").has_value());

    // And they coexist rather than overwriting one another.
    underAnother.insert("sqrt(x^2)", valued("x"));
    CHECK(underOne.find("sqrt(x^2)")->value == "abs(x)");
    CHECK(underAnother.find("sqrt(x^2)")->value == "x");
}

TEST_CASE("a corrupt or truncated entry is ignored, not misread") {
    const auto directory = scratch("corrupt");
    const PersistentCache cache(directory, "stamp");
    cache.insert("q", valued("42"));

    // Truncate every file in the directory to its first line.
    for (const auto &entry : std::filesystem::directory_iterator(directory)) {
        std::ofstream out(entry.path(), std::ios::binary | std::ios::trunc);
        out << "maxima_cpp-cache-1\n";
    }
    CHECK_FALSE(cache.find("q").has_value());
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

TEST_CASE("the hash is stable, and not std::hash") {
    // A cache on disk outlives the build that wrote it, and
    // std::hash<std::string> differs between standard libraries. These values
    // are FNV-1a and must not drift.
    CHECK(mx::detail::stableHash("") == "cbf29ce484222325");
    CHECK(mx::detail::stableHash("a") == "af63dc4c8601ec8c");
    CHECK(mx::detail::stableHash("foobar") == "85944171f73967e8");
    CHECK(mx::detail::stableHash("a").size() == 16);
}

// --- Against a real kernel -------------------------------------------------

TEST_SUITE("maxima") {

TEST_CASE("an answer survives the kernel that computed it") {
    const auto directory = scratch("across_kernels");
    mx::Config config;
    config.cacheDirectory = directory;

    const Symbol x("x");
    const Expr question = pow(Expr(x) + 1, 12);

    {
        mx::Kernel first(config);
        mx::expand(question, first);
        CHECK(fileCount(directory) > 0);
    }

    // A second kernel, a fresh Maxima process, and an empty in-memory cache.
    mx::Kernel second(config);
    const Expr answer = mx::expand(question, second);

    // The count is what makes this conclusive: an answer computed afresh would
    // land in the in-memory cache too, so the presence of an entry proves
    // nothing on its own. Only persistentHits distinguishes the two.
    CHECK(second.cacheStats().persistentHits == 1);
    CHECK(answer == mx::expand(question, second));
}

TEST_CASE("an assumption is part of the key, not an afterthought") {
    // The soundness case for the whole feature. sqrt(x^2) is abs(x) normally
    // and x under assume(x > 0). If the assumption were left out of the key, a
    // process that had made it would poison the answer for one that had not —
    // and unlike the in-memory cache, there is no moment at which the second
    // process could be told to discard anything.
    const auto directory = scratch("assumption_key");
    mx::Config config;
    config.cacheDirectory = directory;

    const Symbol x("assumption_key_probe");
    const Expr root = mx::sqrt(pow(Expr(x), 2));

    {
        mx::Kernel assuming(config);
        mx::Context ctx(assuming);
        ctx.assume(gt(Expr(x), Expr(0)));
        CHECK(mx::simplify(root, assuming) == Expr(x));
    }

    // A different process would see only the directory. This kernel makes no
    // assumption, so it must not be handed the assuming kernel's answer.
    mx::Kernel plain(config);
    CHECK(mx::simplify(root, plain) == mx::abs(Expr(x)));
}

TEST_CASE("a raw eval switches persistence off for that kernel") {
    // Nothing in the text of a raw eval says whether it changed Maxima's state,
    // so the journal can no longer be trusted to describe the session — and the
    // journal is what the key is built from.
    const auto directory = scratch("raw_eval");
    mx::Config config;
    config.cacheDirectory = directory;

    mx::Kernel kernel(config);
    const Symbol x("x");

    mx::expand(pow(Expr(x) + 1, 5), kernel);
    const std::size_t before = fileCount(directory);
    CHECK(before > 0);

    kernel.eval("raw_eval_probe: 7");
    mx::expand(pow(Expr(x) + 1, 6), kernel);

    // Still working, just no longer writing entries it could not honestly key.
    CHECK(fileCount(directory) == before);

    // And no longer reading them either: asking again for something already on
    // disk goes to Maxima rather than to a key that no longer describes this
    // session.
    mx::expand(pow(Expr(x) + 1, 5), kernel);
    CHECK(kernel.cacheStats().persistentHits == 0);
}

TEST_CASE("persistence is off unless a directory is asked for") {
    // A library should not start writing files somewhere on its own.
    mx::Config config;
    CHECK(config.cacheDirectory.empty());

    mx::Kernel kernel(config);
    const Symbol x("x");
    CHECK_NOTHROW(mx::expand(pow(Expr(x) + 1, 3), kernel));
}

} // TEST_SUITE("maxima")
