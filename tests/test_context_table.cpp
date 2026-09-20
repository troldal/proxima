// The table of Maxima contexts kept for sets of assumptions: which set has
// which context, promoted on use, the least recently used evicted past
// capacity. Pure bookkeeping, with no Maxima behind it, which is the point
// of its being a unit of its own.

#include <doctest/doctest.h>

#include "kernel/context_table.hpp"

#include <optional>
#include <string>
#include <vector>

using proxima::detail::ContextTable;

TEST_CASE("a context is found by its key, once recorded") {
    ContextTable table(4);
    CHECK_FALSE(table.find("n > 0").has_value());

    const std::string name = table.next_name();
    CHECK(table.insert("n > 0", name).empty());
    CHECK(table.find("n > 0") == name);
    CHECK(table.size() == 1);
}

TEST_CASE("names are never reused, even after a clear") {
    ContextTable table(4);
    const std::string first = table.next_name();
    const std::string second = table.next_name();
    CHECK(first != second);

    table.insert("a", first);
    table.clear();
    CHECK(table.size() == 0);
    CHECK_FALSE(table.find("a").has_value());
    CHECK(table.next_name() != first);
    CHECK(table.next_name() != second);
}

TEST_CASE("past capacity, the least recently used goes, never the newest") {
    ContextTable table(2);
    table.insert("a", "ctx_a");
    table.insert("b", "ctx_b");

    // Using a makes b the oldest.
    CHECK(table.find("a") == "ctx_a");

    const std::vector<std::string> evicted = table.insert("c", "ctx_c");
    CHECK(evicted == std::vector<std::string>{"ctx_b"});
    CHECK(table.size() == 2);
    CHECK(table.find("a") == "ctx_a");
    CHECK(table.find("c") == "ctx_c");
    CHECK_FALSE(table.find("b").has_value());
}

TEST_CASE("a capacity of one keeps only the newest") {
    ContextTable table(1);
    CHECK(table.insert("a", "ctx_a").empty());
    CHECK(table.insert("b", "ctx_b") == std::vector<std::string>{"ctx_a"});
    CHECK(table.find("b") == "ctx_b");
    CHECK_FALSE(table.find("a").has_value());
}
