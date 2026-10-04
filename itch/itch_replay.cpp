// itch_replay.cpp - Replay a NASDAQ ITCH 5.0 file over UDP.
//
// UDP payload per datagram:
//   [uint64 seq (host byte order)][uint16 len (big-endian, as in file)][ITCH message (len bytes)]
//
// Build:
//   g++ -O2 -std=c++20 -Wall -Wextra -o itch_replay itch_replay.cpp
// Usage:
//   ./itch_replay <file.itch> <dst_ip> <dst_port> [--rate N] [--count N] [--cpu C]
//     --rate N   messages/sec (0 = as fast as possible, default)
//     --count N  stop after N messages (default: whole file)
//     --cpu C    pin this process to CPU core C

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>

namespace {

constexpr size_t kMaxPayload = 2048;   // largest ITCH 5.0 msg is ~50 bytes; plenty of headroom
constexpr size_t kSeqBytes   = 8;
constexpr size_t kLenBytes   = 2;

[[noreturn]] void die(const char* what) {
    std::perror(what);
    std::exit(1);
}

inline uint64_t now_ns() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return uint64_t(ts.tv_sec) * 1'000'000'000ull + uint64_t(ts.tv_nsec);
}

inline void cpu_relax() {
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__)
    asm volatile("yield");
#endif
}

struct Args {
    const char* path = nullptr;
    const char* ip   = nullptr;
    uint16_t    port = 0;
    uint64_t    rate = 0;            // 0 = max speed
    uint64_t    count = UINT64_MAX;  // default: whole file
    int         cpu  = -1;           // -1 = don't pin
};

void usage(const char* prog) {
    std::fprintf(stderr,
        "usage: %s <file.itch> <dst_ip> <dst_port> [--rate N] [--count N] [--cpu C]\n", prog);
    std::exit(2);
}

Args parse_args(int argc, char** argv) {
    if (argc < 4) usage(argv[0]);
    Args a;
    a.path = argv[1];
    a.ip   = argv[2];
    a.port = static_cast<uint16_t>(std::stoul(argv[3]));
    for (int i = 4; i < argc; ++i) {
        std::string opt = argv[i];
        if (i + 1 >= argc) usage(argv[0]);
        if      (opt == "--rate")  a.rate  = std::stoull(argv[++i]);
        else if (opt == "--count") a.count = std::stoull(argv[++i]);
        else if (opt == "--cpu")   a.cpu   = std::stoi(argv[++i]);
        else usage(argv[0]);
    }
    return a;
}

void pin_to_cpu(int cpu) {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    if (sched_setaffinity(0, sizeof(set), &set) != 0) die("sched_setaffinity");
}

// Map the whole file read-only. Returns pointer and sets size.
const uint8_t* map_file(const char* path, size_t& size) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) die("open");
    struct stat st;
    if (fstat(fd, &st) != 0) die("fstat");
    size = static_cast<size_t>(st.st_size);
    if (size == 0) { std::fprintf(stderr, "empty file\n"); std::exit(1); }
    void* p = mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (p == MAP_FAILED) die("mmap");
    close(fd);                                   // mapping stays valid
    madvise(p, size, MADV_SEQUENTIAL);           // hint: we read front to back
    return static_cast<const uint8_t*>(p);
}

int make_udp_socket(const char* ip, uint16_t port, sockaddr_in& dst) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) die("socket");

    int sndbuf = 32 * 1024 * 1024;               // capped by net.core.wmem_max
    if (setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf)) != 0)
        die("setsockopt(SO_SNDBUF)");

    std::memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_port   = htons(port);
    if (inet_pton(AF_INET, ip, &dst.sin_addr) != 1) {
        std::fprintf(stderr, "bad IP: %s\n", ip);
        std::exit(1);
    }
    // connect() on UDP fixes the destination: lets us use send() and skips
    // a per-packet route lookup in the kernel.
    if (connect(fd, reinterpret_cast<sockaddr*>(&dst), sizeof(dst)) != 0) die("connect");
    return fd;
}

// Send one datagram, retrying while the kernel send buffer is full.
// Returns number of retries needed.
uint64_t send_all(int fd, const uint8_t* pkt, size_t len) {
    uint64_t retries = 0;
    for (;;) {
        ssize_t r = send(fd, pkt, len, 0);
        if (r == static_cast<ssize_t>(len)) return retries;
        if (r < 0 && (errno == ENOBUFS || errno == EAGAIN || errno == EINTR)) {
            ++retries;
            cpu_relax();
            continue;
        }
        if (r < 0 && errno == ECONNREFUSED) {
            // Loopback with no receiver bound yet: ICMP port unreachable.
            // Not fatal for a replayer; the datagram is simply dropped.
            return retries;
        }
        die("send");
    }
}

}  // namespace

int main(int argc, char** argv) {
    Args a = parse_args(argc, argv);
    if (a.cpu >= 0) pin_to_cpu(a.cpu);

    size_t size = 0;
    const uint8_t* data = map_file(a.path, size);

    sockaddr_in dst{};
    int fd = make_udp_socket(a.ip, a.port, dst);

    alignas(64) uint8_t pkt[kMaxPayload];
    size_t   p        = 0;      // byte offset into file
    uint64_t seq      = 0;      // gap-free sequence number
    uint64_t retries  = 0;
    uint64_t bytes    = 0;

    const uint64_t gap_ns   = a.rate ? 1'000'000'000ull / a.rate : 0;
    const uint64_t start_ns = now_ns();

    while (p + kLenBytes <= size && seq < a.count) {
        // ---- framing: 2-byte big-endian length ----
        const uint16_t n = static_cast<uint16_t>((data[p] << 8) | data[p + 1]);
        if (n == 0 || p + kLenBytes + n > size) break;            // corrupt / truncated tail
        if (kSeqBytes + kLenBytes + n > kMaxPayload) {
            std::fprintf(stderr, "message too large at offset %zu (len=%u)\n", p, n);
            break;
        }

        // ---- build packet: [seq][len][msg] ----
        std::memcpy(pkt, &seq, kSeqBytes);
        std::memcpy(pkt + kSeqBytes, data + p, kLenBytes + n);
        const size_t pkt_len = kSeqBytes + kLenBytes + n;

        // ---- pacing against an absolute schedule (no drift) ----
        if (gap_ns) {
            const uint64_t target = start_ns + seq * gap_ns;
            while (now_ns() < target) cpu_relax();
        }

        // ---- send ----
        retries += send_all(fd, pkt, pkt_len);

        bytes += pkt_len;
        ++seq;
        p += kLenBytes + n;
    }

    const double secs = double(now_ns() - start_ns) / 1e9;
    std::printf("sent      = %llu msgs\n", static_cast<unsigned long long>(seq));
    std::printf("bytes     = %llu\n",      static_cast<unsigned long long>(bytes));
    std::printf("elapsed   = %.3f s\n",    secs);
    std::printf("rate      = %.0f msg/s\n", secs > 0 ? double(seq) / secs : 0.0);
    std::printf("retries   = %llu (send buffer full)\n", static_cast<unsigned long long>(retries));
    if (p < size && seq < a.count)
        std::printf("stopped at offset %zu of %zu (truncated tail?)\n", p, size);

    munmap(const_cast<uint8_t*>(data), size);
    close(fd);
    return 0;
}

