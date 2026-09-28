#include <aaf/cfb/builder.hpp>

#include <catch2/catch_test_macros.hpp>

#include <functional>

using namespace aaf::cfb;

namespace
{

struct TreeCheck
{
    int blackHeight = -1;
    bool valid = true;
    std::vector<std::uint32_t> inOrder;
};

auto walk(const detail::TreeLayout& t, std::uint32_t node, bool parentRed, int blacks, TreeCheck& check) -> void
{
    if (node == kNoStream)
    {
        if (check.blackHeight < 0)
        {
            check.blackHeight = blacks;
        }
        check.valid = check.valid && check.blackHeight == blacks;
        return;
    }
    const bool red = t.red[node];
    check.valid = check.valid && !(red && parentRed);
    const int next = blacks + (red ? 0 : 1);
    walk(t, t.left[node], red, next, check);
    check.inOrder.push_back(node);
    walk(t, t.right[node], red, next, check);
}

}

TEST_CASE("Sibling layout is a valid red-black search tree", "[cfb][builder]")
{
    for (std::uint32_t n = 0; n <= 300; ++n)
    {
        const auto t = detail::layoutSiblings(n);
        TreeCheck check;
        walk(t, t.root, false, 0, check);
        INFO("n = " << n);
        CHECK(check.valid);
        REQUIRE(check.inOrder.size() == n);
        for (std::uint32_t i = 0; i < n; ++i)
        {
            CHECK(check.inOrder[i] == i);
        }
        if (n > 0)
        {
            CHECK_FALSE(t.red[t.root]);
        }
    }
}

TEST_CASE("Builder rejects invalid trees", "[cfb][builder]")
{
    Builder b;
    const auto storage = b.addStorage(Builder::root(), u"Header-2");
    REQUIRE(storage);
    const auto stream = b.addStream(*storage, u"properties", std::vector<std::byte>{});
    REQUIRE(stream);

    CHECK_FALSE(b.addStream(*storage, u"PROPERTIES", std::vector<std::byte>{}));
    CHECK_FALSE(b.addStorage(*stream, u"child"));
    CHECK_FALSE(b.addStorage(Builder::root(), u"bad/name"));
    CHECK_FALSE(b.addStorage(Builder::root(), std::u16string(32, u'n')));
    CHECK_FALSE(b.addStorage(999, u"x"));
    CHECK_FALSE(b.addStream(Builder::root(), u"src", SourceStream{}));
}

TEST_CASE("Builder allows the same name under different storages", "[cfb][builder]")
{
    Builder b;
    const auto first = b.addStorage(Builder::root(), u"a").value();
    const auto second = b.addStorage(Builder::root(), u"b").value();
    CHECK(b.addStream(first, u"properties", std::vector<std::byte>{}));
    CHECK(b.addStream(second, u"Properties", std::vector<std::byte>{}));
    CHECK_FALSE(b.addStream(second, u"properties", std::vector<std::byte>{}));
}
