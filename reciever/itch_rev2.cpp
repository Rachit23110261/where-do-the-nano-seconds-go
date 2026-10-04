// itch_recv2.cpp - two-thread UDP receiver for itch_replay packets.
//
//   Thread A (rx):     recv -> validate -> seq check -> push Slot into SPSC
//   Thread B (parse):  pop Slot -> decode ITCH -> count types
//
// Packet: [uint64 seq][uint16 len BE][ITCH msg (len bytes)]
// Build:  g++ -O2 -std=c++20 -Wall -Wextra -pthread -o itch_recv2 itch_recv2.cpp
// Usage:  ./itch_recv2 <port> [--rcvbuf BYTES] [--rx-cpu C] [--parse-cpu C] [--mlock]

#include "../spsc/spsc.hpp"
#include "../orderbook/orderbook.hpp"
#include <arpa/inet.h>
#include <endian.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <memory>
#include <string>
#include <thread>

#if defined(__x86_64__)
#include <x86intrin.h>
#endif

namespace {

constexpr std::size_t kMaxMsg   = 54;        // largest ITCH 5.0 msg is 50 bytes
constexpr std::size_t kQueueCap = 1 << 16;   // 65536 slots * 64 B = 4 MB

// One queue slot = exactly one cache line.
struct alignas(64) Slot {
    uint64_t t_rx;            // timestamp right after recv (used by flight recorder in Step 8)
    uint16_t len;             // ITCH message length
    uint8_t  msg[kMaxMsg];    // ITCH message, starts with type byte
};
static_assert(sizeof(Slot) == 64, "Slot must be one cache line");

using Queue = SPSCQueue<Slot, kQueueCap>;

[[noreturn]] void die(const char* s) { std::perror(s); std::exit(1); }

inline uint64_t now_ns() {
    timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return uint64_t(ts.tv_sec) * 1'000'000'000ull + uint64_t(ts.tv_nsec);
}

inline uint64_t tsc() {
#if defined(__x86_64__)
    return __rdtsc();
#else
    return now_ns();
#endif
}

inline void cpu_relax() {
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__)
    asm volatile("yield");
#endif
}

void pin_thread(std::thread& t, int cpu) {
    if (cpu < 0) return;
    cpu_set_t set; CPU_ZERO(&set); CPU_SET(cpu, &set);
    if (int rc = pthread_setaffinity_np(t.native_handle(), sizeof set, &set))
        std::fprintf(stderr, "pthread_setaffinity_np(cpu %d) failed: %s\n", cpu, std::strerror(rc));
}

inline uint64_t be64(const uint8_t* p) { uint64_t v; std::memcpy(&v, p, 8); return be64toh(v); }
inline uint32_t be32(const uint8_t* p) { uint32_t v; std::memcpy(&v, p, 4); return be32toh(v); }

struct Args {
    uint16_t port = 0;
    int rcvbuf = 0, rx_cpu = -1, parse_cpu = -1;
    bool mlock = false;
};

Args parse_args(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <port> [--rcvbuf N] [--rx-cpu C] [--parse-cpu C] [--mlock]\n", argv[0]);
        std::exit(2);
    }
    Args a; a.port = uint16_t(std::stoul(argv[1]));
    for (int i = 2; i < argc; ++i) {
        std::string o = argv[i];
        if (o == "--mlock") { a.mlock = true; continue; }
        if (i + 1 >= argc) { std::fprintf(stderr, "missing value for %s\n", o.c_str()); std::exit(2); }
        if      (o == "--rcvbuf")    a.rcvbuf    = std::stoi(argv[++i]);
        else if (o == "--rx-cpu")    a.rx_cpu    = std::stoi(argv[++i]);
        else if (o == "--parse-cpu") a.parse_cpu = std::stoi(argv[++i]);
        else { std::fprintf(stderr, "unknown option %s\n", o.c_str()); std::exit(2); }
    }
    return a;
}

int open_socket(const Args& a) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) die("socket");
    if (a.rcvbuf > 0 && setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &a.rcvbuf, sizeof a.rcvbuf)) die("SO_RCVBUF");
    int actual = 0; socklen_t sl = sizeof actual;
    getsockopt(fd, SOL_SOCKET, SO_RCVBUF, &actual, &sl);
    std::printf("SO_RCVBUF actual = %d bytes\n", actual);

    timeval tv{2, 0};                                   // 2 s idle = replayer finished
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(a.port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr)) die("bind");
    return fd;
}

// ---- Thread A: network receive ------------------------------------------------
struct RxStats {
    uint64_t received = 0, lost = 0, reordered = 0, malformed = 0, expected = 0;
    uint64_t first_seq = UINT64_MAX;
    uint64_t queue_full = 0;          // pushes that found the queue full at least once
    uint64_t max_depth = 0;           // max observed queue occupancy
    uint64_t t_first = 0, t_last = 0;
};

void rx_loop(int fd, Queue& q, std::atomic<bool>& done, RxStats& s) {
    alignas(64) uint8_t buf[2048];
    Slot slot;
    for (;;) {
        ssize_t r = recv(fd, buf, sizeof buf, 0);
        if (r < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) { if (s.received) break; continue; }
            die("recv");
        }
        const uint64_t t = tsc();

        if (r < 11) { ++s.malformed; continue; }
        const uint16_t n = uint16_t((buf[8] << 8) | buf[9]);
        if (size_t(r) != 10u + n || n > kMaxMsg) { ++s.malformed; continue; }

        uint64_t seq; std::memcpy(&seq, buf, 8);
        if (s.first_seq == UINT64_MAX) {
            s.first_seq = seq;
            s.t_first = now_ns();
            if (seq != 0)
                std::fprintf(stderr, "WARNING: first seq = %llu (receiver started late?)\n",
                             (unsigned long long)seq);
        }
        if (seq >= s.expected) { s.lost += seq - s.expected; s.expected = seq + 1; }
        else { ++s.reordered; if (s.lost) --s.lost; }

        slot.t_rx = t;
        slot.len  = n;
        std::memcpy(slot.msg, buf + 10, n);

        if (!q.try_push(slot)) {                 // backpressure: parser is behind
            ++s.queue_full;
            while (!q.try_push(slot)) cpu_relax();
        }
        if ((s.received & 1023) == 0) {          // sample depth cheaply
            uint64_t d = q.size_approx();
            if (d > s.max_depth) s.max_depth = d;
        }
        ++s.received;
    }
    s.t_last = now_ns();
    done.store(true, std::memory_order_release);   // after the last push
}

// ---- Thread B: parse --------------------------------------------------------
struct ParseStats {
    std::array<uint64_t, 256> counts{};
    uint64_t parsed = 0;
    std::unique_ptr<itch::OrderBook> book = std::make_unique<itch::OrderBook>();
    uint64_t checksum = 0;     // keeps the decode from being optimised away
};

inline void decode(const Slot& s, ParseStats& p) {
  p.counts[s.msg[0]]++;
  p.book->apply(s.msg);
  ++p.parsed;
}

void parse_loop(Queue& q, std::atomic<bool>& done, ParseStats& p) {
    Slot s;
    for (;;) {
        if (q.try_pop(s)) { decode(s, p); continue; }
        if (done.load(std::memory_order_acquire)) {     // producer finished: drain the rest
            while (q.try_pop(s)) decode(s, p);
            break;
        }
        cpu_relax();
    }
}

}  // namespace

int main(int argc, char** argv) {
    Args a = parse_args(argc, argv);
    if (a.mlock && mlockall(MCL_CURRENT | MCL_FUTURE) != 0)
        std::perror("mlockall (continuing without it)");

    int fd = open_socket(a);
    // 4 MB: heap, not stack. make_unique value-initialises (zero-fills) the
    // object, which touches every page now instead of page-faulting in the hot loop.
    auto q = std::make_unique<Queue>();

    std::atomic<bool> done{false};
    RxStats rs; ParseStats ps;

    std::printf("listening on 127.0.0.1:%u\n", a.port);
    std::fflush(stdout);

    std::thread parser(parse_loop, std::ref(*q), std::ref(done), std::ref(ps));
    pin_thread(parser, a.parse_cpu);
    std::thread rx(rx_loop, fd, std::ref(*q), std::ref(done), std::ref(rs));
    pin_thread(rx, a.rx_cpu);

    rx.join();
    parser.join();
    close(fd);

    const double secs = (rs.t_last - rs.t_first) / 1e9;
    std::printf("received   = %llu\n", (unsigned long long)rs.received);
    std::printf("parsed     = %llu %s\n", (unsigned long long)ps.parsed,
                ps.parsed == rs.received ? "(== received, OK)" : "(MISMATCH!)");
    std::printf("lost       = %llu (%.4f%%)\n", (unsigned long long)rs.lost,
                rs.expected ? 100.0 * rs.lost / rs.expected : 0.0);
    std::printf("reordered  = %llu\nmalformed  = %llu\n",
                (unsigned long long)rs.reordered, (unsigned long long)rs.malformed);
    std::printf("queue_full = %llu   max_depth ~ %llu / %zu\n",
                (unsigned long long)rs.queue_full, (unsigned long long)rs.max_depth, kQueueCap);
    std::printf("rate       = %.0f msg/s (incl. 2 s idle tail: %.3f s)\n",
                secs > 2 ? rs.received / (secs - 2) : 0.0, secs);
    const auto& b = ps.book->stats();
    std::printf("book: updates=%llu missing_ref=%llu over_exec=%llu dup_ref=%llu crossed=%llu live=%zu\n",
    (unsigned long long)b.applied, (unsigned long long)b.missing_ref,
    (unsigned long long)b.over_exec, (unsigned long long)b.dup_ref,
    (unsigned long long)b.crossed, ps.book->live_orders());
    for (int c = 0; c < 256; ++c)
        if (ps.counts[c]) std::printf("%c=%llu\n", c, (unsigned long long)ps.counts[c]);
}

