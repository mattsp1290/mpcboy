// Shared doctest entry point for every per-module test binary in tests/.
//
// Defining DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN in exactly one translation
// unit per binary generates a standard `int main(int argc, char** argv)`
// that parses doctest command-line flags and runs all registered test
// cases linked into the binary. This file is the single translation unit
// that defines it; every other test .cpp in `tests/<module>/` only needs
// `#include "doctest/doctest.h"` (or the shortcut below) to register
// TEST_CASE / SUBCASE / CHECK assertions.
//
// See tests/Makefile for how this file is linked into each module's
// binary, and tests/third_party/doctest/SOURCE.md for the upstream pin.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"
