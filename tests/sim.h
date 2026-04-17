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
 *  - records every sendTo() call as a TraceEntry (with the virtual timestamp),
 *  - enqueues outgoing packets in the owning SimNetwork,
 *  - accepts injected incoming packets via inject().
 */
class SimDatagramSocket : public net::DatagramSocket
{
public:
    SimDatagramSocket(SockAddr bound, SimNetwork& net, SimClock& clock, SimTrace& trace)
        : net_(net)
        , clock_(clock)
        , trace_(trace)
        , bound_(std::move(bound))
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
    trace_.push_back({clock_.now(), bound_, dest, crc32(data, size)});
    net_.enqueue(bound_, dest, {data, data + size}, clock_.now());
    return 0;
}

// ─── SimConfig ────────────────────────────────────────────────────────────────

struct SimConfig
{
    uint64_t                    seed          {42};
    bool                        use_sim_clock {true};
    std::chrono::milliseconds   latency       {5};
    size_t                      node_count    {6};
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

    for (size_t i = 0; i < N; ++i) {
        auto addr = makeLoopbackAddr(static_cast<in_port_t>(BASE_PORT + i));
        auto sock = std::make_unique<SimDatagramSocket>(addr, net, clock, trace);
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

    auto step = [&]() -> time_point {
        net.deliverDue(clock.now());
        time_point next = time_point::max();
        for (auto& n : nodes) {
            auto w = n->loop();
            if (w < next)
                next = w;
        }
        return next;
    };

    // Give nodes 10 s of virtual time to bootstrap before issuing operations.
    auto boot_deadline = clock.now() + std::chrono::seconds(10);
    while (clock.now() < boot_deadline) {
        auto next = step();
        auto net_next = net.nextDelivery();
        auto advance_to = std::min({next, net_next, boot_deadline});
        if (advance_to <= clock.now())
            advance_to = clock.now() + std::chrono::milliseconds(1);
        clock.set(advance_to);
    }

    // Post put from the middle node, get from node 0.
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
        auto next = step();
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
