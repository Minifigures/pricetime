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
  explicit ReferenceBook(SelfTradePolicy stp = SelfTradePolicy::None) noexcept
      : stp_(stp) {}

  void submit(const NewOrder& o, EventLog& out);
  void cancel(const CancelOrder& c, EventLog& out);
  void replace(const ReplaceOrder& r, EventLog& out);

  [[nodiscard]] Price best_bid() const noexcept {
    return bids_.empty() ? kInvalidPrice : bids_.begin()->first;
  }
  [[nodiscard]] Price best_ask() const noexcept {
    return asks_.empty() ? kInvalidPrice : asks_.begin()->first;
  }
  [[nodiscard]] Qty qty_at(Side s, Price p) const noexcept {
    const auto& side = (s == Side::Buy) ? as_generic(bids_) : as_generic(asks_);
    Qty total = 0;
    for (const auto& [px, lvl] : side) {
      if (px != p) continue;
      for (const auto& ro : lvl) total += ro.open;
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

  // Erased view so qty_at/depth can walk either side without duplicating code.
  using Generic = std::vector<std::pair<Price, Level>>;
  [[nodiscard]] static Generic as_generic(const Bids& b) {
    return Generic(b.begin(), b.end());
  }
  [[nodiscard]] static Generic as_generic(const Asks& a) {
    return Generic(a.begin(), a.end());
  }

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
  Seq next_seq_ = 1;
};

}  // namespace pricetime
