#pragma once

// Market surveillance over the engine's own event stream.
//
// Deliberately OFF the hot path. The matching engine emits events; this reads
// them afterwards. Nothing here can slow down a match, and nothing here can
// change a matching decision. That separation is the whole design: surveillance
// that can influence execution is not surveillance, it is a trading system with
// a compliance-shaped hole in it.
//
// What it looks for, and why each one is a real regulatory concern:
//
//   TRADE_THROUGH     A print worse than a quote displayed on another venue.
//                     Reg NMS Rule 611. (Note: the SEC proposed rescinding
//                     this rule on 2026-06-17; comments closed 2026-08-17.)
//   LOCKED_MARKET     Best bid equals best ask across venues. Rule 610(e).
//   CROSSED_MARKET    Best bid strictly above best ask across venues.
//   SELF_MATCH        A participant's order met their own resting order.
//                     The CFTC fined Coinbase $6.5M in March 2021 for exactly
//                     this: two of its own automated programs crossing each
//                     other, inflating apparent volume into three published
//                     indices. Not fraud, and expensive anyway.
//   QUOTE_STUFFING    Sustained cancel-to-trade ratio far above the venue norm.
//   FLEETING_ORDER    An order added and cancelled within a very short window,
//                     the classic shape of a quote that exists to be seen
//                     rather than to be filled.
//
// Every finding carries the evidence that produced it, so a human can check the
// engine's conclusion rather than trust it.

#include <cstdint>
#include <string>
#include <vector>

#include "pricetime/consolidator.hpp"
#include "pricetime/events.hpp"

namespace pricetime {

enum class Alert : std::uint8_t {
  TradeThrough,
  LockedMarket,
  CrossedMarket,
  SelfMatch,
  QuoteStuffing,
  FleetingOrder,
};

[[nodiscard]] const char* to_string(Alert a) noexcept;
[[nodiscard]] const char* rule_citation(Alert a) noexcept;

struct Finding {
  Alert         kind = Alert::TradeThrough;
  std::string   symbol;
  std::string   detail;      // human-readable, evidence included
  double        severity = 0.0;   // 0..1, for ranking only
  std::uint64_t count = 1;
};

// Rolling counters the detectors need.
struct SurveillanceStats {
  std::uint64_t adds = 0, cancels = 0, trades = 0, replaces = 0;
  std::uint64_t fleeting = 0;      // cancelled within the fleeting window
  std::uint64_t self_match_blocks = 0;
  [[nodiscard]] double cancel_to_trade() const noexcept {
    return trades == 0 ? 0.0 : static_cast<double>(cancels) /
                               static_cast<double>(trades);
  }
};

class Surveillance {
 public:
  explicit Surveillance(std::string symbol) : symbol_(std::move(symbol)) {}

  // Feed it the engine's output. Order matters; call in emission order.
  void observe(const Event& e, Nanos ts);
  void observe_nbbo(const Nbbo& n);
  void observe_trade_through(const TradeThrough& tt);

  [[nodiscard]] std::vector<Finding> findings() const;
  [[nodiscard]] const SurveillanceStats& stats() const noexcept { return s_; }

  // A plain-language report, built from templates. Always available, no
  // network, no API key. This is the floor: the LLM narrative in
  // scripts/explain.sh enriches this, it never replaces it.
  [[nodiscard]] std::string report() const;
  // The same findings as JSON, for the optional LLM stage to consume.
  [[nodiscard]] std::string findings_json() const;

  // An order added and cancelled inside this window counts as fleeting.
  static constexpr Nanos kFleetingWindowNs = 1'000'000;  // 1 ms
  // Cancel-to-trade above this is flagged. Real equity venues run 20-50x;
  // pro-rata futures venues exceed 96% cancellation (Field & Large 2008), so
  // this threshold is venue-specific by nature and is stated, not hidden.
  static constexpr double kStuffingThreshold = 100.0;

 private:
  std::string symbol_;
  SurveillanceStats s_;
  std::vector<Finding> found_;
  std::uint64_t locked_ = 0, crossed_ = 0;
  Price worst_harm_ = 0;
  std::uint64_t tt_count_ = 0;
  Qty tt_shares_ = 0;
  // order id -> arrival timestamp, for the fleeting-order detector
  std::vector<std::pair<OrderId, Nanos>> open_;
};

}  // namespace pricetime
