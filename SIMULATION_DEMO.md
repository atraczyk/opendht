# Demonstrating OpenDHT Deterministic Simulation Testing

## Prerequisites

```sh
# Build the simulation test binary and demo tool (one-time)
cmake -S . -B build-sim \
  -DCMAKE_BUILD_TYPE=Debug \
  -DBUILD_TESTING=ON \
  -DOPENDHT_TOOLS=OFF \
  -GNinja
ninja -C build-sim test_sim_determinism sim_demo
```

---

## Demo 1 — Run the automated test suite

```sh
LD_LIBRARY_PATH=build-sim build-sim/test_sim_determinism
```

Expected output:
```
.

OK (1 tests)
```

The single test (`testDeterminismToggle`) exercises all three assertions automatically.

---

## Demo 2 — Interactive live demo (`sim_demo`)

`sim_demo` is a standalone tool with runtime flags — no source edits required.

### Deterministic run (default)

```sh
LD_LIBRARY_PATH=build-sim build-sim/sim_demo
```

Both runs produce **46 identical packets** with matching CRCs and virtual timestamps.

### Different seed — same determinism, different universe

```sh
LD_LIBRARY_PATH=build-sim build-sim/sim_demo --seed 0xcafebabe
```

The RNG seed drives which node gets probed first, so the packet sequence differs
from the default seed — but the two runs with `0xcafebabe` are still identical
to each other.

### Wall clock — scheduler stalls without virtual time

```sh
LD_LIBRARY_PATH=build-sim build-sim/sim_demo --wall-clock
```

Without the injectable virtual clock, `Scheduler::syncTime()` reads real
`clock::now()` which never advances during the tight simulation loop.
Only **3 bootstrap pings** go out instead of 46 — the simulation stalls.
The output also prints a sim-clock reference run side-by-side so the
difference is immediately visible.

### Full flag reference

```
sim_demo [--sim-clock | --wall-clock] [--seed HEX] [--nodes N] [--latency MS]

  --sim-clock   Virtual clock (default) — identical traces for same seed
  --wall-clock  Real clock — scheduler stalls, incomplete simulation
  --seed HEX    RNG seed (default: 0xdeadbeef)
  --nodes N     Number of DHT nodes (default: 4)
  --latency MS  Simulated one-way latency (default: 5 ms)
```

---

## Demo 3 — Toggle the seed in the test

Edit `tests/test_sim_determinism.cpp`:

```cpp
cfg.seed = 0xdeadbeef;   // ← change to any uint32_t
```

Rebuild and rerun:

```sh
ninja -C build-sim test_sim_determinism
LD_LIBRARY_PATH=build-sim build-sim/test_sim_determinism
```

The test still passes — traces are always identical for the **same** seed
and always different for a **different** seed.

---

## How it works (30-second explanation)

```
SimClock ──setClock()──► Scheduler::syncTime()   (virtual time, fixed epoch)
SimDatagramSocket ──────► DhtRunner rcv queue     (no real UDP, in-process)
SimNetwork::tick() ──────► loop_() per node       (single-threaded, ordered)
TraceEntry log ──────────► {vtime, from, to, crc} (per-packet fingerprint)
```

1. **`SimClock`** starts at a fixed epoch (1 s past the steady_clock origin) so
   both runs begin at exactly the same virtual time regardless of wall-clock time.
2. **`SimDatagramSocket`** routes packets through a central in-process broker —
   no real networking, no OS timing jitter.
3. **`DhtRunner::Config::threaded = false`** plus a single-threaded driver
   eliminate all thread-scheduling non-determinism.
4. The **RNG seed** is injected into `DhtRunner::Context::rng`, so token
   generation and node-ID assignment are fully reproducible.
