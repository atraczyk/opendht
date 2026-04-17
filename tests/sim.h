// Copyright (c) 2014-2026 Savoir-faire Linux Inc.
// SPDX-License-Identifier: MIT
#pragma once

/**
 * Deterministic simulation infrastructure for OpenDHT.
 *
 * Provides:
 *  - SimClock        : a virtual clock that advances only when the driver
 *                      explicitly calls step() or set().
 *  - SimDatagramSocket : a DatagramSocket implementation that captures all
 *                        outgoing packets (with virtual timestamps) and allows
 *                        packets to be injected from outside.
 *  - SimNetwork      : an in-process "network" that routes packets between
 *                      SimDatagramSocket instances with configurable latency.
 *  - TraceEntry / SimTrace : a flat, ordered log of every packet sent during
 *                            a simulation run, used to prove determinism.
 *  - SimConfig       : parameters for a simulation run.
 *  - runScenario()   : run a basic put/get scenario, returning the SimTrace.
 *
 * Usage:
 *   SimConfig cfg;
 *   cfg.seed = 42;
 *   cfg.use_sim_clock = true;
 *   auto trace1 = runScenario(cfg);
 *   auto trace2 = runScenario(cfg);
 *   assert(trace1 == trace2);   // deterministic
 */

#include <opendht/dhtrunner.h>
#include <opendht/network_utils.h>
#include <opendht/rng.h>

#include <functional>
#include <map>
#include <queue>
#include <vector>
#include <memory>
#include <cstdint>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <netinet/in.h>
#include <arpa/inet.h>
#endif

namespace dht {
namespace sim {

// ─── CRC-32 ──────────────────────────────────────────────────────────────────

static inline uint32_t
crc32(const uint8_t* data, size_t len)
{
    uint32_t crc = 0xFFFFFFFFu;
    while (len--) {
        crc ^= *data++;
        for (int i = 0; i < 8; ++i)
            crc = (crc >> 1) ^ (0xEDB88320u & -(crc & 1u));
    }
    return ~crc;
}

// ─── TraceEntry / SimTrace ───────────────────────────────────────────────────

/**
 * One entry in the simulation event log: a single packet sent by a node.
 * The virtual timestamp, source/destination addresses, and a CRC of the
 * payload are recorded.  CRC avoids storing raw msgpack blobs while still
 * detecting any payload difference between runs.
 */
struct TraceEntry
{
    time_point vtime {};
    SockAddr   from {};
    SockAddr   to {};
    uint32_t   crc {0};

    bool operator==(const TraceEntry& o) const
    {
        return vtime == o.vtime && from == o.from && to == o.to && crc == o.crc;
    }
    bool operator!=(const TraceEntry& o) const { return !(*this == o); }
};

using SimTrace = std::vector<TraceEntry>;

// ─── SimClock ─────────────────────────────────────────────────────────────────

/**
 * A monotonically advancing virtual clock.
 * The simulation driver advances it explicitly; nodes never call clock::now()
 * once the clock function is installed.
 */
class SimClock
{
public:
    /**
     * Start the virtual clock at a fixed, deterministic epoch
     * (1 second past the steady_clock origin) so that two simulation runs
     * always begin at the same virtual time regardless of when they execute.
     *
     * This works because Dht::Dht() now calls scheduler.setClock() before
     * scheduler.syncTime(), so all internal timers are anchored to this
     * virtual start point rather than to wall-clock time.
     */
    SimClock() : now_(time_point {} + std::chrono::seconds(1)) {}

    time_point now() const { return now_; }

    void set(time_point t)
    {
        if (t > now_)
            now_ = t;
    }

    void advance(duration d)
    {
        if (d > duration::zero())
            now_ += d;
    }

    /** Returns a std::function suitable for DhtRunner::Context::clock. */
    std::function<time_point()> fn()
    {
        return [this] { return now_; };
    }

private:
    time_point now_;
};

// ─── Forward declaration ──────────────────────────────────────────────────────

class SimNetwork;

// ─── SimDatagramSocket ────────────────────────────────────────────────────────

/**
 * An in-process DatagramSocket that:
 *  - records every sendTo() call as a TraceEntry using a configurable
 *    time source (virtual for deterministic traces, real for wall-clock traces),
 *  - enqueues outgoing packets in the owning SimNetwork,
 *  - accepts injected incoming packets via inject().
 */
class SimDatagramSocket : public net::DatagramSocket
{
public:
    /**
     * @param vtime_fn  Time source for TraceEntry timestamps.
     *                  Pass clock.fn() for reproducible virtual timestamps;
     *                  pass {} (or a real-clock lambda) to record wall-clock
     *                  timestamps so two runs are distinguishably different.
     */
    SimDatagramSocket(SockAddr bound, SimNetwork& net, SimClock& clock, SimTrace& trace,
                      std::function<time_point()> vtime_fn = {})
        : net_(net)
        , clock_(clock)
        , trace_(trace)
        , bound_(std::move(bound))
        , vtime_fn_(vtime_fn ? std::move(vtime_fn) : []{ return clock::now(); })
    {}

    int sendTo(const SockAddr& dest, const uint8_t* data, size_t size, bool replied) override;

    /** Called by SimNetwork to deliver an incoming packet. */
    void inject(Blob data, SockAddr from)
    {
        net::PacketList pkts;
        pkts.push_back({std::move(data), std::move(from), clock_.now()});
        onReceived(std::move(pkts));
    }

    bool hasIPv4() const override { return bound_.getFamily() == AF_INET; }
    bool hasIPv6() const override { return bound_.getFamily() == AF_INET6; }
    const SockAddr& getBoundRef(sa_family_t = AF_UNSPEC) const override { return bound_; }
    void stop() override {}

private:
    SimNetwork& net_;
    SimClock&   clock_;
    SimTrace&   trace_;
    SockAddr    bound_;
    std::function<time_point()> vtime_fn_;
};

// ─── SimNetwork ───────────────────────────────────────────────────────────────

/**
 * Routes packets between SimDatagramSocket instances with a fixed latency.
 * Packets are held in a priority queue and delivered when the simulation
 * clock advances past their delivery time.
 */
class SimNetwork
{
public:
    explicit SimNetwork(duration latency = std::chrono::milliseconds(5))
        : latency_(latency)
    {}

    void registerSocket(SockAddr addr, SimDatagramSocket* sock)
    {
        sockets_[std::move(addr)] = sock;
    }

    void enqueue(SockAddr from, SockAddr to, Blob data, time_point now)
    {
        queue_.push({now + latency_, std::move(from), std::move(to), std::move(data)});
    }

    /** Deliver all packets whose delivery time <= now. */
    void deliverDue(time_point now)
    {
        while (!queue_.empty() && queue_.top().deliver_at <= now) {
            auto pkt = std::move(const_cast<InFlight&>(queue_.top()));
            queue_.pop();
            auto it = sockets_.find(pkt.to);
            if (it != sockets_.end())
                it->second->inject(std::move(pkt.data), std::move(pkt.from));
        }
    }

    bool hasPending() const { return !queue_.empty(); }

    time_point nextDelivery() const
    {
        return queue_.empty() ? time_point::max() : queue_.top().deliver_at;
    }

private:
    struct InFlight
    {
        time_point deliver_at;
        SockAddr   from, to;
        Blob       data;
        bool operator>(const InFlight& o) const { return deliver_at > o.deliver_at; }
    };
    using PQ = std::priority_queue<InFlight, std::vector<InFlight>, std::greater<InFlight>>;

    PQ                              queue_;
    std::map<SockAddr, SimDatagramSocket*> sockets_;
    duration                        latency_;
};

// ─── SimDatagramSocket::sendTo — defined after SimNetwork ────────────────────

inline int
SimDatagramSocket::sendTo(const SockAddr& dest, const uint8_t* data, size_t size, bool /*replied*/)
{
    trace_.push_back({vtime_fn_(), bound_, dest, crc32(data, size)});
    net_.enqueue(bound_, dest, {data, data + size}, clock_.now());
    return 0;
}

// ─── SimConfig ────────────────────────────────────────────────────────────────

struct SimConfig
{
    uint64_t                    seed              {42};
    bool                        use_sim_clock     {true};
    std::chrono::milliseconds   latency           {5};
    size_t                      node_count        {6};

    /**
     * Staggered bootstrap: add at most this many nodes per batch before
     * letting the network run for bootstrap_interval of virtual time.
     * Each new node bootstraps through a randomly chosen already-running node
     * rather than always through node 0, which avoids flooding a single entry
     * point and produces more realistic routing-table diversity.
     *
     * Set to 0 (default) to add all nodes simultaneously (original behaviour).
     */
    size_t                      bootstrap_batch   {0};

    /** Virtual time to run between bootstrap batches (ignored when batch==0). */
    std::chrono::milliseconds   bootstrap_interval{std::chrono::seconds(2)};
};

// ─── Helpers ─────────────────────────────────────────────────────────────────

static inline SockAddr
makeLoopbackAddr(in_port_t port)
{
    SockAddr addr;
    addr.setFamily(AF_INET);
    addr.setLoopback();
    addr.setPort(port);
    return addr;
}

// ─── runScenario ─────────────────────────────────────────────────────────────

/**
 * Run a basic put/get simulation scenario and return the packet trace.
 *
 * Scenario:
 *   1. Create node_count DhtRunner nodes (threaded=false, simulated socket).
 *   2. Bootstrap nodes 1..N-1 to node 0.
 *   3. Step the simulation until a put from node N/2 and a get from node 0
 *      both complete, or the virtual clock exceeds 120 s.
 *
 * With use_sim_clock=true and the same seed, two calls always return an
 * identical SimTrace.  With use_sim_clock=false the trace timestamps come
 * from the wall clock and differ between runs.
 */
static SimTrace
runScenario(SimConfig cfg)
{
    SimClock   clock;
    SimNetwork net(cfg.latency);
    SimTrace   trace;

    std::mt19937_64 master_rng(cfg.seed);

    const size_t N = cfg.node_count;
    static constexpr in_port_t BASE_PORT = 42220;

    // ── Create and start nodes ────────────────────────────────────────────────
    std::vector<std::unique_ptr<DhtRunner>> nodes(N);

    // With the virtual clock: trace records SimClock time → identical both runs.
    // Without it: trace records real clock::now() → timestamps differ each run,
    // proving the virtual clock is what makes record/replay possible.
    std::function<time_point()> vtime_fn = cfg.use_sim_clock
        ? clock.fn()
        : std::function<time_point()>([]{ return clock::now(); });

    for (size_t i = 0; i < N; ++i) {
        auto addr = makeLoopbackAddr(static_cast<in_port_t>(BASE_PORT + i));
        auto sock = std::make_unique<SimDatagramSocket>(addr, net, clock, trace, vtime_fn);
        net.registerSocket(addr, sock.get());

        DhtRunner::Config config;
        config.threaded                              = false;
        config.dht_config.node_config.max_peer_req_per_sec = -1;
        config.dht_config.node_config.max_req_per_sec      = -1;
        config.dht_config.node_config.max_store_size       = -1;
        config.dht_config.node_config.max_store_keys       = -1;

        DhtRunner::Context context;
        context.sock = std::move(sock);
        context.rng  = std::make_unique<std::mt19937_64>(
            crypto::getDerivedRandomEngine(master_rng));
        if (cfg.use_sim_clock)
            context.clock = clock.fn();

        nodes[i] = std::make_unique<DhtRunner>();
        nodes[i]->run(config, std::move(context));
    }

    // ── Bootstrap: nodes 1..N-1 → node 0 ────────────────────────────────────
    auto node0addr = makeLoopbackAddr(BASE_PORT);
    for (size_t i = 1; i < N; ++i)
        nodes[i]->bootstrap(node0addr);

    // ── Simulation driver ─────────────────────────────────────────────────────
    auto deadline = clock.now() + std::chrono::seconds(120);

    bool put_done  = false;
    bool get_done  = false;

    // Only step nodes that have been started so far (grows as batches join).
    size_t active_nodes = 1; // node 0 is always first

    auto step = [&]() -> time_point {
        net.deliverDue(clock.now());
        time_point next = time_point::max();
        for (size_t i = 0; i < active_nodes; ++i) {
            auto w = nodes[i]->loop();
            if (w < next)
                next = w;
        }
        return next;
    };

    auto runUntil = [&](time_point until) {
        while (clock.now() < until) {
            auto next     = step();
            auto net_next = net.nextDelivery();
            auto advance_to = std::min({next, net_next, until});
            if (advance_to <= clock.now())
                advance_to = clock.now() + std::chrono::milliseconds(1);
            clock.set(advance_to);
        }
    };

    // ── Bootstrap ─────────────────────────────────────────────────────────────
    const size_t batch = (cfg.bootstrap_batch == 0) ? (N - 1) : cfg.bootstrap_batch;

    // Use a separate RNG stream for target selection so it doesn't perturb
    // the master_rng used for node identity (preserves determinism).
    std::mt19937_64 boot_rng(cfg.seed ^ 0x1234567890ABCDEFULL);

    for (size_t i = 1; i < N; ) {
        // Add up to `batch` nodes in this wave.
        size_t wave_end = std::min(i + batch, N);
        for (; i < wave_end; ++i) {
            // Pick a random already-active node as the entry point.
            size_t target = std::uniform_int_distribution<size_t>(0, active_nodes - 1)(boot_rng);
            auto entry = makeLoopbackAddr(static_cast<in_port_t>(BASE_PORT + target));
            nodes[i]->bootstrap(entry);
            ++active_nodes;
        }

        // Let this batch stabilise before the next wave (or before operations).
        runUntil(clock.now() + cfg.bootstrap_interval);
    }

    // Extra settling time after all nodes are in (total boot budget: 10 s).
    auto boot_deadline = clock.now() + std::chrono::seconds(10);
    runUntil(boot_deadline);

    // ── Put / Get ─────────────────────────────────────────────────────────────
    auto key = InfoHash::get("sim-test-key");
    Value val {reinterpret_cast<const uint8_t*>("hello"), 5};
    nodes[N / 2]->put(key, std::move(val), [&](bool ok) { if (ok) put_done = true; });
    nodes[0]->get(key,
                  GetCallback {[&](const std::vector<std::shared_ptr<Value>>&) {
                      get_done = true;
                      return false;
                  }},
                  DoneCallback {});

    // Run until both operations complete or deadline.
    while ((!put_done || !get_done) && clock.now() < deadline) {
        auto next     = step();
        auto net_next = net.nextDelivery();
        auto advance_to = std::min({next, net_next, deadline});
        if (advance_to <= clock.now())
            advance_to = clock.now() + std::chrono::milliseconds(1);
        clock.set(advance_to);
    }
    // One final step to flush any last responses.
    step();

    // Shut down nodes cleanly.
    for (auto& n : nodes)
        n->join();

    return trace;
}

} // namespace sim
} // namespace dht
