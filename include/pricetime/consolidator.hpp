#pragma once

// Cross-venue consolidation: one book per (venue, symbol), and a best bid and
// offer computed across all of them.
//
// Why this shape
// --------------
// A single order book answers "what is the best price HERE". A trader needs
// "what is the best price ANYWHERE", because an execution at a worse price
// than some other venue was displaying is the thing regulators call a
// trade-through. In US equities the consolidated quote is the NBBO (National
// Best Bid and Offer) and, under Reg NMS Rule 611, executing through a
// protected quotation is prohibited.
//
// Worth knowing while reading this: the SEC published a proposal on
// 2026-06-17 (Federal Register doc 2026-12163) to RESCIND the trade-through
// rule and the locked-and-crossed provisions entirely. It is pending. So the
// rule that makes cross-venue price priority binding may not survive, which
// makes the question this class answers more interesting rather than less.
//
// Terminology used here is deliberately the industry's, not invented:
//   - get_exchange_bbo(venue, symbol) -- top of book at one venue
//   - get_nbbo(symbol)                -- best bid and best offer across venues
//   - locked   -- best bid == best ask across venues
//   - crossed  -- best bid >  best ask across venues
//
// Locked and crossed markets are not errors in this engine's arithmetic; they
// are real states that occur when two venues' books disagree, and Reg NMS
// Rule 610 prohibits displaying quotes that lock or cross another venue. A
// consolidator's job is to surface them, so they are reported rather than
// silently normalised away.

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "pricetime/book.hpp"
#include "pricetime/types.hpp"

namespace pricetime {

using VenueId  = std::uint16_t;
using SymbolId = std::uint32_t;

inline constexpr VenueId kNoVenue = 0xFFFFu;

struct Bbo {
  Price bid_px = kInvalidPrice;
  Qty   bid_sz = 0;
  Price ask_px = kInvalidPrice;
  Qty   ask_sz = 0;

  [[nodiscard]] bool has_bid() const noexcept { return bid_px != kInvalidPrice; }
  [[nodiscard]] bool has_ask() const noexcept { return ask_px != kInvalidPrice; }
  [[nodiscard]] bool operator==(const Bbo&) const = default;
};

struct Nbbo {
  Bbo     quote;
  VenueId bid_venue = kNoVenue;
  VenueId ask_venue = kNoVenue;

  // Both sides present and bid == ask. Neither side can trade, but both are
  // displayed, which is the definition of a locked market.
  [[nodiscard]] bool locked() const noexcept {
    return quote.has_bid() && quote.has_ask() && quote.bid_px == quote.ask_px;
  }
  // Bid strictly above ask across venues. Free money if you can reach both.
  [[nodiscard]] bool crossed() const noexcept {
    return quote.has_bid() && quote.has_ask() && quote.bid_px > quote.ask_px;
  }
  // Spread in ticks, or -1 when one side is empty.
  [[nodiscard]] Price spread() const noexcept {
    return (quote.has_bid() && quote.has_ask()) ? quote.ask_px - quote.bid_px
                                                : -1;
  }
  [[nodiscard]] bool operator==(const Nbbo&) const = default;
};

// A trade printed at a price worse than the best quote available elsewhere.
struct TradeThrough {
  SymbolId symbol       = 0;
  VenueId  venue        = kNoVenue;  // where it printed
  Side     aggressor    = Side::Buy;
  Price    print_px     = 0;
  Price    best_elsewhere = 0;
  VenueId  better_venue = kNoVenue;
  Qty      qty          = 0;
  Price    harm         = 0;  // ticks per share the taker gave up
};

class Consolidator {
 public:
  Consolidator() = default;

  VenueId add_venue(std::string name);
  [[nodiscard]] const std::string& venue_name(VenueId v) const;
  [[nodiscard]] std::size_t venue_count() const noexcept { return venues_.size(); }

  // Creates the book on first use. Band is per-symbol and must be chosen so
  // the ladder stays cache-resident; see the cache section of the README.
  Book& book(VenueId v, SymbolId s, Price floor_px, Price ceil_px);
  [[nodiscard]] const Book* find_book(VenueId v, SymbolId s) const;

  [[nodiscard]] Bbo  get_exchange_bbo(VenueId v, SymbolId s) const;
  [[nodiscard]] Nbbo get_nbbo(SymbolId s) const;

  // Was this print worse than the best quote showing on another venue at the
  // time? Returns false when it was not.
  [[nodiscard]] bool check_trade_through(SymbolId s, VenueId printed_on,
                                         Side aggressor, Price print_px,
                                         Qty qty, TradeThrough& out) const;

  [[nodiscard]] std::vector<SymbolId> symbols() const;

 private:
  struct Key {
    VenueId  v;
    SymbolId s;
    [[nodiscard]] bool operator==(const Key&) const = default;
  };
  struct KeyHash {
    [[nodiscard]] std::size_t operator()(const Key& k) const noexcept {
      // splitmix64 finalizer over the packed pair. Venue and symbol ids are
      // both small and dense, so the low bits of a naive combination collide
      // in exactly the pattern a power-of-two bucket count is worst at.
      std::uint64_t x = (static_cast<std::uint64_t>(k.v) << 32) ^ k.s;
      x ^= x >> 30; x *= 0xBF58476D1CE4E5B9ull;
      x ^= x >> 27; x *= 0x94D049BB133111EBull;
      x ^= x >> 31;
      return static_cast<std::size_t>(x);
    }
  };

  std::vector<std::string> venues_;
  std::unordered_map<Key, std::unique_ptr<Book>, KeyHash> books_;
};

}  // namespace pricetime
