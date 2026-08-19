#pragma once

#include <cstdio>
#include <string>
#include <vector>

#include "pricetime/types.hpp"

namespace pricetime {

// The engine's entire output vocabulary.
//
// Every state transition the book makes is published as one of these, in
// order. This matters beyond tidiness: the differential test in
// tests/test_differential.cpp asserts that the optimized book and the
// reference book emit *byte-identical event streams* for the same input, so
// the event stream is the specification of correct behaviour. If a
// refactor changes what the engine does, the streams diverge and the test
// fails. That is only possible because Event is comparable and total.
struct Event {
  enum class Kind : std::uint8_t {
    Accepted,  // order passed validation; matching is about to begin
    Rejected,  // order failed validation; no state change
    Trade,     // a match occurred between aggressor and resting
    Rested,    // unfilled remainder joined the book at a price level
    Canceled,  // order left the book (explicit cancel, IOC remainder, or STP)
    Replaced,  // cancel/replace applied
  };

  Kind          kind      = Kind::Accepted;
  OrderId       order_id  = kInvalidOrderId;  // subject of the event
  OrderId       contra_id = kInvalidOrderId;  // Trade only: the resting side
  Side          side      = Side::Buy;        // subject's side
  Price         price     = 0;                // Trade: execution price
  Qty           qty       = 0;                // Trade: executed size
  Seq           seq       = 0;
  RejectReason  reason    = RejectReason::None;

  // Rested only: total quantity already queued ahead of this order at its
  // price level, at the moment it rested.
  //
  // This is published deliberately. A market maker cannot compute the
  // probability of being filled without knowing how much size sits in front
  // of them, and an engine that hides it forces every participant to either
  // guess or penny the touch with full size. Queue position is the scarce
  // resource a price-time venue allocates; refusing to disclose how much of
  // it you were given makes the allocation unauditable.
  Qty           queue_ahead = 0;

  [[nodiscard]] bool operator==(const Event&) const = default;
};

// A trade always prints at the *resting* order's price, never the
// aggressor's. The resting order was there first and set the terms; an
// aggressor willing to pay more simply gets price improvement. Printing at
// the aggressor's limit would silently overcharge takers and is the classic
// off-by-one-side bug in a first matching engine.
[[nodiscard]] inline Event make_trade(OrderId aggressor, OrderId resting,
                                      Side aggressor_side, Price resting_price,
                                      Qty qty, Seq seq) noexcept {
  Event e;
  e.kind      = Event::Kind::Trade;
  e.order_id  = aggressor;
  e.contra_id = resting;
  e.side      = aggressor_side;
  e.price     = resting_price;
  e.qty       = qty;
  e.seq       = seq;
  return e;
}

[[nodiscard]] inline const char* to_string(Event::Kind k) noexcept {
  switch (k) {
    case Event::Kind::Accepted: return "ACCEPTED";
    case Event::Kind::Rejected: return "REJECTED";
    case Event::Kind::Trade:    return "TRADE";
    case Event::Kind::Rested:   return "RESTED";
    case Event::Kind::Canceled: return "CANCELED";
    case Event::Kind::Replaced: return "REPLACED";
  }
  return "?";
}

// Stable, human-diffable rendering. Used by the differential test to print a
// readable first divergence instead of "vectors differ at index 4171".
[[nodiscard]] inline std::string to_line(const Event& e) {
  char buf[192];
  std::snprintf(buf, sizeof(buf),
                "%-8s id=%llu contra=%llu %s px=%lld qty=%lld seq=%llu rej=%s "
                "qa=%lld",
                to_string(e.kind),
                static_cast<unsigned long long>(e.order_id),
                static_cast<unsigned long long>(e.contra_id),
                to_string(e.side),
                static_cast<long long>(e.price),
                static_cast<long long>(e.qty),
                static_cast<unsigned long long>(e.seq),
                to_string(e.reason),
                static_cast<long long>(e.queue_ahead));
  return std::string(buf);
}

using EventLog = std::vector<Event>;

}  // namespace pricetime
