// orderbook.hpp - ITCH 5.0 order book (correct-first baseline version).
//
// Handles: A/F add, E exec, C exec-with-price, X cancel, D delete, U replace.
// Prices are uint32 fixed-point (4 decimals). Never converted to double here.
//
// Offsets are into the ITCH message starting at the type byte:
//   0 type | 1-2 stock locate | 3-4 tracking | 5-10 timestamp | 11.. body
#pragma once
#include <endian.h>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <unordered_map>
#include <vector>

namespace itch {

inline uint16_t be16(const uint8_t* p) { uint16_t v; std::memcpy(&v, p, 2); return be16toh(v); }
inline uint32_t be32(const uint8_t* p) { uint32_t v; std::memcpy(&v, p, 4); return be32toh(v); }
inline uint64_t be64(const uint8_t* p) { uint64_t v; std::memcpy(&v, p, 8); return be64toh(v); }

struct Order {
    uint32_t price;
    uint32_t shares;
    uint16_t locate;
    char     side;      // 'B' or 'S'
};

struct Book {
    std::map<uint32_t, uint64_t, std::greater<>> bids;   // price -> total shares, best first
    std::map<uint32_t, uint64_t>                 asks;   // price -> total shares, best first
    char symbol[9] = {};                                  // from 'R' or first 'A'
};

struct BookStats {
    uint64_t applied = 0;
    uint64_t missing_ref = 0;     // E/C/X/D/U for an order we don't know
    uint64_t over_exec = 0;       // exec/cancel more shares than the order has
    uint64_t dup_ref = 0;         // add with a ref that already exists
    uint64_t crossed = 0;         // updates after which best bid >= best ask
    uint64_t max_orders = 0;
};

class OrderBook {
public:
    OrderBook() : books_(65536) { orders_.reserve(1 << 22); }   // ~4M live orders, no rehash in hot path

    // Returns true if the message changed a book.
    bool apply(const uint8_t* m) {
        switch (m[0]) {
            case 'R': set_symbol(be16(m + 1), m + 11); return false;
            case 'A': case 'F': {
                const uint16_t loc = be16(m + 1);
                set_symbol_if_empty(loc, m + 24);
                add(be64(m + 11), loc, char(m[19]), be32(m + 32), be32(m + 20));
                break;
            }
            case 'E': reduce(be64(m + 11), be32(m + 19)); break;   // executes at resting price
            case 'C': reduce(be64(m + 11), be32(m + 19)); break;   // book level is still the resting price
            case 'X': reduce(be64(m + 11), be32(m + 19)); break;
            case 'D': remove(be64(m + 11)); break;
            case 'U': replace(be64(m + 11), be64(m + 19), be32(m + 31), be32(m + 27)); break;
            default:  return false;
        }
        ++st_.applied;
        return true;
    }

    const Book& book(uint16_t locate) const { return books_[locate]; }
    const BookStats& stats() const { return st_; }
    std::size_t live_orders() const { return orders_.size(); }

    int find_locate(const char* sym) const {
        for (int i = 0; i < 65536; ++i)
            if (std::strcmp(books_[i].symbol, sym) == 0) return i;
        return -1;
    }

private:
    void set_symbol(uint16_t loc, const uint8_t* s) {
        char* d = books_[loc].symbol;
        std::memcpy(d, s, 8); d[8] = 0;
        for (int i = 7; i >= 0 && d[i] == ' '; --i) d[i] = 0;   // strip padding
    }
    void set_symbol_if_empty(uint16_t loc, const uint8_t* s) {
        if (!books_[loc].symbol[0]) set_symbol(loc, s);
    }

    template <typename Map>
    static void level_add(Map& lv, uint32_t px, uint64_t q) { lv[px] += q; }

    template <typename Map>
    static void level_sub(Map& lv, uint32_t px, uint64_t q) {
        auto it = lv.find(px);
        if (it == lv.end()) return;                 // cannot happen if orders_ is consistent
        if (it->second <= q) lv.erase(it);          // level empty -> remove it
        else it->second -= q;
    }

    void level_change(const Order& o, uint64_t q, bool add) {
        Book& b = books_[o.locate];
        if (o.side == 'B') add ? level_add(b.bids, o.price, q) : level_sub(b.bids, o.price, q);
        else               add ? level_add(b.asks, o.price, q) : level_sub(b.asks, o.price, q);
    }

    void check_cross(uint16_t loc) {
        const Book& b = books_[loc];
        if (!b.bids.empty() && !b.asks.empty() && b.bids.begin()->first >= b.asks.begin()->first)
            ++st_.crossed;
    }

    void add(uint64_t ref, uint16_t loc, char side, uint32_t px, uint32_t sh) {
        auto [it, inserted] = orders_.try_emplace(ref, Order{px, sh, loc, side});
        if (!inserted) { ++st_.dup_ref; return; }
        level_change(it->second, sh, true);
        if (orders_.size() > st_.max_orders) st_.max_orders = orders_.size();
        check_cross(loc);
    }

    void reduce(uint64_t ref, uint32_t sh) {
        auto it = orders_.find(ref);
        if (it == orders_.end()) { ++st_.missing_ref; return; }
        Order& o = it->second;
        if (sh > o.shares) { ++st_.over_exec; sh = o.shares; }
        level_change(o, sh, false);
        o.shares -= sh;
        const uint16_t loc = o.locate;
        if (o.shares == 0) orders_.erase(it);       // fully filled/cancelled
        check_cross(loc);
    }

    void remove(uint64_t ref) {
        auto it = orders_.find(ref);
        if (it == orders_.end()) { ++st_.missing_ref; return; }
        level_change(it->second, it->second.shares, false);
        const uint16_t loc = it->second.locate;
        orders_.erase(it);
        check_cross(loc);
    }

    // U: old order is removed; new ref inherits side and stock.
    void replace(uint64_t old_ref, uint64_t new_ref, uint32_t px, uint32_t sh) {
        auto it = orders_.find(old_ref);
        if (it == orders_.end()) { ++st_.missing_ref; return; }
        const Order old = it->second;
        level_change(old, old.shares, false);
        orders_.erase(it);
        add(new_ref, old.locate, old.side, px, sh);
    }

    std::unordered_map<uint64_t, Order> orders_;
    std::vector<Book> books_;                       // indexed by stock locate: O(1), no hashing
    BookStats st_;
};

}  // namespace itch

