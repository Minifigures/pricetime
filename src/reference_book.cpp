#include "pricetime/reference_book.hpp"

namespace pricetime {
namespace {

struct MatchResult {
  Qty  filled           = 0;
  bool aggressor_killed = false;  // set by SelfTradePolicy::CancelAggressor
};

// One matching loop, instantiated for both side maps. The only thing that
// differs between buying and selling is the map's comparator, which already
// puts the most aggressive price at begin(), and the crosses() asymmetry.
// Writing this twice is how the two sides drift apart.
template <class SideMap>
MatchResult do_match(SideMap& contra, Side aggressor_side, Price limit,
                     Qty want, OrderId aggr_id, ParticipantId aggr_owner,
                     SelfTradePolicy stp, Seq& next_seq, EventLog& out,
                     std::unordered_map<OrderId, std::pair<Side, Price>>& live,
                     Side contra_side) {
  MatchResult res;
  Qty remaining = want;

  while (remaining > 0 && !contra.empty()) {
    auto lvl_it = contra.begin();
    const Price px = lvl_it->first;
    if (!crosses(aggressor_side, limit, px)) break;

    auto& level = lvl_it->second;
    std::size_t i = 0;
    while (i < level.size() && remaining > 0) {
      RestingOrder& ro = level[i];

      const bool self_match =
          stp != SelfTradePolicy::None &&
          aggr_owner != kAnonymous && ro.owner == aggr_owner;

      if (self_match) {
        if (stp == SelfTradePolicy::CancelResting) {
          Event e;
          e.kind     = Event::Kind::Canceled;
          e.order_id = ro.id;
          e.side     = contra_side;
          e.price    = ro.price;
          e.qty      = ro.open;
          e.seq      = next_seq++;
          out.push_back(e);
          live.erase(ro.id);
          level.erase(level.begin() + static_cast<std::ptrdiff_t>(i));
          continue;  // same index now holds the next order
        }
        res.aggressor_killed = true;
        break;
      }

      const Qty q = std::min(remaining, ro.open);
      out.push_back(make_trade(aggr_id, ro.id, aggressor_side, px, q,
                               next_seq++));
      ro.open   -= q;
      remaining -= q;
      res.filled += q;

      if (ro.open == 0) {
        live.erase(ro.id);
        level.erase(level.begin() + static_cast<std::ptrdiff_t>(i));
      } else {
        ++i;
      }
    }

    const bool drained = level.empty();
    if (drained) contra.erase(lvl_it);
    if (res.aggressor_killed) break;
    if (!drained && remaining > 0) break;  // level had an untouchable head
  }
  return res;
}

template <class SideMap>
Qty count_available(const SideMap& contra, Side aggressor_side, Price limit) {
  Qty total = 0;
  for (const auto& [px, level] : contra) {
    if (!crosses(aggressor_side, limit, px)) break;
    for (const auto& ro : level) total += ro.open;
  }
  return total;
}

}  // namespace

Qty ReferenceBook::available_against(Side aggressor, Price limit) const {
  return aggressor == Side::Buy ? count_available(asks_, aggressor, limit)
                                : count_available(bids_, aggressor, limit);
}

void ReferenceBook::rest(const RestingOrder& ro) {
  if (ro.side == Side::Buy) bids_[ro.price].push_back(ro);
  else                      asks_[ro.price].push_back(ro);
  live_[ro.id] = {ro.side, ro.price};
}

void ReferenceBook::remove_from_level(Side s, Price p, OrderId id) {
  auto strip = [&](auto& side_map) {
    auto it = side_map.find(p);
    if (it == side_map.end()) return;
    auto& lvl = it->second;
    lvl.erase(std::remove_if(lvl.begin(), lvl.end(),
                             [&](const RestingOrder& r) { return r.id == id; }),
              lvl.end());
    if (lvl.empty()) side_map.erase(it);
  };
  if (s == Side::Buy) strip(bids_); else strip(asks_);
  live_.erase(id);
}

void ReferenceBook::submit(const NewOrder& o, EventLog& out) {
  auto reject = [&](RejectReason why) {
    Event e;
    e.kind     = Event::Kind::Rejected;
    e.order_id = o.id;
    e.side     = o.side;
    e.price    = o.price;
    e.qty      = o.qty;
    e.seq      = next_seq_++;
    e.reason   = why;
    out.push_back(e);
  };

  if (o.qty <= 0)                       return reject(RejectReason::InvalidQuantity);
  if (o.id == kInvalidOrderId)          return reject(RejectReason::UnknownOrderId);
  if (live_.count(o.id) != 0)           return reject(RejectReason::DuplicateOrderId);
  if (o.type == OrderType::Limit &&
      (o.price > kMaxPrice || o.price < kMinPrice))
    return reject(RejectReason::InvalidPrice);
  // A market order that would rest has nowhere to rest *at*: there is no
  // price to post. Reject rather than inventing one.
  if (o.type == OrderType::Market && o.tif == TimeInForce::Day)
    return reject(RejectReason::MarketOrderMustNotRest);

  // A market order is a limit order priced through the entire book. One
  // matching path, not two.
  const Price limit = (o.type == OrderType::Market)
                          ? (o.side == Side::Buy ? kMaxPrice : kMinPrice)
                          : o.price;

  if (o.tif == TimeInForce::FOK && available_against(o.side, limit) < o.qty)
    return reject(RejectReason::FokUnfillable);

  Event acc;
  acc.kind     = Event::Kind::Accepted;
  acc.order_id = o.id;
  acc.side     = o.side;
  acc.price    = o.price;
  acc.qty      = o.qty;
  acc.seq      = next_seq_++;
  out.push_back(acc);

  const MatchResult mr =
      (o.side == Side::Buy)
          ? do_match(asks_, o.side, limit, o.qty, o.id, o.owner, stp_,
                     next_seq_, out, live_, Side::Sell)
          : do_match(bids_, o.side, limit, o.qty, o.id, o.owner, stp_,
                     next_seq_, out, live_, Side::Buy);

  const Qty remainder = o.qty - mr.filled;
  if (remainder <= 0) return;

  const bool must_not_rest = o.type == OrderType::Market ||
                             o.tif == TimeInForce::IOC ||
                             o.tif == TimeInForce::FOK ||
                             mr.aggressor_killed;
  if (must_not_rest) {
    Event e;
    e.kind     = Event::Kind::Canceled;
    e.order_id = o.id;
    e.side     = o.side;
    e.price    = o.price;
    e.qty      = remainder;
    e.seq      = next_seq_++;
    out.push_back(e);
    return;
  }

  RestingOrder ro;
  ro.id    = o.id;
  ro.owner = o.owner;
  ro.side  = o.side;
  ro.price = o.price;
  ro.open  = remainder;
  ro.seq   = acc.seq;

  Event rested;
  rested.kind        = Event::Kind::Rested;
  rested.order_id    = o.id;
  rested.side        = o.side;
  rested.price       = o.price;
  rested.qty         = remainder;
  rested.seq         = next_seq_++;
  rested.queue_ahead = qty_at(o.side, o.price);  // measured before insertion
  out.push_back(rested);

  rest(ro);
}

void ReferenceBook::cancel(const CancelOrder& c, EventLog& out) {
  auto it = live_.find(c.id);
  if (it == live_.end()) {
    Event e;
    e.kind     = Event::Kind::Rejected;
    e.order_id = c.id;
    e.seq      = next_seq_++;
    e.reason   = RejectReason::UnknownOrderId;
    out.push_back(e);
    return;
  }
  const auto [side, px] = it->second;

  Qty open = 0;
  auto find_open = [&](auto& side_map) {
    auto lit = side_map.find(px);
    if (lit == side_map.end()) return;
    for (const auto& r : lit->second)
      if (r.id == c.id) open = r.open;
  };
  if (side == Side::Buy) find_open(bids_); else find_open(asks_);

  Event e;
  e.kind     = Event::Kind::Canceled;
  e.order_id = c.id;
  e.side     = side;
  e.price    = px;
  e.qty      = open;
  e.seq      = next_seq_++;
  out.push_back(e);
  remove_from_level(side, px, c.id);
}

void ReferenceBook::replace(const ReplaceOrder& r, EventLog& out) {
  auto it = live_.find(r.id);
  if (it == live_.end()) {
    Event e;
    e.kind     = Event::Kind::Rejected;
    e.order_id = r.id;
    e.seq      = next_seq_++;
    e.reason   = RejectReason::UnknownOrderId;
    out.push_back(e);
    return;
  }
  if (r.qty <= 0) {
    Event e;
    e.kind     = Event::Kind::Rejected;
    e.order_id = r.id;
    e.seq      = next_seq_++;
    e.reason   = RejectReason::InvalidQuantity;
    out.push_back(e);
    return;
  }

  const auto [side, px] = it->second;
  RestingOrder existing;
  auto snapshot = [&](auto& side_map) {
    auto lit = side_map.find(px);
    if (lit == side_map.end()) return;
    for (const auto& ro : lit->second)
      if (ro.id == r.id) existing = ro;
  };
  if (side == Side::Buy) snapshot(bids_); else snapshot(asks_);

  // Priority rule. A pure size *reduction* at the same price keeps the queue
  // position: the participant is only ever giving up size, so nobody behind
  // them is disadvantaged. Any price change, or any size *increase*, goes to
  // the back of the (possibly new) level. Without this asymmetry a participant
  // could hold the front of the queue indefinitely and grow into it, which is
  // exactly the abuse the rule exists to prevent.
  const bool keeps_priority = (r.price == px) && (r.qty <= existing.open);

  Event ev;
  ev.kind     = Event::Kind::Replaced;
  ev.order_id = r.id;
  ev.side     = side;
  ev.price    = r.price;
  ev.qty      = r.qty;
  ev.seq      = next_seq_++;
  out.push_back(ev);

  if (keeps_priority) {
    auto patch = [&](auto& side_map) {
      auto lit = side_map.find(px);
      if (lit == side_map.end()) return;
      for (auto& ro : lit->second)
        if (ro.id == r.id) ro.open = r.qty;
    };
    if (side == Side::Buy) patch(bids_); else patch(asks_);
    return;
  }

  remove_from_level(side, px, r.id);
  RestingOrder ro = existing;
  ro.price = r.price;
  ro.open  = r.qty;
  ro.seq   = ev.seq;  // lost priority: re-stamped to now

  Event rested;
  rested.kind        = Event::Kind::Rested;
  rested.order_id    = r.id;
  rested.side        = side;
  rested.price       = r.price;
  rested.qty         = r.qty;
  rested.seq         = next_seq_++;
  rested.queue_ahead = qty_at(side, r.price);
  out.push_back(rested);

  rest(ro);
}

std::vector<std::pair<Price, Qty>> ReferenceBook::depth(
    Side s, std::size_t levels) const {
  std::vector<std::pair<Price, Qty>> out;
  auto walk = [&](const auto& side_map) {
    for (const auto& [px, lvl] : side_map) {
      if (out.size() >= levels) break;
      Qty total = 0;
      for (const auto& ro : lvl) total += ro.open;
      out.emplace_back(px, total);
    }
  };
  if (s == Side::Buy) walk(bids_); else walk(asks_);
  return out;
}

}  // namespace pricetime
