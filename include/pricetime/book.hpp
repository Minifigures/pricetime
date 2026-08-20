#pragma once

#include <bit>
#include <map>
#include <utility>
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
// TWO TIERS, because the flat ladder alone does not survive real data.
//
// A flat ladder is O(1) per level access, but its constant depends entirely on
// whether the array is cache-resident. Measured on real IEX order flow: a
// 12 MB ladder gives p50 47ns, a 27.7 MB ladder gives p50 1,986ns. The cliff
// sits exactly where the ladder stops fitting in this CPU's 20 MB L3. A single
// symbol's full-day price range is wide enough to land on the wrong side of it.
//
// So the ladder covers a bounded HOT band sized to stay in cache, and anything
// outside it goes into an ordered map. Real order flow clusters within a few
// ticks of the touch, so the map stays nearly empty in practice: for AAPL on
// 2024-12-23, a 262k-tick hot band covered 405,038 of 405,038 messages with
// 130 landing cold. The cold tier is O(log n) on a handful of levels; the hot
// tier is an array offset on everything that matters.
// ---------------------------------------------------------------------------
// One price level: an intrusive FIFO of order-pool indices plus a running
// total, so queue position and top-of-book size are both O(1).
struct Level {
  std::uint32_t head  = 0xFFFFFFFFu;
  std::uint32_t tail  = 0xFFFFFFFFu;
  Qty           total = 0;
  [[nodiscard]] bool empty() const noexcept { return head == 0xFFFFFFFFu; }
};

class Book {
 public:
  // hot_ticks bounds the flat ladder. The default keeps it comfortably inside
  // a typical L3 (262,144 levels x 16 bytes x 2 sides = 8 MB). Prices outside
  // the hot band still work; they just take the ordered-map path.
  static constexpr Price kDefaultHotTicks = 262'144;

  Book(Price floor_px, Price ceil_px,
       SelfTradePolicy stp = SelfTradePolicy::None,
       std::size_t expected_orders = 1u << 16,
       Price hot_ticks = kDefaultHotTicks,
       Allocation alloc = Allocation::Fifo,
       int fifo_percent = kDefaultFifoPercent,
       int time_weight = kDefaultTimeWeight);

  void submit(const NewOrder& o, EventLog& out);
  void cancel(const CancelOrder& c, EventLog& out);
  void replace(const ReplaceOrder& r, EventLog& out);

  [[nodiscard]] Price best_bid() const noexcept { return best_px(Side::Buy); }
  [[nodiscard]] Price best_ask() const noexcept { return best_px(Side::Sell); }
  [[nodiscard]] Qty qty_at(Side s, Price p) const noexcept;

  // Top of book in one call. O(1): the touch indices and the per-level running
  // totals are both already maintained, so this reads them rather than
  // searching. The consolidator calls this once per venue per symbol on every
  // NBBO recomputation, so it must not walk anything.
  [[nodiscard]] Qty bid_size() const noexcept { return qty_at(Side::Buy, best_px(Side::Buy)); }
  [[nodiscard]] Qty ask_size() const noexcept { return qty_at(Side::Sell, best_px(Side::Sell)); }
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
    // Price, not ladder index: a level may live in either tier, and price is
    // the only identifier both tiers share.
    Price         price = 0;
    Idx           prev  = kNil;
    Idx           next  = kNil;
    Side          side  = Side::Buy;
  };

  // One level per side per tick. Bids and asks are separate ladders so a
  // crossed book is representable (and rejected) rather than corrupting one
  // shared array.

  [[nodiscard]] bool in_band(Price p) const noexcept {
    return p >= floor_ && p <= ceil_;
  }
  [[nodiscard]] bool is_hot(Price p) const noexcept {
    return p >= hot_floor_ && p <= hot_ceil_;
  }
  [[nodiscard]] Idx to_idx(Price p) const noexcept {
    return static_cast<Idx>(p - hot_floor_);
  }
  [[nodiscard]] Price to_price(Idx i) const noexcept {
    return hot_floor_ + static_cast<Price>(i);
  }

  // Tier-dispatching level access. These are the only places that know a level
  // can live in two different structures.
  [[nodiscard]] Level*       find_level(Side s, Price p) noexcept;
  [[nodiscard]] const Level* find_level(Side s, Price p) const noexcept;
  [[nodiscard]] Level&       level_for(Side s, Price p);   // creates if absent
  void erase_level(Side s, Price p) noexcept;

  // Best price on a side across BOTH tiers, or kInvalidPrice.
  [[nodiscard]] Price best_px(Side s) const noexcept;

  Idx  alloc_node();
  void free_node(Idx n);
  void link_back(Level& lvl, Idx node);
  void unlink(Level& lvl, Idx node);

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
  void rest_order(const NewOrder& o, Qty qty, Seq stamp, EventLog& out);

  Price floor_ = 0, ceil_ = 0;          // accepted price band (rejects outside)
  Price hot_floor_ = 0, hot_ceil_ = 0;   // flat-ladder band (map outside)
  Idx   span_  = 0;

  std::vector<Level>        bid_lvls_, ask_lvls_;   // hot tier
  std::vector<std::uint64_t> bid_bm_,  ask_bm_;
  Idx   best_bid_ = kNoLevel;   // highest occupied HOT bid index
  Idx   best_ask_ = kNoLevel;   // lowest  occupied HOT ask index

  // Cold tier. Ordered so begin() is the most aggressive price on that side,
  // matching the hot tier's convention.
  std::map<Price, Level, std::greater<Price>> cold_bids_;
  std::map<Price, Level, std::less<Price>>    cold_asks_;

  std::vector<Node> pool_;
  Idx               free_head_ = kNil;

  std::unordered_map<OrderId, Idx> index_;
  SelfTradePolicy stp_      = SelfTradePolicy::None;
  Allocation      alloc_    = Allocation::Fifo;
  int             fifo_pct_ = kDefaultFifoPercent;
  int             time_weight_ = kDefaultTimeWeight;
  Seq             next_seq_ = 1;
  // Scratch reused across matches so the pro-rata pass allocates nothing on
  // the hot path. Cleared, never freed.
  mutable std::vector<Idx> pr_nodes_;
  mutable std::vector<Qty> pr_share_;
  mutable std::vector<long double> pr_suffix_;
};

}  // namespace pricetime
