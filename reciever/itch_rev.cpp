// itch_recv.cpp - single-threaded UDP receiver for itch_replay packets.
// Packet: [uint64 seq][uint16 len BE][ITCH msg (len bytes)]
// Build: g++ -O2 -std=c++20 -Wall -Wextra -o itch_recv itch_recv.cpp
// Usage: ./itch_recv <port> [--rcvbuf BYTES] [--cpu C]

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sched.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>

static uint64_t now_ns() {
    timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return uint64_t(ts.tv_sec) * 1'000'000'000ull + ts.tv_nsec;
}
[[noreturn]] static void die(const char* s) { perror(s); exit(1); }

int main(int argc, char** argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <port> [--rcvbuf N] [--cpu C]\n", argv[0]); return 2; }
    uint16_t port = uint16_t(std::stoul(argv[1]));
    int rcvbuf = 0, cpu = -1;
    for (int i = 2; i + 1 < argc; i += 2) {
        std::string o = argv[i];
        if (o == "--rcvbuf") rcvbuf = std::stoi(argv[i + 1]);
        else if (o == "--cpu") cpu = std::stoi(argv[i + 1]);
    }

    if (cpu >= 0) {
        cpu_set_t set; CPU_ZERO(&set); CPU_SET(cpu, &set);
        if (sched_setaffinity(0, sizeof set, &set)) die("sched_setaffinity");
    }

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) die("socket");
    if (rcvbuf > 0 && setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof rcvbuf)) die("SO_RCVBUF");
    int actual = 0; socklen_t sl = sizeof actual;
    getsockopt(fd, SOL_SOCKET, SO_RCVBUF, &actual, &sl);
    printf("SO_RCVBUF actual = %d bytes\n", actual);   // kernel doubles it, capped by rmem_max

    // 2 s idle timeout = "replayer finished"
    timeval tv{2, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(fd, (sockaddr*)&addr, sizeof addr)) die("bind");
    printf("listening on 127.0.0.1:%u\n", port);

    alignas(64) uint8_t buf[2048];
    std::array<uint64_t, 256> counts{};
    uint64_t received = 0, lost = 0, reordered = 0, malformed = 0, expected = 0;
    uint64_t t_first = 0, t_last = 0;

    for (;;) {
        ssize_t r = recv(fd, buf, sizeof buf, 0);
        if (r < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                if (received) break;         // idle after traffic → done
                continue;                    // still waiting for first packet
            }
            die("recv");
        }
        uint64_t t = now_ns();
        if (!received) t_first = t;
        t_last = t;

        if (r < 11) { ++malformed; continue; }            // seq + len + type
        uint16_t n = uint16_t((buf[8] << 8) | buf[9]);
        if (size_t(r) != 10u + n) { ++malformed; continue; }

        uint64_t seq; memcpy(&seq, buf, 8);
        if (seq >= expected) { lost += seq - expected; expected = seq + 1; }
        else { ++reordered; if (lost) --lost; }           // late arrival of a "lost" one

        counts[buf[10]]++;
        ++received;
    }

    double secs = (t_last - t_first) / 1e9;
    printf("received  = %llu\n", (unsigned long long)received);
    printf("lost      = %llu (%.4f%%)\n", (unsigned long long)lost,
           expected ? 100.0 * lost / expected : 0.0);
    printf("reordered = %llu\nmalformed = %llu\n",
           (unsigned long long)reordered, (unsigned long long)malformed);
    printf("rate      = %.0f msg/s\n", secs > 0 ? received / secs : 0.0);
    for (int c = 0; c < 256; ++c)
        if (counts[c]) printf("%c=%llu\n", c, (unsigned long long)counts[c]);
    close(fd);
}
