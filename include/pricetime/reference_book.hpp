#pragma once

#include <algorithm>
#include <map>
#include <unordered_map>
#include <vector>

#include "pricetime/events.hpp"
#include "pricetime/order.hpp"
#include "pricetime/types.hpp"

namespace pricetime {

// ---------------------------------------------------------------------------
// ReferenceBook: the specification, written to be obviously correct.
//
// This implementation is deliberately slow and deliberately boring. It uses
// std::map keyed by price and a std::vector per level, it copies freely, and
// it never optimizes anything. That is the entire point. It exists so that
// Book -- which is not obviously correct, because it is fast -- has something
// to be checked against.
//
// The rule this codebase follows: when the two disagree, the reference is
// right until proven otherwise. If a behaviour is genuinely ambiguous, it gets
// decided here first, in the slow code where the decision is legible, and only
// then ported.
//
// See tests/test_differential.cpp for the harness that drives millions of
// randomized operations through both and asserts identical event streams.
// ---------------------------------------------------------------------------
class ReferenceBook {
 public:
  explicit ReferenceBook(SelfTradePolicy stp = SelfTradePolicy::None,
                         Allocation alloc = Allocation::Fifo) noexcept
      : stp_(stp), alloc_(alloc) {}

  void submit(const NewOrder& o, EventLog& out);
  void cancel(const CancelOrder& c, EventLog& out);
  void replace(const ReplaceOrder& r, EventLog& out);

  [[nodiscard]] Price best_bid() const noexcept {
    return bids_.empty() ? kInvalidPrice : bids_.begin()->first;
  }
  [[nodiscard]] Price best_ask() const noexcept {
    return asks_.empty() ? kInvalidPrice : asks_.begin()->first;
  }
  // Sums the resting quantity at one price. Slow by the standards of Book --
  // it walks the level rather than keeping a running total -- which is fine
  // and deliberate. What is NOT fine is copying the whole book to do it: an
  // earlier version of this routed through as_generic(), which materialises
  // every level into a fresh vector. Since queue_ahead made this run on every
  // rest, that turned the reference engine 90x slower and made the
  // Book-vs-Reference comparison meaningless. The four-regime benchmark caught
  // it. Being the slow implementation is the reference's job; being
  // accidentally quadratic is not.
  [[nodiscard]] Qty qty_at(Side s, Price p) const noexcept {
    Qty total = 0;
    if (s == Side::Buy) {
      const auto it = bids_.find(p);
      if (it != bids_.end())
        for (const auto& ro : it->second) total += ro.open;
    } else {
      const auto it = asks_.find(p);
      if (it != asks_.end())
        for (const auto& ro : it->second) total += ro.open;
    }
    return total;
  }
  [[nodiscard]] std::size_t resting_count() const noexcept {
    return live_.size();
  }
  // Depth snapshot, best-first, for the replay view and for asserting book
  // state equality between implementations.
  [[nodiscard]] std::vector<std::pair<Price, Qty>> depth(Side s,
                                                         std::size_t levels) const;

 private:
  using Level = std::vector<RestingOrder>;  // FIFO: front is oldest
  // Bids descend (best = highest), asks ascend (best = lowest). Encoding the
  // comparator in the type is what lets both sides share one matching loop.
  using Bids = std::map<Price, Level, std::greater<Price>>;
  using Asks = std::map<Price, Level, std::less<Price>>;

  [[nodiscard]] Qty available_against(Side aggressor, Price limit) const;
  void rest(const RestingOrder& ro);
  void remove_from_level(Side s, Price p, OrderId id);

  Bids bids_;
  Asks asks_;
  // id -> (side, price) so cancel/replace can find a resting order without
  // scanning. The reference is allowed exactly this one index, because
  // linear-scanning on cancel would make the differential fuzz too slow to
  // run the volumes that actually find bugs.
  std::unordered_map<OrderId, std::pair<Side, Price>> live_;
  SelfTradePolicy stp_ = SelfTradePolicy::None;
  Allocation      alloc_ = Allocation::Fifo;
  Seq next_seq_ = 1;
};

}  // namespace pricetime
