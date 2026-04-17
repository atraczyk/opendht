// Copyright (c) 2014-2026 Savoir-faire Linux Inc.
// SPDX-License-Identifier: MIT

#include "test_sim_determinism.h"
#include "sim.h"

namespace test {

CPPUNIT_TEST_SUITE_REGISTRATION(SimDeterminismTester);

void
SimDeterminismTester::testDeterminismToggle()
{
    using namespace dht::sim;

    // ── 1. Same seed + sim clock → identical traces ───────────────────────────
    SimConfig cfg;
    cfg.seed          = 0xdeadbeef;
    cfg.node_count    = 6;
    cfg.latency       = std::chrono::milliseconds(5);
    cfg.use_sim_clock = true;

    auto trace1 = runScenario(cfg);
    auto trace2 = runScenario(cfg);

    CPPUNIT_ASSERT_MESSAGE("Trace must be non-empty (simulation did real work)",
                           !trace1.empty());
    CPPUNIT_ASSERT_EQUAL_MESSAGE("Same seed + sim clock: trace sizes must match",
                                 trace1.size(), trace2.size());
    CPPUNIT_ASSERT_MESSAGE("Same seed + sim clock: traces must be byte-identical",
                           trace1 == trace2);

    // ── 2. Different seed → different trace ───────────────────────────────────
    cfg.seed = 0xcafebabe;
    auto trace3 = runScenario(cfg);

    CPPUNIT_ASSERT_MESSAGE("Different seed must produce a different trace",
                           trace1 != trace3);

    // ── 3. Wall clock → trace timestamps differ from sim-clock run ────────────
    // Without the injectable clock, nodes read clock::now() which advances
    // independently of any simulation seed.  The resulting timestamps will
    // not match those produced under the virtual clock (which started at a
    // fixed epoch point).
    cfg.seed          = 0xdeadbeef; // same seed as trace1
    cfg.use_sim_clock = false;
    auto trace4 = runScenario(cfg);

    CPPUNIT_ASSERT_MESSAGE(
        "Wall-clock run must differ from sim-clock run (timestamps come from clock::now())",
        trace1 != trace4);
}

} // namespace test
