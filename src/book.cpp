#include "pricetime/book.hpp"

#include <algorithm>
#include <cmath>

namespace pricetime {

Book::Book(Price floor_px, Price ceil_px, SelfTradePolicy stp,
           std::size_t expected_orders, Price hot_ticks, Allocation alloc,
           int fifo_percent, int time_weight)
    : floor_(floor_px), ceil_(ceil_px), stp_(stp), alloc_(alloc),
      fifo_pct_(std::clamp(fifo_percent, 0, 100)),
      time_weight_(std::clamp(time_weight, 1, 32)) {
  // The hot ladder is centred on the accepted band and clamped to it. When the
  // accepted band is already small (the synthetic benchmarks, most tests) the
  // whole thing is hot and the cold tier never sees a level.
  const Price accepted = ceil_ - floor_ + 1;
  const Price hot_w = std::clamp<Price>(hot_ticks, 1, accepted > 0 ? accepted : 1);
  const Price mid = floor_ + accepted / 2;
  hot_floor_ = std::max<Price>(floor_, mid - hot_w / 2);
  hot_ceil_  = std::min<Price>(ceil_, hot_floor_ + hot_w - 1);
  if (hot_ceil_ < hot_floor_) hot_ceil_ = hot_floor_;

  const Price raw_span = hot_ceil_ - hot_floor_ + 1;
  span_ = static_cast<Idx>(raw_span > 0 ? raw_span : 0);
  bid_lvls_.resize(span_);
  ask_lvls_.resize(span_);
  const std::size_t words = (static_cast<std::size_t>(span_) + 63u) / 64u;
  bid_bm_.assign(words, 0ull);
  ask_bm_.assign(words, 0ull);

  pool_.resize(expected_orders);
  for (std::size_t i = 0; i < pool_.size(); ++i)
    pool_[i].next = (i + 1 < pool_.size()) ? static_cast<Idx>(i + 1) : kNil;
  free_head_ = pool_.empty() ? kNil : 0u;
}

// ---------------------------------------------------------------------- pool

Book::Idx Book::alloc_node() {
  if (free_head_ == kNil) {
    const std::size_t old = pool_.size();
    pool_.resize(old == 0 ? 1024 : old * 2);
    for (std::size_t i = old; i < pool_.size(); ++i)
      pool_[i].next = (i + 1 < pool_.size()) ? static_cast<Idx>(i + 1) : kNil;
    free_head_ = static_cast<Idx>(old);
  }
  const Idx n = free_head_;
  free_head_ = pool_[n].next;
  pool_[n] = Node{};
  return n;
}

void Book::free_node(Idx n) {
  pool_[n] = Node{};
  pool_[n].next = free_head_;
  free_head_ = n;
}

void Book::link_back(Level& L, Idx node) {
  pool_[node].prev = L.tail;
  pool_[node].next = kNil;
  if (L.tail != kNil) pool_[L.tail].next = node; else L.head = node;
  L.tail = node;
  L.total += pool_[node].open;
}

void Book::unlink(Level& L, Idx node) {
  const Idx p = pool_[node].prev, n = pool_[node].next;
  if (p != kNil) pool_[p].next = n; else L.head = n;
  if (n != kNil) pool_[n].prev = p; else L.tail = p;
  L.total -= pool_[node].open;
  pool_[node].prev = pool_[node].next = kNil;
}

// --------------------------------------------------------------- hot bitmap

void Book::set_occupied(Side s, Idx lvl, bool on) noexcept {
  auto& bm = (s == Side::Buy) ? bid_bm_ : ask_bm_;
  const std::size_t w = static_cast<std::size_t>(lvl) >> 6;
  const std::uint64_t bit = 1ull << (static_cast<unsigned>(lvl) & 63u);
  if (on) bm[w] |= bit; else bm[w] &= ~bit;
}

Book::Idx Book::scan_up(const std::vector<std::uint64_t>& bm,
                        Idx from) const noexcept {
  if (span_ == 0 || from >= span_) return kNoLevel;
  std::size_t w = static_cast<std::size_t>(from) >> 6;
  std::uint64_t word = bm[w] & (~0ull << (static_cast<unsigned>(from) & 63u));
  for (;;) {
    if (word != 0ull) {
      const Idx hit = static_cast<Idx>((w << 6) +
                      static_cast<std::size_t>(std::countr_zero(word)));
      return hit < span_ ? hit : kNoLevel;
    }
    if (++w >= bm.size()) return kNoLevel;
    word = bm[w];
  }
}

Book::Idx Book::scan_down(const std::vector<std::uint64_t>& bm,
                          Idx from) const noexcept {
  if (span_ == 0) return kNoLevel;
  if (from >= span_) from = span_ - 1u;
  std::size_t w = static_cast<std::size_t>(from) >> 6;
  const unsigned bit = static_cast<unsigned>(from) & 63u;
  const std::uint64_t mask =
      (bit == 63u) ? ~0ull : ((1ull << (bit + 1u)) - 1ull);
  std::uint64_t word = bm[w] & mask;
  for (;;) {
    if (word != 0ull)
      return static_cast<Idx>((w << 6) + 63u -
             static_cast<std::size_t>(std::countl_zero(word)));
    if (w == 0) return kNoLevel;
    --w;
    word = bm[w];
  }
}

void Book::best_after_insert(Side s, Idx lvl) noexcept {
  if (s == Side::Buy) {
    if (best_bid_ == kNoLevel || lvl > best_bid_) best_bid_ = lvl;
  } else {
    if (best_ask_ == kNoLevel || lvl < best_ask_) best_ask_ = lvl;
  }
}

void Book::best_after_remove(Side s, Idx lvl) noexcept {
  if (s == Side::Buy) {
    if (best_bid_ != lvl) return;
    best_bid_ = (lvl == 0) ? kNoLevel : scan_down(bid_bm_, lvl - 1u);
  } else {
    if (best_ask_ != lvl) return;
    best_ask_ = scan_up(ask_bm_, lvl + 1u);
  }
}

// ------------------------------------------------------------ tier dispatch

Level* Book::find_level(Side s, Price p) noexcept {
  return const_cast<Level*>(std::as_const(*this).find_level(s, p));
}

const Level* Book::find_level(Side s, Price p) const noexcept {
  if (p == kInvalidPrice) return nullptr;
  if (is_hot(p)) {
    const Idx i = to_idx(p);
    const Level& L = (s == Side::Buy) ? bid_lvls_[i] : ask_lvls_[i];
    return L.empty() ? nullptr : &L;
  }
  if (s == Side::Buy) {
    const auto it = cold_bids_.find(p);
    return it == cold_bids_.end() ? nullptr : &it->second;
  }
  const auto it = cold_asks_.find(p);
  return it == cold_asks_.end() ? nullptr : &it->second;
}

Level& Book::level_for(Side s, Price p) {
  if (is_hot(p)) {
    const Idx i = to_idx(p);
    return (s == Side::Buy) ? bid_lvls_[i] : ask_lvls_[i];
  }
  return (s == Side::Buy) ? cold_bids_[p] : cold_asks_[p];
}

void Book::erase_level(Side s, Price p) noexcept {
  if (is_hot(p)) {
    const Idx i = to_idx(p);
    set_occupied(s, i, false);
    best_after_remove(s, i);
    return;
  }
  if (s == Side::Buy) cold_bids_.erase(p); else cold_asks_.erase(p);
}

Price Book::best_px(Side s) const noexcept {
  if (s == Side::Buy) {
    const Price hot = (best_bid_ == kNoLevel) ? kInvalidPrice : to_price(best_bid_);
    const Price cold = cold_bids_.empty() ? kInvalidPrice : cold_bids_.begin()->first;
    if (hot == kInvalidPrice) return cold;
    if (cold == kInvalidPrice) return hot;
    return std::max(hot, cold);   // bids: higher is better
  }
  const Price hot = (best_ask_ == kNoLevel) ? kInvalidPrice : to_price(best_ask_);
  const Price cold = cold_asks_.empty() ? kInvalidPrice : cold_asks_.begin()->first;
  if (hot == kInvalidPrice) return cold;
  if (cold == kInvalidPrice) return hot;
  return std::min(hot, cold);     // asks: lower is better
}

Qty Book::qty_at(Side s, Price p) const noexcept {
  const Level* L = find_level(s, p);
  return L == nullptr ? 0 : L->total;
}

Qty Book::available_against(Side aggressor, Price limit) const {
  // Walks both tiers merged in price order, stopping at the first level the
  // aggressor cannot reach. Used only by the FOK precheck.
  Qty total = 0;
  const bool buying = (aggressor == Side::Buy);
  Idx hot = buying ? best_ask_ : best_bid_;

  if (buying) {
    auto cit = cold_asks_.begin();
    for (;;) {
      const Price hp = (hot == kNoLevel) ? kInvalidPrice : to_price(hot);
      const Price cp = (cit == cold_asks_.end()) ? kInvalidPrice : cit->first;
      if (hp == kInvalidPrice && cp == kInvalidPrice) break;
      const bool take_hot = (cp == kInvalidPrice) || (hp != kInvalidPrice && hp <= cp);
      const Price px = take_hot ? hp : cp;
      if (!crosses(aggressor, limit, px)) break;
      total += take_hot ? ask_lvls_[hot].total : cit->second.total;
      if (take_hot) hot = scan_up(ask_bm_, hot + 1u); else ++cit;
    }
  } else {
    auto cit = cold_bids_.begin();
    for (;;) {
      const Price hp = (hot == kNoLevel) ? kInvalidPrice : to_price(hot);
      const Price cp = (cit == cold_bids_.end()) ? kInvalidPrice : cit->first;
      if (hp == kInvalidPrice && cp == kInvalidPrice) break;
      const bool take_hot = (cp == kInvalidPrice) || (hp != kInvalidPrice && hp >= cp);
      const Price px = take_hot ? hp : cp;
      if (!crosses(aggressor, limit, px)) break;
      total += take_hot ? bid_lvls_[hot].total : cit->second.total;
      if (take_hot) hot = (hot == 0 ? kNoLevel : scan_down(bid_bm_, hot - 1u));
      else ++cit;
    }
  }
  return total;
}

// -------------------------------------------------------------------- rest

void Book::rest_order(const NewOrder& o, Qty qty, Seq stamp, EventLog& out) {
  Event rested;
  rested.kind        = Event::Kind::Rested;
  rested.order_id    = o.id;
  rested.side        = o.side;
  rested.price       = o.price;
  rested.qty         = qty;
  rested.seq         = next_seq_++;
  rested.queue_ahead = qty_at(o.side, o.price);  // measured before insertion
  out.push_back(rested);

  const Idx nidx = alloc_node();
  Node& nn = pool_[nidx];
  nn.id = o.id; nn.owner = o.owner; nn.open = qty;
  nn.seq = stamp; nn.side = o.side; nn.price = o.price;
  Level& L = level_for(o.side, o.price);
  link_back(L, nidx);
  if (is_hot(o.price)) {
    const Idx i = to_idx(o.price);
    set_occupied(o.side, i, true);
    best_after_insert(o.side, i);
  }
  index_[o.id] = nidx;
}

// ------------------------------------------------------------------ submit

void Book::submit(const NewOrder& o, EventLog& out) {
  auto reject = [&](RejectReason why) {
    Event e;
    e.kind = Event::Kind::Rejected; e.order_id = o.id; e.side = o.side;
    e.price = o.price; e.qty = o.qty; e.seq = next_seq_++; e.reason = why;
    out.push_back(e);
  };

  if (o.qty <= 0)              return reject(RejectReason::InvalidQuantity);
  if (o.id == kInvalidOrderId) return reject(RejectReason::UnknownOrderId);
  if (index_.count(o.id) != 0) return reject(RejectReason::DuplicateOrderId);
  if (o.type == OrderType::Limit && !in_band(o.price))
    return reject(RejectReason::InvalidPrice);
  if (o.type == OrderType::Market && o.tif == TimeInForce::Day)
    return reject(RejectReason::MarketOrderMustNotRest);

  const Price limit = (o.type == OrderType::Market)
                          ? (o.side == Side::Buy ? ceil_ : floor_)
                          : o.price;

  if (o.tif == TimeInForce::FOK && available_against(o.side, limit) < o.qty)
    return reject(RejectReason::FokUnfillable);

  Event acc;
  acc.kind = Event::Kind::Accepted; acc.order_id = o.id; acc.side = o.side;
  acc.price = o.price; acc.qty = o.qty; acc.seq = next_seq_++;
  out.push_back(acc);

  const Side cside = opposite(o.side);
  Qty  remaining = o.qty;
  bool killed    = false;

  // Walk the contra side best-first across both tiers. best_px() is O(1), so
  // recomputing it after draining a level is cheaper than threading a cursor
  // through two structures with different iterator types.
  for (;;) {
    if (remaining <= 0) break;
    const Price px = best_px(cside);
    if (px == kInvalidPrice) break;
    if (!crosses(o.side, limit, px)) break;

    Level* Lp = find_level(cside, px);
    if (Lp == nullptr) break;
    Level& L = *Lp;

    // Split: a fixed percentage placed FIFO before the proportional pass.
    if (alloc_ == Allocation::Split && remaining > 0 && L.head != kNil) {
      Qty fifo_part = (remaining * fifo_pct_) / 100;
      Idx n = L.head;
      while (n != kNil && fifo_part > 0 && remaining > 0) {
        const Idx nxt = pool_[n].next;
        const bool self_match = stp_ != SelfTradePolicy::None &&
                                o.owner != kAnonymous && pool_[n].owner == o.owner;
        if (self_match) { n = nxt; continue; }
        const Qty q = std::min(fifo_part, pool_[n].open);
        out.push_back(make_trade(o.id, pool_[n].id, o.side, px, q, next_seq_++));
        pool_[n].open -= q; L.total -= q; remaining -= q; fifo_part -= q;
        if (pool_[n].open == 0) {
          index_.erase(pool_[n].id);
          const Idx p2 = pool_[n].prev;
          if (p2 != kNil) pool_[p2].next = nxt; else L.head = nxt;
          if (nxt != kNil) pool_[nxt].prev = p2; else L.tail = p2;
          free_node(n);
        }
        n = nxt;
      }
    }

    // CME's FIFO Exception: when the aggressor takes the whole level, FIFO is
    // applied in place of the configured algorithm. Every resting order fills
    // completely either way, so the outcome is identical and the proportional
    // work is pure waste.
    const bool sweeps_level = remaining >= L.total;

    // Proportional pass. Mirrors the reference implementation exactly; the
    // differential fuzz across policies is what keeps them that way.
    if ((alloc_ == Allocation::ProRata || alloc_ == Allocation::Split ||
         alloc_ == Allocation::TimeWeighted) &&
        remaining > 0 && L.head != kNil && !sweeps_level) {
      pr_nodes_.clear();
      pr_share_.clear();
      Qty total = 0;
      for (Idx n = L.head; n != kNil; n = pool_[n].next) {
        const bool self_match = stp_ != SelfTradePolicy::None &&
                                o.owner != kAnonymous &&
                                pool_[n].owner == o.owner;
        pr_nodes_.push_back(n);
        if (self_match) { pr_share_.push_back(-1); }        // -1 marks skip
        else            { pr_share_.push_back(0); total += pool_[n].open; }
      }
      if (total > 0 && alloc_ == Allocation::TimeWeighted) {
        // f_j(k) = (Q_j^k - Q_{j+1}^k) / V^k over the eligible suffix sums.
        const std::size_t n = pr_nodes_.size();
        pr_suffix_.assign(n + 1, 0.0L);
        for (std::size_t k = n; k-- > 0;)
          pr_suffix_[k] = pr_suffix_[k + 1] +
              (pr_share_[k] < 0 ? 0.0L
                                : static_cast<long double>(pool_[pr_nodes_[k]].open));
        const long double V = pr_suffix_[0];
        const long double Vk = std::pow(V, static_cast<long double>(time_weight_));
        for (std::size_t k = 0; k < n; ++k) {
          if (pr_share_[k] < 0) continue;
          const long double a = std::pow(pr_suffix_[k],     static_cast<long double>(time_weight_));
          const long double b = std::pow(pr_suffix_[k + 1], static_cast<long double>(time_weight_));
          Qty q = static_cast<Qty>(((a - b) / Vk) * static_cast<long double>(remaining));
          q = std::min(q, pool_[pr_nodes_[k]].open);
          pr_share_[k] = q > 0 ? q : 0;
        }
      } else if (total > 0) {
        for (std::size_t k = 0; k < pr_nodes_.size(); ++k) {
          if (pr_share_[k] < 0) continue;
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
          const auto num = static_cast<__int128>(pool_[pr_nodes_[k]].open) * remaining;
#pragma GCC diagnostic pop
          Qty q = static_cast<Qty>(num / total);
          q = std::min(q, pool_[pr_nodes_[k]].open);
          pr_share_[k] = q;
        }
      }

      // Emit, shared by every proportional policy. This loop used to sit
      // INSIDE the pro-rata branch, so the time-weighted kernel computed its
      // allocations correctly and then discarded them, silently degrading to
      // FIFO. Every existing test still passed, because none of them ran the
      // time-weighted policy yet. Shared computation belongs outside the
      // branch that happened to be written first.
      if (total > 0) {
        for (std::size_t k = 0; k < pr_nodes_.size() && remaining > 0; ++k) {
          if (pr_share_[k] <= 0) continue;
          const Idx n = pr_nodes_[k];
          const Qty q = std::min(pr_share_[k], remaining);
          out.push_back(make_trade(o.id, pool_[n].id, o.side, px, q, next_seq_++));
          pool_[n].open -= q;
          L.total       -= q;
          remaining     -= q;
          if (pool_[n].open == 0) {
            index_.erase(pool_[n].id);
            const Idx nx = pool_[n].next, p2 = pool_[n].prev;
            if (p2 != kNil) pool_[p2].next = nx; else L.head = nx;
            if (nx != kNil) pool_[nx].prev = p2; else L.tail = p2;
            free_node(n);
          }
        }
      }
    }

    while (L.head != kNil && remaining > 0) {
      const Idx n = L.head;
      Node& rn = pool_[n];

      const bool self_match = stp_ != SelfTradePolicy::None &&
                              o.owner != kAnonymous && rn.owner == o.owner;
      if (self_match) {
        if (stp_ == SelfTradePolicy::CancelResting) {
          Event e;
          e.kind = Event::Kind::Canceled; e.order_id = rn.id; e.side = cside;
          e.price = px; e.qty = rn.open; e.seq = next_seq_++;
          out.push_back(e);
          index_.erase(rn.id);
          unlink(L, n);
          free_node(n);
          continue;
        }
        killed = true;
        break;
      }

      const Qty q = std::min(remaining, rn.open);
      out.push_back(make_trade(o.id, rn.id, o.side, px, q, next_seq_++));
      rn.open   -= q;
      L.total   -= q;
      remaining -= q;
      if (rn.open == 0) {
        index_.erase(rn.id);
        const Idx nx = rn.next, p = rn.prev;
        if (p != kNil) pool_[p].next = nx; else L.head = nx;
        if (nx != kNil) pool_[nx].prev = p; else L.tail = p;
        free_node(n);
      }
    }

    const bool drained = (L.head == kNil);
    if (drained) erase_level(cside, px);
    if (killed) break;
    if (!drained) break;   // level survived with an untouchable head
  }

  if (remaining <= 0) return;

  const bool must_not_rest = o.type == OrderType::Market ||
                             o.tif == TimeInForce::IOC ||
                             o.tif == TimeInForce::FOK || killed;
  if (must_not_rest) {
    Event e;
    e.kind = Event::Kind::Canceled; e.order_id = o.id; e.side = o.side;
    e.price = o.price; e.qty = remaining; e.seq = next_seq_++;
    out.push_back(e);
    return;
  }
  rest_order(o, remaining, acc.seq, out);
}

// ------------------------------------------------------------------ cancel

void Book::cancel(const CancelOrder& c, EventLog& out) {
  auto it = index_.find(c.id);
  if (it == index_.end()) {
    Event e;
    e.kind = Event::Kind::Rejected; e.order_id = c.id; e.seq = next_seq_++;
    e.reason = RejectReason::UnknownOrderId;
    out.push_back(e);
    return;
  }
  const Idx  n    = it->second;
  const Node node = pool_[n];

  Event e;
  e.kind = Event::Kind::Canceled; e.order_id = c.id; e.side = node.side;
  e.price = node.price; e.qty = node.open; e.seq = next_seq_++;
  out.push_back(e);

  Level& L = level_for(node.side, node.price);
  unlink(L, n);
  if (L.head == kNil) erase_level(node.side, node.price);
  free_node(n);
  index_.erase(it);
}

// ----------------------------------------------------------------- replace

void Book::replace(const ReplaceOrder& r, EventLog& out) {
  auto it = index_.find(r.id);
  if (it == index_.end()) {
    Event e;
    e.kind = Event::Kind::Rejected; e.order_id = r.id; e.seq = next_seq_++;
    e.reason = RejectReason::UnknownOrderId;
    out.push_back(e);
    return;
  }
  if (r.qty <= 0) {
    Event e;
    e.kind = Event::Kind::Rejected; e.order_id = r.id; e.seq = next_seq_++;
    e.reason = RejectReason::InvalidQuantity;
    out.push_back(e);
    return;
  }

  const Idx  n        = it->second;
  const Node existing = pool_[n];
  const Price old_px  = existing.price;

  const bool keeps_priority = (r.price == old_px) && (r.qty <= existing.open);

  Event ev;
  ev.kind = Event::Kind::Replaced; ev.order_id = r.id; ev.side = existing.side;
  ev.price = r.price; ev.qty = r.qty; ev.seq = next_seq_++;
  out.push_back(ev);

  if (keeps_priority) {
    Level& L = level_for(existing.side, old_px);
    L.total -= (existing.open - r.qty);
    pool_[n].open = r.qty;
    return;
  }

  Level& L = level_for(existing.side, old_px);
  unlink(L, n);
  if (L.head == kNil) erase_level(existing.side, old_px);
  free_node(n);
  index_.erase(it);

  if (!in_band(r.price)) return;  // replaced out of band: order is gone

  NewOrder shim;
  shim.id = existing.id; shim.owner = existing.owner;
  shim.side = existing.side; shim.price = r.price;
  rest_order(shim, r.qty, ev.seq, out);
}

// ------------------------------------------------------------------- depth

std::vector<std::pair<Price, Qty>> Book::depth(Side s,
                                               std::size_t levels) const {
  std::vector<std::pair<Price, Qty>> out;
  const bool buy = (s == Side::Buy);
  Idx hot = buy ? best_bid_ : best_ask_;

  if (buy) {
    auto cit = cold_bids_.begin();
    while (out.size() < levels) {
      const Price hp = (hot == kNoLevel) ? kInvalidPrice : to_price(hot);
      const Price cp = (cit == cold_bids_.end()) ? kInvalidPrice : cit->first;
      if (hp == kInvalidPrice && cp == kInvalidPrice) break;
      const bool take_hot = (cp == kInvalidPrice) || (hp != kInvalidPrice && hp >= cp);
      if (take_hot) { out.emplace_back(hp, bid_lvls_[hot].total);
                      hot = (hot == 0 ? kNoLevel : scan_down(bid_bm_, hot - 1u)); }
      else          { out.emplace_back(cp, cit->second.total); ++cit; }
    }
  } else {
    auto cit = cold_asks_.begin();
    while (out.size() < levels) {
      const Price hp = (hot == kNoLevel) ? kInvalidPrice : to_price(hot);
      const Price cp = (cit == cold_asks_.end()) ? kInvalidPrice : cit->first;
      if (hp == kInvalidPrice && cp == kInvalidPrice) break;
      const bool take_hot = (cp == kInvalidPrice) || (hp != kInvalidPrice && hp <= cp);
      if (take_hot) { out.emplace_back(hp, ask_lvls_[hot].total);
                      hot = scan_up(ask_bm_, hot + 1u); }
      else          { out.emplace_back(cp, cit->second.total); ++cit; }
    }
  }
  return out;
}

}  // namespace pricetime
