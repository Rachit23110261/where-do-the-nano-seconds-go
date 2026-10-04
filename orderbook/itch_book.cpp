// itch_book.cpp - offline order-book driver: mmap an ITCH file, apply every message,
// print correctness stats, throughput, and top of book for one symbol.
// Build: g++ -O2 -std=c++20 -Wall -Wextra -o itch_book itch_book.cpp
// Usage: ./itch_book <file.itch> [SYMBOL] [--count N]
#include "orderbook.hpp"
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>

static void print_top(const itch::OrderBook& ob, int loc, uint64_t at) {
    const auto& b = ob.book(uint16_t(loc));
    std::printf("  [%10llu] %-6s ", (unsigned long long)at, b.symbol);
    if (b.bids.empty()) std::printf("bid ---------------- ");
    else std::printf("bid %8llu @ %9.4f ", 
        (unsigned long long)b.bids.begin()->second, b.bids.begin()->first / 1e4);
    if (b.asks.empty()) std::printf("| ask ----------------\n");
    else std::printf("| ask %8llu @ %9.4f\n", 
        (unsigned long long)b.asks.begin()->second, b.asks.begin()->first / 1e4);
}

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr,
        "usage: %s <file.itch> [SYMBOL] [--count N]\n", argv[0]); return 2; }
    std::string sym = "AAPL";
    uint64_t limit = UINT64_MAX;
    for (int i = 2; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--count" && i + 1 < argc) limit = std::stoull(argv[++i]);
        else sym = a;
    }

    int fd = open(argv[1], O_RDONLY);
    if (fd < 0) { std::perror("open"); return 1; }
    struct stat st; fstat(fd, &st);
    const size_t size = size_t(st.st_size);
    auto* data = static_cast<const uint8_t*>(mmap(nullptr,
          size, PROT_READ, MAP_PRIVATE | MAP_POPULATE, fd, 0));
    if (data == MAP_FAILED) { std::perror("mmap"); return 1; }
    close(fd);
    if (size >= 2 && data[0] == 0x1f && data[1] == 0x8b) {
      std::fprintf(stderr, "gzip file: decompress first\n"); 
      return 1;
    }

    auto ob = std::make_unique<itch::OrderBook>();
    int loc = -1;
    uint64_t n_msgs = 0;
    size_t p = 0;

    auto t0 = std::chrono::steady_clock::now();
    while (p + 2 <= size && n_msgs < limit) {
        const uint16_t n = uint16_t((data[p] << 8) | data[p + 1]);
        if (n == 0 || p + 2 + n > size) break;
        const uint8_t* m = data + p + 2;
        ob->apply(m);
        ++n_msgs;
        p += 2 + n;

        if (loc < 0 && (m[0] == 'R' || m[0] == 'A')) {          // resolve symbol once it appears
            int l = itch::be16(m + 1);
            if (sym == ob->book(uint16_t(l)).symbol) loc = l;
        }
        if (loc >= 0 && n_msgs % 500000 == 0) print_top(*ob, loc, n_msgs);
    }
    double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

    const auto& x = ob->stats();
    std::printf("\nmessages     = %llu\n", (unsigned long long)n_msgs);
    std::printf("book updates = %llu\n", (unsigned long long)x.applied);
    std::printf("missing_ref  = %llu   (must be 0 when starting at the file beginning)\n",
                                        (unsigned long long)x.missing_ref);
    std::printf("over_exec    = %llu   (must be 0)\n", (unsigned long long)x.over_exec);
    std::printf("dup_ref      = %llu   (must be 0)\n", (unsigned long long)x.dup_ref);
    std::printf("crossed      = %llu   (updates leaving best bid >= best ask)\n",
                                        (unsigned long long)x.crossed);
    std::printf("live orders  = %zu (peak %llu)\n", ob->live_orders(),
                                        (unsigned long long)x.max_orders);
    std::printf("time         = %.3f s  -> %.1f ns/msg, %.2f M msg/s\n",
                                        s, s * 1e9 / n_msgs, n_msgs / s / 1e6);
    if (loc >= 0) { std::printf("final:\n"); print_top(*ob, loc, n_msgs); }
    else std::printf("symbol %s not seen\n", sym.c_str());
    munmap(const_cast<uint8_t*>(data), size);
}

