#include "pricetime/surveillance.hpp"

#include <algorithm>
#include <cstdio>
#include <sstream>

namespace pricetime {

const char* to_string(Alert a) noexcept {
  switch (a) {
    case Alert::TradeThrough:  return "TRADE_THROUGH";
    case Alert::LockedMarket:  return "LOCKED_MARKET";
    case Alert::CrossedMarket: return "CROSSED_MARKET";
    case Alert::SelfMatch:     return "SELF_MATCH";
    case Alert::QuoteStuffing: return "QUOTE_STUFFING";
    case Alert::FleetingOrder: return "FLEETING_ORDER";
  }
  return "UNKNOWN";
}

const char* rule_citation(Alert a) noexcept {
  switch (a) {
    case Alert::TradeThrough:
      return "Reg NMS Rule 611 (rescission proposed 2026-06-17, Rel. 34-105655)";
    case Alert::LockedMarket:
    case Alert::CrossedMarket:
      return "Reg NMS Rule 610(e)";
    case Alert::SelfMatch:
      return "CEA 4c(a) wash trading; cf. CFTC v. Coinbase, Rel. 8369-21 ($6.5M)";
    case Alert::QuoteStuffing:
      return "SEC/FINRA disruptive quoting; CFTC 17 CFR 38.251(e)-(g)";
    case Alert::FleetingOrder:
      return "indicative only; not a violation on its own";
  }
  return "";
}

void Surveillance::observe(const Event& e, Nanos ts) {
  switch (e.kind) {
    case Event::Kind::Rested:
      ++s_.adds;
      open_.emplace_back(e.order_id, ts);
      break;
    case Event::Kind::Trade:
      ++s_.trades;
      break;
    case Event::Kind::Replaced:
      ++s_.replaces;
      break;
    case Event::Kind::Canceled: {
      ++s_.cancels;
      // Fleeting: added and pulled inside the window. Linear scan over a small
      // recent window rather than a map, because the window is short by
      // definition and this is off the hot path anyway.
      const auto it = std::find_if(open_.begin(), open_.end(),
          [&](const auto& p) { return p.first == e.order_id; });
      if (it != open_.end()) {
        if (ts > 0 && it->second > 0 && ts - it->second < kFleetingWindowNs)
          ++s_.fleeting;
        open_.erase(it);
      }
      break;
    }
    default: break;
  }
  // Bound memory: surveillance must never be the thing that runs a box out of
  // RAM. Oldest entries fall out; the consequence is under-counting fleeting
  // orders, which is the safe direction to be wrong in.
  if (open_.size() > 65536) open_.erase(open_.begin(), open_.begin() + 32768);
}

void Surveillance::observe_nbbo(const Nbbo& n) {
  if (n.crossed()) ++crossed_;
  else if (n.locked()) ++locked_;
}

void Surveillance::observe_trade_through(const TradeThrough& tt) {
  ++tt_count_;
  tt_shares_ += tt.qty;
  worst_harm_ = std::max(worst_harm_, tt.harm);
}

std::vector<Finding> Surveillance::findings() const {
  std::vector<Finding> out;
  char buf[512];

  if (tt_count_ > 0) {
    std::snprintf(buf, sizeof(buf),
                  "%llu print(s) executed at a worse price than another venue "
                  "was displaying, covering %lld shares; worst single case gave "
                  "up %lld ticks per share",
                  static_cast<unsigned long long>(tt_count_),
                  static_cast<long long>(tt_shares_),
                  static_cast<long long>(worst_harm_));
    out.push_back({Alert::TradeThrough, symbol_, buf,
                   std::min(1.0, static_cast<double>(tt_count_) / 50.0), tt_count_});
  }
  if (crossed_ > 0) {
    std::snprintf(buf, sizeof(buf),
                  "%llu interval(s) where the consolidated best bid exceeded the "
                  "best offer across venues (an inverted market)",
                  static_cast<unsigned long long>(crossed_));
    out.push_back({Alert::CrossedMarket, symbol_, buf, 0.9, crossed_});
  }
  if (locked_ > 0) {
    std::snprintf(buf, sizeof(buf),
                  "%llu interval(s) where the consolidated bid equalled the "
                  "offer; both sides displayed, neither able to trade",
                  static_cast<unsigned long long>(locked_));
    out.push_back({Alert::LockedMarket, symbol_, buf, 0.4, locked_});
  }
  if (s_.self_match_blocks > 0) {
    std::snprintf(buf, sizeof(buf),
                  "%llu order(s) prevented from matching against the same "
                  "participant's own resting interest",
                  static_cast<unsigned long long>(s_.self_match_blocks));
    out.push_back({Alert::SelfMatch, symbol_, buf, 0.8, s_.self_match_blocks});
  }
  const double ctt = s_.cancel_to_trade();
  if (ctt > kStuffingThreshold && s_.cancels > 1000) {
    std::snprintf(buf, sizeof(buf),
                  "cancel-to-trade ratio %.1f:1 over %llu cancels against %llu "
                  "trades, above the %.0f:1 review threshold",
                  ctt, static_cast<unsigned long long>(s_.cancels),
                  static_cast<unsigned long long>(s_.trades), kStuffingThreshold);
    out.push_back({Alert::QuoteStuffing, symbol_, buf,
                   std::min(1.0, ctt / (kStuffingThreshold * 5.0)), s_.cancels});
  }
  if (s_.fleeting > 0 && s_.adds > 0) {
    const double pct = 100.0 * static_cast<double>(s_.fleeting) /
                       static_cast<double>(s_.adds);
    std::snprintf(buf, sizeof(buf),
                  "%llu order(s), %.1f%% of all resting orders, were cancelled "
                  "within %lld microseconds of arriving",
                  static_cast<unsigned long long>(s_.fleeting), pct,
                  static_cast<long long>(kFleetingWindowNs / 1000));
    out.push_back({Alert::FleetingOrder, symbol_, buf, std::min(1.0, pct / 50.0),
                   s_.fleeting});
  }

  std::sort(out.begin(), out.end(),
            [](const Finding& a, const Finding& b) { return a.severity > b.severity; });
  return out;
}

std::string Surveillance::report() const {
  std::ostringstream o;
  const auto f = findings();
  o << "SURVEILLANCE REPORT  symbol=" << symbol_ << "\n";
  o << "  observed : " << s_.adds << " rested, " << s_.cancels << " cancelled, "
    << s_.trades << " traded, " << s_.replaces << " replaced\n";
  char r[64];
  std::snprintf(r, sizeof(r), "%.1f:1", s_.cancel_to_trade());
  o << "  cancel-to-trade : " << r << "\n\n";
  if (f.empty()) {
    o << "  No findings. Order flow is consistent with normal two-sided "
         "quoting.\n";
    return o.str();
  }
  o << "  " << f.size() << " finding(s), most severe first:\n\n";
  for (const auto& x : f) {
    char sev[16];
    std::snprintf(sev, sizeof(sev), "%.2f", x.severity);
    o << "  [" << to_string(x.kind) << "] severity " << sev << "\n"
      << "    " << x.detail << "\n"
      << "    basis: " << rule_citation(x.kind) << "\n\n";
  }
  o << "  These are screening signals, not conclusions. Each names the evidence\n"
       "  that produced it so a reviewer can check the engine rather than trust\n"
       "  it. No finding here asserts intent.\n";
  return o.str();
}

std::string Surveillance::findings_json() const {
  std::ostringstream o;
  o << "{\"symbol\":\"" << symbol_ << "\",\"stats\":{"
    << "\"rested\":" << s_.adds << ",\"cancelled\":" << s_.cancels
    << ",\"traded\":" << s_.trades << ",\"replaced\":" << s_.replaces
    << ",\"cancel_to_trade\":" << s_.cancel_to_trade() << "},\"findings\":[";
  const auto f = findings();
  for (std::size_t i = 0; i < f.size(); ++i) {
    if (i) o << ",";
    o << "{\"alert\":\"" << to_string(f[i].kind) << "\",\"severity\":"
      << f[i].severity << ",\"count\":" << f[i].count
      << ",\"detail\":\"" << f[i].detail << "\",\"basis\":\""
      << rule_citation(f[i].kind) << "\"}";
  }
  o << "]}";
  return o.str();
}

}  // namespace pricetime
