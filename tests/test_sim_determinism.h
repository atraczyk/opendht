// Copyright (c) 2014-2026 Savoir-faire Linux Inc.
// SPDX-License-Identifier: MIT
#pragma once

#include <cppunit/TestFixture.h>
#include <cppunit/extensions/HelperMacros.h>

namespace test {

class SimDeterminismTester : public CppUnit::TestFixture
{
    CPPUNIT_TEST_SUITE(SimDeterminismTester);
    CPPUNIT_TEST(testDeterminismToggle);
    CPPUNIT_TEST_SUITE_END();

public:
    void setUp() override {}
    void tearDown() override {}

    /**
     * Verifies three properties:
     *
     *  1. Same seed + sim clock  → identical packet traces across two runs
     *     (proves determinism is real).
     *
     *  2. Different seed + sim clock → different packet traces
     *     (proves the seed actually drives behaviour, not a no-op).
     *
     *  3. Same seed + wall clock → different virtual timestamps
     *     (proves the sim-clock toggle changes observable behaviour;
     *     without it, timestamps come from clock::now() and are unique
     *     to each run).
     */
    void testDeterminismToggle();
};

} // namespace test
