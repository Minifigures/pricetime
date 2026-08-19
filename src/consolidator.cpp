#include "pricetime/consolidator.hpp"

#include <algorithm>
#include <set>
#include <stdexcept>

namespace pricetime {

VenueId Consolidator::add_venue(std::string name) {
  venues_.push_back(std::move(name));
  return static_cast<VenueId>(venues_.size() - 1);
}

const std::string& Consolidator::venue_name(VenueId v) const {
  static const std::string kUnknown = "<unknown>";
  return v < venues_.size() ? venues_[v] : kUnknown;
}

Book& Consolidator::book(VenueId v, SymbolId s, Price floor_px, Price ceil_px) {
  const Key k{v, s};
  auto it = books_.find(k);
  if (it == books_.end()) {
    it = books_.emplace(k, std::make_unique<Book>(floor_px, ceil_px)).first;
  }
  return *it->second;
}

const Book* Consolidator::find_book(VenueId v, SymbolId s) const {
  const auto it = books_.find(Key{v, s});
  return it == books_.end() ? nullptr : it->second.get();
}

Bbo Consolidator::get_exchange_bbo(VenueId v, SymbolId s) const {
  Bbo out;
  const Book* b = find_book(v, s);
  if (b == nullptr) return out;
  out.bid_px = b->best_bid();
  out.ask_px = b->best_ask();
  out.bid_sz = b->bid_size();
  out.ask_sz = b->ask_size();
  return out;
}

Nbbo Consolidator::get_nbbo(SymbolId s) const {
  Nbbo n;
  // Venue count is small (single digits in practice, 16 US equity exchanges at
  // the extreme), so a linear pass is the right structure: it is branch-light,
  // cache-friendly, and avoids keeping a second index consistent. If this ever
  // needed to scale, the answer is a per-symbol packed array of BBOs updated
  // on write, not a cleverer search here.
  for (VenueId v = 0; v < static_cast<VenueId>(venues_.size()); ++v) {
    const Book* b = find_book(v, s);
    if (b == nullptr) continue;

    const Price bid = b->best_bid();
    if (bid != kInvalidPrice) {
      // Strictly greater: on a tie the earlier venue keeps the attribution.
      // Arbitrary but deterministic, which is what matters for replay.
      if (!n.quote.has_bid() || bid > n.quote.bid_px) {
        n.quote.bid_px = bid;
        n.quote.bid_sz = b->bid_size();
        n.bid_venue    = v;
      } else if (bid == n.quote.bid_px) {
        // Size at the NBBO is the aggregate across every venue showing it.
        // This matters: a taker sweeping the touch can lift all of it.
        n.quote.bid_sz += b->bid_size();
      }
    }

    const Price ask = b->best_ask();
    if (ask != kInvalidPrice) {
      if (!n.quote.has_ask() || ask < n.quote.ask_px) {
        n.quote.ask_px = ask;
        n.quote.ask_sz = b->ask_size();
        n.ask_venue    = v;
      } else if (ask == n.quote.ask_px) {
        n.quote.ask_sz += b->ask_size();
      }
    }
  }
  return n;
}

bool Consolidator::check_trade_through(SymbolId s, VenueId printed_on,
                                       Side aggressor, Price print_px, Qty qty,
                                       TradeThrough& out) const {
  // A buy aggressor trades through if some OTHER venue was offering lower than
  // the price it paid. A sell aggressor trades through if another venue was
  // bidding higher than the price it received.
  Price best_elsewhere = kInvalidPrice;
  VenueId better       = kNoVenue;

  for (VenueId v = 0; v < static_cast<VenueId>(venues_.size()); ++v) {
    if (v == printed_on) continue;
    const Book* b = find_book(v, s);
    if (b == nullptr) continue;
    const Price px = (aggressor == Side::Buy) ? b->best_ask() : b->best_bid();
    if (px == kInvalidPrice) continue;
    const bool better_than_print =
        (aggressor == Side::Buy) ? px < print_px : px > print_px;
    if (!better_than_print) continue;
    const bool improves = (best_elsewhere == kInvalidPrice) ||
                          ((aggressor == Side::Buy) ? px < best_elsewhere
                                                    : px > best_elsewhere);
    if (improves) { best_elsewhere = px; better = v; }
  }

  if (better == kNoVenue) return false;

  out.symbol         = s;
  out.venue          = printed_on;
  out.aggressor      = aggressor;
  out.print_px       = print_px;
  out.best_elsewhere = best_elsewhere;
  out.better_venue   = better;
  out.qty            = qty;
  out.harm = (aggressor == Side::Buy) ? print_px - best_elsewhere
                                      : best_elsewhere - print_px;
  return true;
}

std::vector<SymbolId> Consolidator::symbols() const {
  std::set<SymbolId> uniq;
  for (const auto& [k, b] : books_) uniq.insert(k.s);
  return std::vector<SymbolId>(uniq.begin(), uniq.end());
}

}  // namespace pricetime
