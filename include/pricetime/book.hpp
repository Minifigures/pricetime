#pragma once

#include <bit>
#include <cstddef>
#include <unordered_map>
#include <vector>

#include "pricetime/events.hpp"
#include "pricetime/order.hpp"
#include "pricetime/types.hpp"

namespace pricetime {

// ---------------------------------------------------------------------------
// Book: the fast path. Same observable behaviour as ReferenceBook, different
// data structures. Where the reference reaches for std::map and std::vector,
// this reaches for the three things that actually matter in a matching engine:
//
//   1. A FLAT PRICE LADDER instead of a tree. Real order flow clusters in a
//      narrow band around the touch, so a std::map's O(log n) plus a pointer
//      chase per level is paid for nothing. The ladder is a vector indexed by
//      (price - floor), making level lookup a single array offset.
//
//   2. INTRUSIVE DOUBLY-LINKED LISTS with a free list. Orders live in one
//      pre-sized pool and are threaded together by index, not pointer. After
//      warmup the engine performs no allocation at all on the hot path, so
//      there is no malloc lock and no tail latency spike when the allocator
//      decides to go get more memory. Indices also stay valid across pool
//      growth, which raw pointers would not.
//
//   3. A BITMAP OVER OCCUPIED LEVELS. When the best level is consumed the
//      engine must find the next one. Walking the ladder is O(band). Keeping
//      one bit per level and scanning 64 at a time with std::countr_zero makes
//      it O(band/64) with no branches in the inner step.
//
// The price band is bounded and declared up front. That is a real constraint,
// not a hidden one: prices outside [floor, ceil] are rejected rather than
// silently clamped, and the README says so. Venues do the same thing under the
// name "price bands", for the same reason.
// ---------------------------------------------------------------------------
class Book {
 public:
  Book(Price floor_px, Price ceil_px,
       SelfTradePolicy stp = SelfTradePolicy::None,
       std::size_t expected_orders = 1u << 16);

  void submit(const NewOrder& o, EventLog& out);
  void cancel(const CancelOrder& c, EventLog& out);
  void replace(const ReplaceOrder& r, EventLog& out);

  [[nodiscard]] Price best_bid() const noexcept {
    return best_bid_ == kNoLevel ? kInvalidPrice : to_price(best_bid_);
  }
  [[nodiscard]] Price best_ask() const noexcept {
    return best_ask_ == kNoLevel ? kInvalidPrice : to_price(best_ask_);
  }
  [[nodiscard]] Qty qty_at(Side s, Price p) const noexcept;

  // Top of book in one call. O(1): the touch indices and the per-level running
  // totals are both already maintained, so this reads them rather than
  // searching. The consolidator calls this once per venue per symbol on every
  // NBBO recomputation, so it must not walk anything.
  [[nodiscard]] Qty bid_size() const noexcept {
    return best_bid_ == kNoLevel ? 0 : bid_lvls_[best_bid_].total;
  }
  [[nodiscard]] Qty ask_size() const noexcept {
    return best_ask_ == kNoLevel ? 0 : ask_lvls_[best_ask_].total;
  }
  [[nodiscard]] std::size_t resting_count() const noexcept { return index_.size(); }
  [[nodiscard]] std::vector<std::pair<Price, Qty>> depth(Side s,
                                                         std::size_t levels) const;

 private:
  using Idx = std::uint32_t;
  static constexpr Idx kNil     = 0xFFFFFFFFu;
  static constexpr Idx kNoLevel = 0xFFFFFFFFu;

  struct Node {
    OrderId       id    = kInvalidOrderId;
    ParticipantId owner = kAnonymous;
    Qty           open  = 0;
    Seq           seq   = 0;
    Idx           lvl   = kNoLevel;
    Idx           prev  = kNil;
    Idx           next  = kNil;
    Side          side  = Side::Buy;
  };

  // One level per side per tick. Bids and asks are separate ladders so a
  // crossed book is representable (and rejected) rather than corrupting one
  // shared array.
  struct Level {
    Idx head  = kNil;
    Idx tail  = kNil;
    Qty total = 0;
    [[nodiscard]] bool empty() const noexcept { return head == kNil; }
  };

  [[nodiscard]] bool in_band(Price p) const noexcept {
    return p >= floor_ && p <= ceil_;
  }
  [[nodiscard]] Idx to_idx(Price p) const noexcept {
    return static_cast<Idx>(p - floor_);
  }
  [[nodiscard]] Price to_price(Idx i) const noexcept {
    return floor_ + static_cast<Price>(i);
  }

  Idx  alloc_node();
  void free_node(Idx n);
  void link_back(std::vector<Level>& side, Idx lvl, Idx node);
  void unlink(std::vector<Level>& side, Idx lvl, Idx node);

  void set_occupied(Side s, Idx lvl, bool on) noexcept;
  [[nodiscard]] Idx scan_down(const std::vector<std::uint64_t>& bm, Idx from) const noexcept;
  [[nodiscard]] Idx scan_up(const std::vector<std::uint64_t>& bm, Idx from) const noexcept;

  // Best-price maintenance is incremental, and this is load-bearing.
  //
  // The obvious implementation rescans the occupancy bitmap from the end of
  // the band after every mutation. That is O(band) and it is invisible on a
  // synthetic benchmark with a narrow band: at 2,001 ticks the bitmap is 31
  // words and the scan disappears into the noise. Replaying a real symbol,
  // whose daily range spans hundreds of thousands of ticks, the same scan
  // touches ~9,500 words per operation and dominates everything else.
  //
  // Instead: an insert can only ever improve the best, which is a compare and
  // a store. A removal only matters if it emptied the level that WAS the best,
  // and then the scan runs from there in the worsening direction only, so it
  // costs the distance the touch actually moved rather than the width of the
  // band.
  void best_after_insert(Side s, Idx lvl) noexcept;
  void best_after_remove(Side s, Idx lvl) noexcept;

  [[nodiscard]] Qty available_against(Side aggressor, Price limit) const;

  Price floor_ = 0;
  Price ceil_  = 0;
  Idx   span_  = 0;

  std::vector<Level>        bid_lvls_, ask_lvls_;
  std::vector<std::uint64_t> bid_bm_,  ask_bm_;
  Idx   best_bid_ = kNoLevel;   // highest occupied bid index
  Idx   best_ask_ = kNoLevel;   // lowest  occupied ask index

  std::vector<Node> pool_;
  Idx               free_head_ = kNil;

  std::unordered_map<OrderId, Idx> index_;
  SelfTradePolicy stp_      = SelfTradePolicy::None;
  Seq             next_seq_ = 1;
};

}  // namespace pricetime
