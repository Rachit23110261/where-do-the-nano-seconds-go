// itch_rev3.cpp - two-thread ITCH receiver + order book + FLIGHT RECORDER.
//
//   Thread A (rx):     recv -> t_rx=tsc() -> validate -> seq check -> push Slot
//   Thread B (parse):  pop -> t_deq=tsc() -> book.apply -> t_book=tsc()
//                      -> push Trace{t_rx,t_deq,t_book,type} into trace ring
//   Thread C (logger): drain trace ring -> big fwrite() batches -> trace file
//
// Hot-path rules: no printf, no file I/O, no allocation, never block on the
// trace ring (if it is full the record is dropped and counted).
//
// Stages (all in TSC cycles, converted to ns by the analyser):
//   queue  = t_deq  - t_rx    (copy + push + time waiting in SPSC + pop)
//   book   = t_book - t_deq   (order-book update)
//   total  = t_book - t_rx    (user-space receive -> book updated)
//
// Packet: [uint64 seq][uint16 len BE][ITCH msg]
// Build:  g++ -O2 -std=c++20 -Wall -Wextra -pthread -I../spsc -I../orderbook
//         -o itch_rev3 itch_rev3.cpp
// Usage:  ./itch_rev3 <port> [--rcvbuf N] [--rx-cpu C] [--parse-cpu C]
//                     [--log-cpu C] [--mlock] [--trace FILE] [--sample K]

#include "../spsc/spsc.hpp"
#include "../orderbook/orderbook.hpp"
#include "tsc.hpp"

#include <arpa/inet.h>
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
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr std::size_t kMaxMsg    = 55;          // largest ITCH 5.0 msg is 50 bytes
constexpr std::size_t kQueueCap  = 1 << 16;     // 65,536 x 64 B  = 4 MB
constexpr std::size_t kTraceCap  = 1 << 20;     // 1,048,576 x 32 B = 32 MB
constexpr std::size_t kLogBatch  = 1 << 16;     // records per fwrite

struct alignas(64) Slot {
    uint64_t t_rx;
    uint8_t  len;
    uint8_t  msg[kMaxMsg];
};
static_assert(sizeof(Slot) == 64, "Slot must be one cache line");

struct Trace {                                   // 32 bytes, matches analyser dtype
    uint64_t t_rx, t_deq, t_book;
    uint8_t  type;
    uint8_t  pad[7];
};
static_assert(sizeof(Trace) == 32, "Trace layout is a file format");

struct TraceHeader {                             // 32 bytes at file start
    char     magic[8];                           // "ITCHTRC1"
    double   cycles_per_ns;
    uint64_t records;                            // patched at end
    uint64_t dropped;                            // patched at end
};
static_assert(sizeof(TraceHeader) == 32);

using Queue      = SPSCQueue<Slot, kQueueCap>;
using TraceQueue = SPSCQueue<Trace, kTraceCap>;

[[noreturn]] void die(const char* s) { std::perror(s); std::exit(1); }

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
        std::fprintf(stderr, "pin cpu %d failed: %s\n", cpu, std::strerror(rc));
}

struct Args {
    uint16_t port = 0;
    int rcvbuf = 0, rx_cpu = -1, parse_cpu = -1, log_cpu = -1;
    bool mlock = false;
    std::string trace = "trace.bin";
    uint64_t sample = 1;                         // record every K-th message
};

Args parse_args(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <port> [--rcvbuf N] [--rx-cpu C] [--parse-cpu C] "
                             "[--log-cpu C] [--mlock] [--trace FILE] [--sample K]\n", argv[0]);
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
        else if (o == "--log-cpu")   a.log_cpu   = std::stoi(argv[++i]);
        else if (o == "--trace")     a.trace     = argv[++i];
        else if (o == "--sample")    a.sample    = std::max<uint64_t>(1, std::stoull(argv[++i]));
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
    timeval tv{2, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(a.port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr)) die("bind");
    return fd;
}

// ---- Thread A: receive ---------------------------------------------------------
struct RxStats {
    uint64_t received = 0, lost = 0, reordered = 0, malformed = 0, expected = 0;
    uint64_t first_seq = UINT64_MAX, queue_full = 0, max_depth = 0;
    uint64_t t_first = 0, t_last = 0;
};

void rx_loop(int fd, Queue& q, std::atomic<bool>& rx_done, RxStats& s) {
    alignas(64) uint8_t buf[2048];
    Slot slot;
    for (;;) {
        ssize_t r = recv(fd, buf, sizeof buf, 0);
        const uint64_t t = tscx::tsc();                         // T1: right after recv
        if (r < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) { if (s.received) break; continue; }
            die("recv");
        }
        if (r < 11) { ++s.malformed; continue; }
        const uint16_t n = uint16_t((buf[8] << 8) | buf[9]);
        if (size_t(r) != 10u + n || n > kMaxMsg) { ++s.malformed; continue; }

        uint64_t seq; std::memcpy(&seq, buf, 8);
        if (s.first_seq == UINT64_MAX) {
            s.first_seq = seq; s.t_first = tscx::mono_ns();
            if (seq != 0) std::fprintf(stderr, "WARNING: first seq = %llu (receiver started late?)\n",
                                       (unsigned long long)seq);
        }
        if (seq >= s.expected) { s.lost += seq - s.expected; s.expected = seq + 1; }
        else { ++s.reordered; if (s.lost) --s.lost; }

        slot.t_rx = t;
        slot.len  = uint8_t(n);
        std::memcpy(slot.msg, buf + 10, n);
        if (!q.try_push(slot)) { ++s.queue_full; while (!q.try_push(slot)) cpu_relax(); }
        if ((s.received & 1023) == 0) { uint64_t d = q.size_approx(); if (d > s.max_depth) s.max_depth = d; }
        ++s.received;
    }
    s.t_last = tscx::mono_ns();
    rx_done.store(true, std::memory_order_release);
}

// ---- Thread B: parse + book + trace -------------------------------------------------
struct ParseStats {
    std::array<uint64_t, 256> counts{};
    uint64_t parsed = 0, traced = 0, trace_dropped = 0;
    std::unique_ptr<itch::OrderBook> book = std::make_unique<itch::OrderBook>();
};

void parse_loop(Queue& q, TraceQueue& tq, std::atomic<bool>& rx_done,
                std::atomic<bool>& parse_done, uint64_t sample, ParseStats& p) {
    Slot s;
    uint64_t k = 0;
    auto handle = [&](const Slot& sl) {
        const uint64_t t_deq = tscx::tsc();                    // T2: after pop
        p.book->apply(sl.msg);
        const uint64_t t_book = tscx::tsc();                   // T3: book updated
        p.counts[sl.msg[0]]++;
        ++p.parsed;
        if (++k == sample) {
            k = 0;
            Trace tr{sl.t_rx, t_deq, t_book, sl.msg[0], {}};
            if (tq.try_push(tr)) ++p.traced; else ++p.trace_dropped;   // never block
        }
    };
    for (;;) {
        if (q.try_pop(s)) { handle(s); continue; }
        if (rx_done.load(std::memory_order_acquire)) {
            while (q.try_pop(s)) handle(s);
            break;
        }
        cpu_relax();
    }
    parse_done.store(true, std::memory_order_release);
}

// ---- Thread C: logger (off the hot path) ------------------------------------------------
void log_loop(TraceQueue& tq, std::atomic<bool>& parse_done, FILE* f, uint64_t& written) {
    std::vector<Trace> batch(kLogBatch);
    size_t n = 0;
    Trace t;
    auto flush = [&] {
        if (n && std::fwrite(batch.data(), sizeof(Trace), n, f) != n) die("fwrite");
        written += n; n = 0;
    };
    for (;;) {
        bool got = false;
        while (n < kLogBatch && tq.try_pop(t)) { batch[n++] = t; got = true; }
        if (n == kLogBatch) flush();
        if (!got) {
            if (parse_done.load(std::memory_order_acquire)) {
                while (tq.try_pop(t)) { batch[n++] = t; if (n == kLogBatch) flush(); }
                flush();
                return;
            }
            usleep(200);          // logger may sleep: it is not latency-critical
        }
    }
}

}  // namespace

int main(int argc, char** argv) {
    Args a = parse_args(argc, argv);
    if (a.mlock && mlockall(MCL_CURRENT | MCL_FUTURE) != 0)
        std::perror("mlockall (continuing without it)");

    // ---- TSC sanity + calibration (before any traffic) ----
    const bool c_tsc = tscx::cpu_has_flag("constant_tsc");
    const bool n_tsc = tscx::cpu_has_flag("nonstop_tsc");
    std::printf("constant_tsc=%s nonstop_tsc=%s\n", c_tsc ? "yes" : "NO", n_tsc ? "yes" : "NO");
    if (!c_tsc || !n_tsc)
        std::fprintf(stderr, "WARNING: TSC may not be invariant; cross-core deltas are suspect\n");
    const double cpn = tscx::calibrate_tsc(200);
    const auto ov = tscx::tsc_overhead();
    std::printf("TSC: %.4f cycles/ns (%.3f GHz), tsc() overhead min=%llu mean=%.1f cycles (~%.1f ns)\n",
                cpn, cpn, (unsigned long long)ov.min_cycles, ov.mean_cycles, ov.mean_cycles / cpn);

    // ---- trace file ----
    FILE* f = std::fopen(a.trace.c_str(), "wb");
    if (!f) die("fopen trace");
    TraceHeader h{}; std::memcpy(h.magic, "ITCHTRC1", 8); h.cycles_per_ns = cpn;
    std::fwrite(&h, sizeof h, 1, f);

    int fd = open_socket(a);
    auto q  = std::make_unique<Queue>();          // value-init touches pages now
    auto tq = std::make_unique<TraceQueue>();

    std::atomic<bool> rx_done{false}, parse_done{false};
    RxStats rs; ParseStats ps; uint64_t written = 0;

    std::printf("listening on 127.0.0.1:%u (trace -> %s, sample 1/%llu)\n",
                a.port, a.trace.c_str(), (unsigned long long)a.sample);
    std::fflush(stdout);

    std::thread logger(log_loop, std::ref(*tq), std::ref(parse_done), f, std::ref(written));
    pin_thread(logger, a.log_cpu);
    std::thread parser(parse_loop, std::ref(*q), std::ref(*tq), std::ref(rx_done),
                       std::ref(parse_done), a.sample, std::ref(ps));
    pin_thread(parser, a.parse_cpu);
    std::thread rx(rx_loop, fd, std::ref(*q), std::ref(rx_done), std::ref(rs));
    pin_thread(rx, a.rx_cpu);

    rx.join(); parser.join(); logger.join();
    close(fd);

    h.records = written; h.dropped = ps.trace_dropped;
    std::fseek(f, 0, SEEK_SET);
    std::fwrite(&h, sizeof h, 1, f);
    std::fclose(f);

    const double secs = (rs.t_last - rs.t_first) / 1e9 - 2.0;   // minus idle timeout
    const auto& b = ps.book->stats();
    std::printf("received   = %llu\n", (unsigned long long)rs.received);
    std::printf("parsed     = %llu %s\n", (unsigned long long)ps.parsed,
                ps.parsed == rs.received ? "(== received, OK)" : "(MISMATCH!)");
    std::printf("lost       = %llu (%.4f%%)  reordered=%llu  malformed=%llu\n",
                (unsigned long long)rs.lost, rs.expected ? 100.0 * rs.lost / rs.expected : 0.0,
                (unsigned long long)rs.reordered, (unsigned long long)rs.malformed);
    std::printf("queue_full = %llu   max_depth ~ %llu / %zu\n",
                (unsigned long long)rs.queue_full, (unsigned long long)rs.max_depth, kQueueCap);
    std::printf("rate       = %.0f msg/s\n", secs > 0 ? rs.received / secs : 0.0);
    std::printf("book: updates=%llu missing_ref=%llu over_exec=%llu dup_ref=%llu crossed=%llu live=%zu\n",
                (unsigned long long)b.applied, (unsigned long long)b.missing_ref,
                (unsigned long long)b.over_exec, (unsigned long long)b.dup_ref,
                (unsigned long long)b.crossed, ps.book->live_orders());
    std::printf("trace: written=%llu dropped=%llu -> %s\n",
                (unsigned long long)written, (unsigned long long)ps.trace_dropped, a.trace.c_str());
}

