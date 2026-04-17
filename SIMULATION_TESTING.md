# Deterministic Simulation Testing in OpenDHT

## Overview

Deterministic simulation testing (DST) — also called "discrete-event simulation testing" or "simulation-based testing" — is a technique where a distributed system is run entirely inside a single process, with a **virtual clock**, **in-process network**, and **seeded pseudo-random sources**, so that every run with the same seed produces exactly the same sequence of events and outcomes. This allows bugs that normally require rare timing coincidences to be reproduced reliably, and entire protocol scenarios (churn, partition, replication, expiry) to be exercised at arbitrary time scales.

This document analyses what would be required to implement DST for OpenDHT.

---

## Current Architecture

```
DhtRunner          (public API — thread-safe, manages lifecycle & I/O thread)
  └── SecureDht    (GnuTLS crypto: signing, encryption, identity)
        └── Dht    (core Kademlia: routing, storage, find/get/put, Scheduler)
              └── NetworkEngine   (UDP socket abstraction, msgpack serialisation)
                        └── DatagramSocket  (virtual interface — real or mock)
```

### Threading Model

`DhtRunner` owns a single `dht_thread` that runs a tight loop:

```
loop_() {
    drain pending_ops queue
    drain rcv (received packet) queue → dht_->periodic(pkt, now)
    if no packets: dht_->periodic(nullptr, clock::now())
    cv.wait_until(lk, next_wakeup, has_work)
}
```

`UdpSocket` owns a separate `rcv_thread` that calls `poll()` on real OS sockets and pushes raw `ReceivedPacket` objects (with a wall-clock `received` timestamp) into `DhtRunner::rcv` under `sock_mtx`.

`ThreadPool::computation()` and `ThreadPool::io()` are global singletons used for crypto operations (key generation, signing, encryption) and proxy I/O.

### Time

All protocol timing is driven by `Scheduler` (`include/opendht/scheduler.h`):

```cpp
class Scheduler {
    time_point now { clock::now() };            // ← std::chrono::steady_clock
    std::multimap<time_point, Sp<Job>> timers;

    time_point syncTime() { return (now = clock::now()); }   // reads wall clock
    void       syncTime(const time_point& n) { now = n; }    // overridable!
    time_point run();                                          // fires due jobs
};
```

`clock` is `std::chrono::steady_clock` (defined in `include/opendht/utils.h`).

`DhtRunner::loop_()` calls `clock::now()` directly (bypassing `Scheduler::syncTime`) in at least three places to timestamp packets and determine staleness.

### Network Abstraction

`DatagramSocket` (`include/opendht/network_utils.h`) is already an abstract interface:

```cpp
class DatagramSocket {
    virtual int sendTo(const SockAddr&, const uint8_t*, size_t, bool) = 0;
    virtual bool hasIPv4() const = 0;
    virtual bool hasIPv6() const = 0;
    virtual void stop() = 0;
    using OnReceive = std::function<PacketList(PacketList&&)>;
    void setOnReceive(OnReceive&&);
};
```

`DhtRunner::Context` already accepts a `std::unique_ptr<net::DatagramSocket>` and `std::unique_ptr<std::mt19937_64>` from the caller, and `Dht` similarly accepts `std::unique_ptr<net::DatagramSocket>` and `std::unique_ptr<std::mt19937_64>`.

`test_networkengine.cpp` already exploits this with a `TestDatagramSocket` that records sent packets and calls `engine.processMessage()` directly — this is the nucleus of simulation infrastructure.

### Random Number Generation

`rng.h` provides `getSeededRandomEngine<T>()` (reads from `std::random_device`) and `getDerivedRandomEngine<T>(source)` (deterministic derivation). `Dht` stores an `std::mt19937_64 rd` and uses it for node ID randomisation, bucket exploration, and node selection. Each `DhtRunner::Context` can supply a pre-seeded `rng`.

`SecureDht`/`crypto.cpp` generate keys via GnuTLS, which draws entropy from the OS (`gnutls_rnd`, `std::random_device`). Token generation uses `gnutls_rnd(GNUTLS_RND_NONCE, ...)`.

---

## Sources of Non-Determinism

| # | Source | Location | Severity |
|---|--------|----------|----------|
| 1 | **Wall-clock reads** | `clock::now()` in `DhtRunner::loop_()` (×3) and `Scheduler::syncTime()` | Critical |
| 2 | **`rcv_thread` / `poll()` timing** | `UdpSocket::rcv_thread` (`network_utils.cpp`) | Critical |
| 3 | **`dht_thread` / `cv.wait_until`** | `DhtRunner` main loop | Critical |
| 4 | **RNG seeding from OS** | `getSeededRandomEngine()` → `std::random_device` | High |
| 5 | **GnuTLS entropy** | `gnutls_rnd()` in token generation and crypto | High |
| 6 | **Thread pool scheduling** | `ThreadPool::computation()` / `ThreadPool::io()` (global singletons) | Medium |
| 7 | **`std::random_device` in crypto.cpp** | Key/IV generation during `encrypt`/`sign` | Medium |
| 8 | **`ReceivedPacket::received` timestamp** | Set at socket receive time, used to drop stale packets | Medium |
| 9 | **Packet reordering / loss** | OS UDP stack; currently no drop/reorder simulation | Low (absent) |

---

## What Would Be Required

### 1. Virtual Clock (`SimClock`)

Replace the global `using clock = std::chrono::steady_clock` type alias with a policy-based or injectable clock:

**Option A — Clock Policy Template**
Parameterise `Scheduler<Clock>`, `Dht<Clock>`, `DhtRunner<Clock>`. Production code uses `std::chrono::steady_clock`; simulation uses a `SimClock` with a manually advanced `now`. This is invasive across the entire type hierarchy.

**Option B — Thread-Local / Injectable Clock (recommended)**
Introduce a `Clock` interface (or a global function pointer / `std::function`) behind a thin layer:

```cpp
// include/opendht/clock.h
namespace dht {
struct Clock {
    using time_point = std::chrono::steady_clock::time_point;
    using duration   = std::chrono::steady_clock::duration;

    virtual time_point now() const = 0;
    static Clock& get();          // returns *current* instance (default: WallClock)
    static void   set(Clock*);    // swap for simulation
};
} // namespace dht
```

All `clock::now()` calls in `dhtrunner.cpp` and `scheduler.h` are redirected to `Clock::get().now()`. In simulation, install a `SimClock` and advance time explicitly.

**Scope**: `include/opendht/utils.h`, `include/opendht/scheduler.h`, `src/dhtrunner.cpp` (3 sites), `src/network_engine.cpp` (request timeout checks), `src/dht.cpp` (indirect via `Scheduler::syncTime`).

### 2. In-Process Network (`SimNetwork`)

Build a message bus that connects multiple `Dht` / `DhtRunner` nodes without real sockets:

```cpp
class SimDatagramSocket : public net::DatagramSocket {
    SimNetwork& network_;
    SockAddr    bound_;
public:
    int sendTo(const SockAddr& dest, const uint8_t* data, size_t size, bool) override {
        network_.enqueue(bound_, dest, {data, data + size}, simClock.now());
        return 0;
    }
    void deliverTo(const uint8_t* data, size_t size, const SockAddr& from) {
        PacketList pkts;
        pkts.push_back({Blob(data, data+size), from, simClock.now()});
        onReceived(std::move(pkts));
    }
    // hasIPv4/hasIPv6/stop as needed
};

class SimNetwork {
    struct Packet { SockAddr from, to; Blob data; time_point at; };
    std::priority_queue<Packet, ...> queue_;  // ordered by delivery time
public:
    void enqueue(SockAddr from, SockAddr to, Blob data, time_point now,
                 duration latency = 5ms, float loss = 0.0f);
    void deliverDue(time_point now);          // called by simulation tick
    // Optional: partition(), setLatency(), setLoss()
};
```

`deliverDue()` is called by the simulation harness after each virtual clock step. Because the socket interface is already virtual, **no changes to `NetworkEngine` or `Dht` internals are needed**.

### 3. Single-Threaded Simulation Driver

Replace `DhtRunner`'s `dht_thread` + `rcv_thread` with a synchronous step function:

```cpp
class SimNode {
    SimDatagramSocket* sock_;
    DhtRunner          runner_;   // Config::threaded = false
public:
    time_point step(time_point now) {
        // 1. Inject any packets delivered by SimNetwork up to `now`
        // 2. runner_.loop_()  (calling with virtual `now`)
        // 3. Return next wakeup from Scheduler
    }
};

class SimDriver {
    SimNetwork        net_;
    std::vector<SimNode> nodes_;
    time_point        now_;
public:
    void runUntil(time_point deadline) {
        while (now_ < deadline) {
            time_point next = time_point::max();
            net_.deliverDue(now_);
            for (auto& n : nodes_)
                next = std::min(next, n.step(now_));
            now_ = std::min({next, deadline, now_ + MAX_STEP});
        }
    }
};
```

`DhtRunner` already supports `Config::threaded = false` which skips spawning `dht_thread` and lets the caller drive `loop_()`.

**Required change**: expose a `loop_(time_point now)` overload (or a way to inject `now` without reading the wall clock), so that `loop_()` uses the virtual clock rather than `clock::now()`.

### 4. Deterministic RNG

The `DhtRunner::Context::rng` injection point already exists. For simulation:

```cpp
context.rng = std::make_unique<std::mt19937_64>(seed + node_index);
```

All Kademlia-layer randomness (bucket exploration, node selection, ID generation) is then deterministic.

**Remaining issue**: `getSeededRandomEngine()` in `rng.h` uses `std::random_device`, but this is only called as a fallback when no `rng` is supplied. Supplying one always is sufficient.

### 5. Crypto / GnuTLS Entropy

GnuTLS uses OS entropy internally (`gnutls_rnd`). Fully deterministic crypto would require:

- **Mock identities**: pre-generate key material offline with fixed seeds, store as fixtures, load in simulation. `DhtRunner::Config::dht_config.id` already accepts a `crypto::Identity`.
- **Token generation**: `crypto.cpp` uses `gnutls_rnd` for nonces; replacing with seeded RNG requires patching `generateToken` / `isTokenValid`.
- **Scope**: Moderate — only relevant if the test exercises signed/encrypted values; unsigned `dht::Value` blobs sidestep this entirely.

### 6. Thread Pool

`ThreadPool::computation()` and `ThreadPool::io()` are global singletons. In simulation, crypto operations (signing, encryption) must complete synchronously before the next step is taken, otherwise their callbacks may fire out-of-order.

Options:
- **Bypass threadpool**: In simulation mode, execute crypto callbacks inline on the calling thread. The pool already accepts `std::function<void()>`, so a `SimThreadPool` that runs jobs immediately is straightforward.
- **Wait for completion**: Call `ThreadPool::computation().waitAll()` between simulation steps.

---

## Existing Infrastructure That Helps

| Asset | File | Notes |
|-------|------|-------|
| `DatagramSocket` abstract interface | `include/opendht/network_utils.h` | Already virtual; `sendTo`, `stop`, `hasIPv4/6` |
| `TestDatagramSocket` | `tests/test_networkengine.cpp` | Captures sent packets; basis for `SimDatagramSocket` |
| `DhtRunner::Context::sock` | `include/opendht/dhtrunner.h` | Inject custom socket per node |
| `DhtRunner::Context::rng` | `include/opendht/dhtrunner.h` | Inject seeded RNG |
| `DhtRunner::Config::threaded = false` | `include/opendht/dhtrunner.h` | Disable background thread; caller drives loop |
| `Scheduler::syncTime(time_point)` | `include/opendht/scheduler.h` | Override scheduler's `now` without wall clock |
| `NetworkEngine::processMessage()` | `include/opendht/network_engine.h` | Inject raw packets directly |
| `getDerivedRandomEngine()` | `include/opendht/rng.h` | Derive child RNGs deterministically |

---

## Existing Infrastructure That Helps — Detailed Notes

### `Dht::periodic()` Already Accepts `time_point`

```cpp
// dht.h
time_point periodic(const uint8_t* buf, size_t buflen, SockAddr, const time_point& now);
```

This is the core processing entry point and **already accepts `now` as a parameter**. `DhtRunner::loop_()` passes `clock::now()` to it, but a simulation driver can pass a virtual `time_point` instead. This is the most important seam already present in the codebase.

### `DhtRunner::Config::threaded = false`

Setting `threaded = false` prevents `DhtRunner` from spawning its `dht_thread`. The caller is then responsible for driving the loop via `DhtRunner::loop()`. This is the correct entry point for a deterministic driver — the main remaining issue is that `loop_()` still calls `clock::now()` internally at three sites.

### `Scheduler::syncTime(time_point)`

```cpp
inline void syncTime(const time_point& n) { now = n; }
```

This overload exists and works correctly. The issue is that `Scheduler::syncTime()` (no arguments) reads `clock::now()` and is called from `dht.cpp` at the start of several methods. These call sites would need to be changed to use the passed-in time instead.

---

## Estimated Scope of Work

| Area | Effort | Risk |
|------|--------|------|
| Virtual clock injection (replace `clock::now()` call sites in `dhtrunner.cpp`) | Small (3 sites) | Low |
| `Scheduler::syncTime` call sites in `dht.cpp` | Small–Medium (~10 sites) | Low |
| `SimDatagramSocket` + `SimNetwork` | Medium (new files, ~250–350 LOC) | Low |
| `SimDriver` harness + simulation loop | Medium (~150–200 LOC) | Low |
| Deterministic RNG (already injectable) | Trivial | None |
| Crypto fixtures / mock identities | Medium | Medium |
| Thread pool synchronous mode | Medium | Medium |
| Full GnuTLS entropy control | Large | High |
| Integration tests using the harness | Medium | Low |

**Total for a minimal viable simulation** (unsigned values, no crypto, N-node put/get/listen, controllable latency and churn): approximately **1 000–1 400 new/modified lines**, spread over 2–3 weeks of focused work.

A **minimal viable simulation** — one that covers unsigned DHT put/get/listen across N nodes with controllable churn and latency, without crypto — can be achieved with the first five items. The crypto items can be deferred or worked around with pre-generated fixture identities.

---

## Recommended Implementation Order

1. **Introduce `SimClock`** — injectable clock with a `set/get` mechanism. Replace all `clock::now()` call sites in `dhtrunner.cpp`.
2. **Wire `Scheduler::syncTime` to `SimClock`** — already accepts `time_point` by value.
3. **Implement `SimDatagramSocket`** — wraps `SimNetwork`, records and delivers packets.
4. **Implement `SimNetwork`** — priority-queue of in-flight packets keyed by delivery time; supports latency, loss, partition.
5. **Implement `SimDriver`** — advances virtual clock, steps all `SimNode`s in round-robin, drains `SimNetwork`.
6. **Write first simulation test** — 3-node bootstrap, put/get roundtrip, verify result deterministically.
7. **Add pre-generated fixture identities** — remove `std::random_device` dependency for test runs that exercise signing.
8. **Make `ThreadPool` synchronous in simulation** — run callbacks inline or add `waitAll()` barriers between steps.

---

## References

- `include/opendht/scheduler.h` — `Scheduler` class with injectable `syncTime`
- `include/opendht/network_utils.h` — `DatagramSocket` abstract interface
- `include/opendht/dhtrunner.h` — `Config::threaded`, `Context::sock`, `Context::rng`
- `include/opendht/rng.h` — `getDerivedRandomEngine`, `getSeededRandomEngine`
- `include/opendht/utils.h` — `clock` type alias (`std::chrono::steady_clock`)
- `src/dhtrunner.cpp` — `loop_()` main loop, `clock::now()` call sites
- `tests/test_networkengine.cpp` — `TestDatagramSocket`, existing mock socket pattern
