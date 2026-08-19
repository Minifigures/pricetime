#include "pricetime/book.hpp"

#include <algorithm>

namespace pricetime {

Book::Book(Price floor_px, Price ceil_px, SelfTradePolicy stp,
           std::size_t expected_orders)
    : floor_(floor_px), ceil_(ceil_px), stp_(stp) {
  const Price raw_span = ceil_px - floor_px + 1;
  span_ = static_cast<Idx>(raw_span > 0 ? raw_span : 0);
  bid_lvls_.resize(span_);
  ask_lvls_.resize(span_);
  const std::size_t words = (static_cast<std::size_t>(span_) + 63u) / 64u;
  bid_bm_.assign(words, 0ull);
  ask_bm_.assign(words, 0ull);

  // Pre-size the pool so steady-state operation never allocates. Growth is
  // still handled (below) because a hard cap would turn a load spike into a
  // crash, but in the benchmark path it never triggers.
  pool_.resize(expected_orders);
  for (std::size_t i = 0; i < pool_.size(); ++i) {
    pool_[i].next = (i + 1 < pool_.size()) ? static_cast<Idx>(i + 1) : kNil;
  }
  free_head_ = pool_.empty() ? kNil : 0u;
}

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
    if (word != 0ull) {
      return static_cast<Idx>((w << 6) + 63u -
             static_cast<std::size_t>(std::countl_zero(word)));
    }
    if (w == 0) return kNoLevel;
    --w;
    word = bm[w];
  }
}

void Book::refresh_best(Side s) noexcept {
  if (s == Side::Buy) {
    best_bid_ = (span_ == 0) ? kNoLevel : scan_down(bid_bm_, span_ - 1u);
  } else {
    best_ask_ = scan_up(ask_bm_, 0u);
  }
}

void Book::link_back(std::vector<Level>& side, Idx lvl, Idx node) {
  Level& L = side[lvl];
  pool_[node].prev = L.tail;
  pool_[node].next = kNil;
  if (L.tail != kNil) pool_[L.tail].next = node; else L.head = node;
  L.tail = node;
  L.total += pool_[node].open;
}

void Book::unlink(std::vector<Level>& side, Idx lvl, Idx node) {
  Level& L = side[lvl];
  const Idx p = pool_[node].prev, n = pool_[node].next;
  if (p != kNil) pool_[p].next = n; else L.head = n;
  if (n != kNil) pool_[n].prev = p; else L.tail = p;
  L.total -= pool_[node].open;
  pool_[node].prev = pool_[node].next = kNil;
}

Qty Book::available_against(Side aggressor, Price limit) const {
  Qty total = 0;
  if (aggressor == Side::Buy) {
    for (Idx l = best_ask_; l != kNoLevel; l = scan_up(ask_bm_, l + 1u)) {
      if (!crosses(aggressor, limit, to_price(l))) break;
      total += ask_lvls_[l].total;
    }
  } else {
    for (Idx l = best_bid_; l != kNoLevel;
         l = (l == 0 ? kNoLevel : scan_down(bid_bm_, l - 1u))) {
      if (!crosses(aggressor, limit, to_price(l))) break;
      total += bid_lvls_[l].total;
    }
  }
  return total;
}

Qty Book::qty_at(Side s, Price p) const noexcept {
  if (!in_band(p)) return 0;
  const Idx l = to_idx(p);
  return (s == Side::Buy) ? bid_lvls_[l].total : ask_lvls_[l].total;
}

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

  const bool buying = (o.side == Side::Buy);
  auto& clvls = buying ? ask_lvls_ : bid_lvls_;
  const Side cside = opposite(o.side);

  Qty  remaining = o.qty;
  bool killed    = false;
  Idx  lvl       = buying ? best_ask_ : best_bid_;

  while (remaining > 0 && lvl != kNoLevel) {
    const Price px = to_price(lvl);
    if (!crosses(o.side, limit, px)) break;

    Level& L = clvls[lvl];
    while (L.head != kNil && remaining > 0) {
      const Idx  n  = L.head;
      Node&      rn = pool_[n];

      const bool self_match = stp_ != SelfTradePolicy::None &&
                              o.owner != kAnonymous && rn.owner == o.owner;
      if (self_match) {
        if (stp_ == SelfTradePolicy::CancelResting) {
          Event e;
          e.kind = Event::Kind::Canceled; e.order_id = rn.id; e.side = cside;
          e.price = px; e.qty = rn.open; e.seq = next_seq_++;
          out.push_back(e);
          index_.erase(rn.id);
          unlink(clvls, lvl, n);
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
        L.total += 0;              // node already discounted above
        const Idx nx = rn.next;
        // unlink without double-counting total (open is now 0)
        const Idx p = rn.prev;
        if (p != kNil) pool_[p].next = nx; else L.head = nx;
        if (nx != kNil) pool_[nx].prev = p; else L.tail = p;
        free_node(n);
      }
    }

    const bool drained = (L.head == kNil);
    if (drained) {
      set_occupied(cside, lvl, false);
      lvl = buying ? scan_up(ask_bm_, lvl + 1u)
                   : (lvl == 0 ? kNoLevel : scan_down(bid_bm_, lvl - 1u));
    }
    if (killed) break;
    if (!drained && remaining > 0) break;
  }
  refresh_best(cside);

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

  const Idx nidx = alloc_node();
  Node& nn = pool_[nidx];
  nn.id = o.id; nn.owner = o.owner; nn.open = remaining;
  nn.seq = acc.seq; nn.side = o.side; nn.lvl = to_idx(o.price);
  auto& own = buying ? bid_lvls_ : ask_lvls_;
  link_back(own, nn.lvl, nidx);
  set_occupied(o.side, nn.lvl, true);
  index_[o.id] = nidx;
  refresh_best(o.side);
}

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
  auto& lvls = (node.side == Side::Buy) ? bid_lvls_ : ask_lvls_;

  Event e;
  e.kind = Event::Kind::Canceled; e.order_id = c.id; e.side = node.side;
  e.price = to_price(node.lvl); e.qty = node.open; e.seq = next_seq_++;
  out.push_back(e);

  unlink(lvls, node.lvl, n);
  if (lvls[node.lvl].head == kNil) set_occupied(node.side, node.lvl, false);
  free_node(n);
  index_.erase(it);
  refresh_best(node.side);
}

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
  const Price old_px  = to_price(existing.lvl);
  auto& lvls = (existing.side == Side::Buy) ? bid_lvls_ : ask_lvls_;

  const bool keeps_priority = (r.price == old_px) && (r.qty <= existing.open);

  Event ev;
  ev.kind = Event::Kind::Replaced; ev.order_id = r.id; ev.side = existing.side;
  ev.price = r.price; ev.qty = r.qty; ev.seq = next_seq_++;
  out.push_back(ev);

  if (keeps_priority) {
    Level& L = lvls[existing.lvl];
    L.total -= (existing.open - r.qty);
    pool_[n].open = r.qty;
    return;
  }

  unlink(lvls, existing.lvl, n);
  if (lvls[existing.lvl].head == kNil)
    set_occupied(existing.side, existing.lvl, false);
  free_node(n);
  index_.erase(it);
  refresh_best(existing.side);

  if (!in_band(r.price)) return;  // replaced out of band: order is gone

  const Idx nidx = alloc_node();
  Node& nn = pool_[nidx];
  nn.id = existing.id; nn.owner = existing.owner; nn.open = r.qty;
  nn.seq = ev.seq; nn.side = existing.side; nn.lvl = to_idx(r.price);
  auto& own = (existing.side == Side::Buy) ? bid_lvls_ : ask_lvls_;
  link_back(own, nn.lvl, nidx);
  set_occupied(existing.side, nn.lvl, true);
  index_[existing.id] = nidx;
  refresh_best(existing.side);
}

std::vector<std::pair<Price, Qty>> Book::depth(Side s,
                                               std::size_t levels) const {
  std::vector<std::pair<Price, Qty>> out;
  if (s == Side::Buy) {
    for (Idx l = best_bid_; l != kNoLevel && out.size() < levels;
         l = (l == 0 ? kNoLevel : scan_down(bid_bm_, l - 1u)))
      out.emplace_back(to_price(l), bid_lvls_[l].total);
  } else {
    for (Idx l = best_ask_; l != kNoLevel && out.size() < levels;
         l = scan_up(ask_bm_, l + 1u))
      out.emplace_back(to_price(l), ask_lvls_[l].total);
  }
  return out;
}

}  // namespace pricetime
