#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

int main() {
    std::vector<uint8_t> buf(1 << 20);          // 1 MB read buffer
    size_t have = 0;
    std::array<uint64_t, 256> counts{};
    uint64_t total = 0;

    for (;;) {
        size_t r = fread(buf.data() + have, 1, buf.size() - have, stdin);
        have += r;

        size_t p = 0;
        while (have - p >= 2) {
            uint16_t n = (uint16_t(buf[p]) << 8) | buf[p + 1];   // big-endian length
            if (n == 0 || have - p < 2u + n) break;              // need more bytes
            counts[buf[p + 2]]++;                                // type byte
            total++;
            p += 2 + n;                                          // next message
        }
        std::memmove(buf.data(), buf.data() + p, have - p);      // keep partial msg
        have -= p;
        if (r == 0) break;                                       // EOF (truncated tail ignored)
    }

    printf("total=%llu\n", (unsigned long long)total);
    for (int t = 0; t < 256; ++t)
        if (counts[t]) printf("%c=%llu\n", t, (unsigned long long)counts[t]);
}
