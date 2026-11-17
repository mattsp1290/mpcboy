// Smoke test — proves the tests/Makefile + doctest vendor drop compile
// and run against a plain host toolchain (no libDaisy, no Rack SDK). This
// is intentionally minimal: it does not touch any real rackboy_core code
// yet (that code doesn't exist), but it guarantees `make -C tests test`
// stays green as the scaffold fills in. Replace or expand when the first
// real core unit test lands.

#include "doctest/doctest.h"

TEST_CASE("tests/core scaffold smoke test")
{
    // Use CHECK_EQ (not CHECK(a == b)) so future authors copy-pasting
    // this file as a template get doctest's two-operand diagnostic
    // output on failure. See doctest-cpp-testing-framework research.
    CHECK_EQ(1 + 1, 2);
    CHECK_FALSE(false);

    SUBCASE("doctest SUBCASE works")
    {
        int accumulator = 0;
        for(int i = 0; i < 10; ++i)
        {
            accumulator += i;
        }
        CHECK_EQ(accumulator, 45);
    }
}
