// Copyright (c) 2014-2026 Savoir-faire Linux Inc.
// SPDX-License-Identifier: MIT
//
// sim_demo — interactive CLI for demonstrating OpenDHT deterministic simulation.
//
// Usage:
//   sim_demo [--sim-clock | --wall-clock] [--seed HEX] [--nodes N] [--latency MS]
//
// Examples:
//   sim_demo                          # default: sim clock, seed 0xdeadbeef
//   sim_demo --seed 0xcafebabe        # different seed → different (but still reproducible) trace
//   sim_demo --wall-clock             # disable virtual clock → non-deterministic timestamps

#include "sim.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <string>

using namespace dht::sim;
using namespace std::chrono;

static void usage(const char* prog) {
    printf("Usage: %s [--sim-clock | --wall-clock] [--seed HEX] [--nodes N] [--latency MS]\n\n"
           "  --sim-clock   Use virtual clock (default) — runs are byte-identical for same seed\n"
           "  --wall-clock  Use real wall clock — timestamps vary between runs\n"
           "  --seed HEX    RNG seed as a hex integer (default: 0xdeadbeef)\n"
           "  --nodes N     Number of DHT nodes (default: 4)\n"
           "  --latency MS  Simulated one-way latency in milliseconds (default: 5)\n",
           prog);
}

static void printTrace(const SimTrace& t) {
    printf("  %-22s  %-22s  %-12s  %s\n", "from", "to", "vtime(ms)", "crc32");
    printf("  %s\n", std::string(72, '-').c_str());
    for (auto& e : t) {
        auto ms = duration_cast<milliseconds>(e.vtime.time_since_epoch()).count();
        printf("  %-22s  %-22s  %-12lld  %08X\n",
               e.from.toString().c_str(),
               e.to.toString().c_str(),
               (long long)ms,
               e.crc);
    }
}

int main(int argc, char** argv) {
    SimConfig cfg;
    cfg.seed          = 0xdeadbeef;
    cfg.node_count    = 4;
    cfg.latency       = milliseconds(5);
    cfg.use_sim_clock = true;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--sim-clock") == 0) {
            cfg.use_sim_clock = true;
        } else if (strcmp(argv[i], "--wall-clock") == 0) {
            cfg.use_sim_clock = false;
        } else if (strcmp(argv[i], "--seed") == 0 && i + 1 < argc) {
            cfg.seed = (uint32_t)strtoul(argv[++i], nullptr, 0);
        } else if (strcmp(argv[i], "--nodes") == 0 && i + 1 < argc) {
            cfg.node_count = (int)strtoul(argv[++i], nullptr, 10);
        } else if (strcmp(argv[i], "--latency") == 0 && i + 1 < argc) {
            cfg.latency = milliseconds(strtoul(argv[++i], nullptr, 10));
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            usage(argv[0]);
            return 1;
        }
    }

    printf("\n");
    printf("┌─────────────────────────────────────────────────────────────────────┐\n");
    printf("│        OpenDHT  —  Deterministic Simulation Demo                   │\n");
    printf("└─────────────────────────────────────────────────────────────────────┘\n");
    printf("\n");
    printf("  clock : %s\n", cfg.use_sim_clock ? "virtual (sim clock)" : "real (wall clock)");
    printf("  seed  : 0x%08lX\n", (unsigned long)cfg.seed);
    printf("  nodes : %zu\n", (size_t)cfg.node_count);
    printf("  latency: %lld ms\n", (long long)duration_cast<milliseconds>(cfg.latency).count());
    printf("\n");

    printf("Running simulation  (run 1) …\n");
    auto trace1 = runScenario(cfg);
    printf("Running simulation  (run 2) …\n");
    auto trace2 = runScenario(cfg);

    printf("\n─── Run 1  (%zu packets) ──────────────────────────────────────────────\n", trace1.size());
    printTrace(trace1);

    printf("\n─── Run 2  (%zu packets) ──────────────────────────────────────────────\n", trace2.size());
    printTrace(trace2);

    bool identical = (trace1 == trace2);
    printf("\n");
    printf("  Traces identical across two runs?  %s\n",
           identical ? "YES ✅  (deterministic)" : "NO  ❌  (non-deterministic)");

    if (!identical) {
        // Find first differing entry
        size_t minSz = std::min(trace1.size(), trace2.size());
        for (size_t i = 0; i < minSz; ++i) {
            if (!(trace1[i] == trace2[i])) {
                auto ms1 = duration_cast<milliseconds>(trace1[i].vtime.time_since_epoch()).count();
                auto ms2 = duration_cast<milliseconds>(trace2[i].vtime.time_since_epoch()).count();
                printf("  First diff at packet %zu:\n", i);
                printf("    run1: vtime=%lld ms  crc=%08X\n", (long long)ms1, trace1[i].crc);
                printf("    run2: vtime=%lld ms  crc=%08X\n", (long long)ms2, trace2[i].crc);
                break;
            }
        }
        if (trace1.size() != trace2.size())
            printf("  Trace sizes differ: %zu vs %zu\n", trace1.size(), trace2.size());
    }

    if (!cfg.use_sim_clock) {
        // Compare against a sim-clock run with the same seed to show the contrast.
        SimConfig simCfg = cfg;
        simCfg.use_sim_clock = true;
        printf("\nRunning a sim-clock reference (same seed) for comparison …\n");
        auto simTrace = runScenario(simCfg);

        bool sameAsSim = (trace1 == simTrace);
        printf("\n─── Sim-clock reference  (%zu packets) ──────────────────────────────────\n",
               simTrace.size());
        printTrace(simTrace);

        printf("\n  Wall-clock trace (%zu packets) vs sim-clock trace (%zu packets):  %s\n",
               trace1.size(), simTrace.size(),
               sameAsSim ? "SAME (unexpected)" : "DIFFER ✅  (expected)");
        printf("\n  ℹ  Without the virtual clock the scheduler never advances past the\n"
               "     first bootstrap ping — only %zu packet(s) are exchanged instead of %zu.\n"
               "     Re-run with --sim-clock to get the full, reproducible simulation.\n",
               trace1.size(), simTrace.size());
    }

    printf("\n");
    return identical ? 0 : 1;
}
