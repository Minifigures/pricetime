#include "pricetime/reference_book.hpp"

#include <cmath>

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
// Pro-rata allocation across one price level.
//
// Each eligible order is offered floor(open_i * want / total). The floor is not
// a detail: with any realistic mix of sizes the shares almost never divide
// evenly, so a remainder always exists and something has to place it. CME
// resolves that with a FIFO step, and states outright that pro-rata is never
// the last step of an algorithm for exactly this reason.
//
// Timestamps are deliberately NOT consulted in the proportional step. That is
// what makes pro-rata pro-rata: a large order that arrived a moment ago
// outranks a small one that has been resting all day, which is the whole
// economic point and the whole reason it changes participant behaviour.
//
// Returns per-order allocations aligned to `level`, summing to at most `want`.
// Time-weighted shares: f_j(k) = (Q_j^k - Q_{j+1}^k) / V^k, with Q_j the
// cumulative eligible volume from order j onward in TIME order and V = Q_1.
//
// The exponent is what makes this one function instead of five. k=1 collapses
// to Q_j - Q_{j+1} over V, which is just q_j/V, pure pro-rata. Larger k pushes
// weight toward the front of the queue, and the limit is FIFO. Eurex's
// Time-Pro-Rata recursion expands by induction to exactly this at k=2, and ICE
// runs k=2 on Euribor and k=4 on Short Sterling.
//
// Computed in long double rather than integers because Q^k overflows int64 for
// any realistic book beyond k=2: a level holding a million lots cubed is 10^18
// and a fourth power is 10^24. Eurex warns its own implementers that the
// arithmetic precision applied affects the result, so the choice is stated
// rather than left implicit. The shares telescope to 1 exactly in exact
// arithmetic, so any residual from rounding falls through to the FIFO step.
std::vector<Qty> time_weighted_shares(const std::vector<RestingOrder>& level,
                                      const std::vector<bool>& eligible,
                                      Qty want, int k) {
  std::vector<Qty> share(level.size(), 0);
  const std::size_t n = level.size();
  if (n == 0 || want <= 0) return share;

  // Suffix sums of eligible quantity, in time order.
  std::vector<long double> suffix(n + 1, 0.0L);
  for (std::size_t i = n; i-- > 0;)
    suffix[i] = suffix[i + 1] +
                (eligible[i] ? static_cast<long double>(level[i].open) : 0.0L);
  const long double V = suffix[0];
  if (V <= 0.0L) return share;

  const long double Vk = std::pow(V, static_cast<long double>(k));
  for (std::size_t i = 0; i < n; ++i) {
    if (!eligible[i]) continue;
    const long double a = std::pow(suffix[i],     static_cast<long double>(k));
    const long double b = std::pow(suffix[i + 1], static_cast<long double>(k));
    const long double f = (a - b) / Vk;
    Qty q = static_cast<Qty>(f * static_cast<long double>(want));
    q = std::min(q, level[i].open);
    q = std::min(q, want);
    share[i] = q > 0 ? q : 0;
  }
  return share;
}

std::vector<Qty> prorata_shares(const std::vector<RestingOrder>& level,
                                const std::vector<bool>& eligible, Qty want) {
  std::vector<Qty> share(level.size(), 0);
  Qty total = 0;
  for (std::size_t i = 0; i < level.size(); ++i)
    if (eligible[i]) total += level[i].open;
  if (total <= 0 || want <= 0) return share;

  for (std::size_t i = 0; i < level.size(); ++i) {
    if (!eligible[i]) continue;
    // 128-bit intermediate. open and want are both int64 and their product
    // overflows int64 for realistic book sizes; a silent wrap would hand
    // someone an enormous fill. __int128 is a GCC/Clang extension rather than
    // standard C++, so the pedantic warning is silenced here specifically
    // rather than dropped from the build, and the alternative (splitting the
    // multiply around the division) is harder to read for no benefit on the
    // two compilers this targets.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
    const auto num = static_cast<__int128>(level[i].open) * want;
#pragma GCC diagnostic pop
    Qty q = static_cast<Qty>(num / total);
    q = std::min(q, level[i].open);
    q = std::min(q, want);
    share[i] = q;
  }
  return share;
}

template <class SideMap>
MatchResult do_match(SideMap& contra, Side aggressor_side, Price limit,
                     Qty want, OrderId aggr_id, ParticipantId aggr_owner,
                     SelfTradePolicy stp, Seq& next_seq, EventLog& out,
                     std::unordered_map<OrderId, std::pair<Side, Price>>& live,
                     Side contra_side, Allocation alloc, int fifo_pct,
                     int time_weight) {
  MatchResult res;
  Qty remaining = want;

  while (remaining > 0 && !contra.empty()) {
    auto lvl_it = contra.begin();
    const Price px = lvl_it->first;
    if (!crosses(aggressor_side, limit, px)) break;

    auto& level = lvl_it->second;

    // Under Split, a fixed percentage of the fill is placed FIFO before the
    // proportional pass runs on what is left. CME's step order is Split, then
    // FIFO, then Pro-Rata, then a final FIFO for the rounding remainder.
    if (alloc == Allocation::Split && remaining > 0 && !level.empty()) {
      Qty fifo_part = (remaining * fifo_pct) / 100;
      std::size_t k = 0;
      while (k < level.size() && fifo_part > 0 && remaining > 0) {
        RestingOrder& ro = level[k];
        const bool self_match = stp != SelfTradePolicy::None &&
                                aggr_owner != kAnonymous && ro.owner == aggr_owner;
        if (self_match) { ++k; continue; }
        const Qty q = std::min(fifo_part, ro.open);
        out.push_back(make_trade(aggr_id, ro.id, aggressor_side, px, q, next_seq++));
        ro.open -= q; remaining -= q; fifo_part -= q; res.filled += q;
        if (ro.open == 0) {
          live.erase(ro.id);
          level.erase(level.begin() + static_cast<std::ptrdiff_t>(k));
        } else { ++k; }
      }
    }

    // Pro-rata runs a proportional pass, then falls through to the FIFO loop
    // below to place the rounding remainder. Under plain FIFO this is skipped
    // entirely and behaviour is unchanged.
    // CME's FIFO Exception, verbatim: "In the scenario where an aggressing
    // order quantity is greater than or equal to the displayed quantity in an
    // instrument at a given price level, for matching efficiency, CME Globex
    // applies FIFO in lieu of the designated product algorithm."
    //
    // The outcome is identical either way when the aggressor takes the whole
    // level, since every resting order fills completely. What changes is the
    // work done and the order the fills are emitted in. Skipping the
    // proportional pass here is both what CME does and strictly cheaper.
    Qty level_total = 0;
    for (const auto& ro : level) level_total += ro.open;
    const bool sweeps_level = remaining >= level_total;

    if ((alloc == Allocation::ProRata || alloc == Allocation::Split ||
         alloc == Allocation::TimeWeighted) &&
        remaining > 0 && !level.empty() && !sweeps_level) {
      std::vector<bool> eligible(level.size(), true);
      if (stp != SelfTradePolicy::None && aggr_owner != kAnonymous)
        for (std::size_t k = 0; k < level.size(); ++k)
          if (level[k].owner == aggr_owner) eligible[k] = false;

      const auto share = (alloc == Allocation::TimeWeighted)
                             ? time_weighted_shares(level, eligible, remaining,
                                                    time_weight)
                             : prorata_shares(level, eligible, remaining);
      // Emit in level order so the event stream stays deterministic.
      for (std::size_t k = 0; k < level.size() && remaining > 0; ++k) {
        const Qty q = std::min(share[k], remaining);
        if (q <= 0) continue;
        out.push_back(make_trade(aggr_id, level[k].id, aggressor_side, px, q,
                                 next_seq++));
        level[k].open -= q;
        remaining     -= q;
        res.filled    += q;
      }
      // Drop anything the proportional pass fully consumed.
      for (std::size_t k = level.size(); k-- > 0;) {
        if (level[k].open == 0) {
          live.erase(level[k].id);
          level.erase(level.begin() + static_cast<std::ptrdiff_t>(k));
        }
      }
    }

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
Qty count_available(const SideMap& contra, Side aggressor_side, Price limit,
                    SelfTradePolicy stp, ParticipantId owner) {
  // Size the aggressor owns is not fillable by the aggressor once self-trade
  // prevention is on, so counting it toward a fill-or-kill decision passes
  // orders that cannot fill.
  const bool skip_own = stp != SelfTradePolicy::None && owner != kAnonymous;
  Qty total = 0;
  for (const auto& [px, level] : contra) {
    if (!crosses(aggressor_side, limit, px)) break;
    for (const auto& ro : level)
      if (!skip_own || ro.owner != owner) total += ro.open;
  }
  return total;
}

}  // namespace

Qty ReferenceBook::available_against(Side aggressor, Price limit,
                                     ParticipantId owner) const {
  return aggressor == Side::Buy
             ? count_available(asks_, aggressor, limit, stp_, owner)
             : count_available(bids_, aggressor, limit, stp_, owner);
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

  if (o.tif == TimeInForce::FOK &&
      available_against(o.side, limit, o.owner) < o.qty)
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
                     next_seq_, out, live_, Side::Sell, alloc_, fifo_pct_,
                     time_weight_)
          : do_match(bids_, o.side, limit, o.qty, o.id, o.owner, stp_,
                     next_seq_, out, live_, Side::Buy, alloc_, fifo_pct_,
                     time_weight_);

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
