#pragma once

#include "pricetime/types.hpp"

namespace pricetime {

// An inbound instruction. This is the engine's entire input vocabulary:
// everything a participant can do is one of these three.
struct NewOrder {
  OrderId       id       = kInvalidOrderId;
  ParticipantId owner    = kAnonymous;
  Side          side     = Side::Buy;
  OrderType     type     = OrderType::Limit;
  TimeInForce   tif      = TimeInForce::Day;
  Price         price    = 0;   // ignored when type == Market
  Qty           qty      = 0;
  Nanos         ts       = 0;   // observability only, never priority

  [[nodiscard]] bool operator==(const NewOrder&) const = default;
};

struct CancelOrder {
  OrderId id = kInvalidOrderId;
  Nanos   ts = 0;

  [[nodiscard]] bool operator==(const CancelOrder&) const = default;
};

// Cancel/replace. Any change to price, or any *increase* in quantity, loses
// time priority and goes to the back of the new price level. A pure quantity
// decrease keeps priority. That asymmetry is not an implementation detail, it
// is the rule every major venue uses, and it exists so a participant cannot
// hold a queue position while quietly growing the size behind it.
struct ReplaceOrder {
  OrderId id       = kInvalidOrderId;
  Price   price    = 0;
  Qty     qty      = 0;
  Nanos   ts       = 0;

  [[nodiscard]] bool operator==(const ReplaceOrder&) const = default;
};

// A resting order as the book holds it.
struct RestingOrder {
  OrderId       id     = kInvalidOrderId;
  ParticipantId owner  = kAnonymous;
  Side          side   = Side::Buy;
  Price         price  = 0;
  Qty           open   = 0;  // unfilled remainder
  Seq           seq    = 0;  // time priority: lower is older

  [[nodiscard]] bool operator==(const RestingOrder&) const = default;
};

}  // namespace pricetime
