#pragma once

#include <cstdint>
#include <limits>

namespace pricetime {

// ---------------------------------------------------------------------------
// Prices are integer ticks. Never floating point.
//
// A tick is the venue's minimum price increment; a Price is a count of them.
// Converting to a human-readable currency amount is a presentation concern and
// happens exactly once, at the edge (see format_price).
//
// Rationale, because this is the single decision the rest of the engine rests
// on: binary floating point cannot represent most decimal prices exactly, so
// 0.1 + 0.2 != 0.3. In a matching engine that is not a rounding curiosity, it
// is a fill printed at a price that does not exist on the ladder, a position
// that does not reconcile, and a break that someone has to unwind by hand the
// next morning. Integer ticks make price comparison exact and total, which is
// what price-time priority requires. Every production venue does this.
// ---------------------------------------------------------------------------
using Price = std::int64_t;

// Quantities are integer units (shares, lots, contracts). Same argument.
using Qty = std::int64_t;

// Client-assigned identifier for an order. Unique per session.
using OrderId = std::uint64_t;

// Engine-assigned monotonic sequence number, stamped on acceptance. This is
// the *only* thing that establishes time priority: wall-clock timestamps are
// not reliable for ordering (they can tie, and they can go backwards under
// NTP correction), so the book never compares them for priority.
using Seq = std::uint64_t;

// Nanoseconds since an arbitrary epoch. Carried for observability and
// transaction-cost analysis, never for priority.
using Nanos = std::int64_t;

inline constexpr Price  kInvalidPrice   = std::numeric_limits<Price>::min();
inline constexpr OrderId kInvalidOrderId = 0;

// A market order is represented as a limit order priced through the whole
// book, which keeps one matching path instead of two. These are the bounds.
inline constexpr Price kMaxPrice = std::numeric_limits<Price>::max() / 4;
inline constexpr Price kMinPrice = -kMaxPrice;

enum class Side : std::uint8_t { Buy = 0, Sell = 1 };

[[nodiscard]] constexpr Side opposite(Side s) noexcept {
  return s == Side::Buy ? Side::Sell : Side::Buy;
}

[[nodiscard]] constexpr const char* to_string(Side s) noexcept {
  return s == Side::Buy ? "BUY" : "SELL";
}

// True when `price` is at least as aggressive as `limit` for a resting order
// on `side`. Buy orders improve upward, sell orders improve downward, and this
// is the only place that asymmetry is encoded.
[[nodiscard]] constexpr bool crosses(Side aggressor, Price aggressor_px,
                                     Price resting_px) noexcept {
  return aggressor == Side::Buy ? aggressor_px >= resting_px
                                : aggressor_px <= resting_px;
}

enum class OrderType : std::uint8_t {
  Limit,   // rest at the limit price after taking whatever it crosses
  Market,  // take only; never rests
};

enum class TimeInForce : std::uint8_t {
  Day,  // rest until cancelled
  IOC,  // immediate-or-cancel: take what is available, cancel the remainder
  FOK,  // fill-or-kill: fill the full quantity or do nothing at all
};

// Which side of a self-match gets removed. Venues differ; the engine is
// explicit rather than silently picking one.
enum class SelfTradePolicy : std::uint8_t {
  None,           // allow self-matching (the default on many venues)
  CancelResting,  // cancel the resting order, aggressor continues
  CancelAggressor // cancel the incoming order's remainder
};

// How an incoming order's quantity is divided among the resting orders at a
// price level once price priority has already selected that level.
//
// FIFO is not the only answer, and treating it as the only answer is a
// parochialism of equity markets. CME exposes the algorithm per instrument in
// FIX tag 1142 and runs ten of them; Eurex runs three; ICE runs a time-weighted
// pro-rata on its short-term-rate products. The choice is not cosmetic: it
// changes what participants do. Field and Large (CFS WP 2008/40) found pro-rata
// one-tick futures markets sit at the minimum spread with depth around 100x
// mean trade size and cancellation rates above 96 percent, because rationing by
// size makes traders submit orders far larger than they intend to fill.
//
// Implemented here:
//
//   Fifo     Price-time priority. The oldest order at the price fills first.
//            What most equity venues use.
//
//   ProRata  Each resting order receives a share proportional to its size,
//            rounded DOWN, and the rounding remainder is then distributed
//            FIFO. Timestamps are not consulted in the proportional step.
//            Pro-rata is never the last step precisely because of the
//            rounding; something has to place the leftovers.
//   Split    CME's Configurable algorithm: a fixed percentage of each fill is
//            allocated FIFO and the rest pro-rata. CME calls the parameter
//            "Price Time Percentage", an integer where 100 is pure price-time
//            and 0 is pure pro-rata, and runs it in production at 40 percent
//            FIFO on grain and oilseed contracts. It exists because neither
//            pure rule is satisfactory: pure FIFO lets one large resting order
//            block everyone behind it, and pure pro-rata rewards submitting
//            size you never intend to fill.
enum class Allocation : std::uint8_t { Fifo = 0, ProRata = 1, Split = 2 };

[[nodiscard]] constexpr const char* to_string(Allocation a) noexcept {
  switch (a) {
    case Allocation::Fifo:    return "FIFO";
    case Allocation::ProRata: return "PRO-RATA";
    case Allocation::Split:   return "SPLIT";
  }
  return "?";
}

// Percentage of each fill allocated by the FIFO step under Allocation::Split.
// 100 is equivalent to Fifo, 0 to ProRata. CME's own default on the contracts
// that use it is 40.
inline constexpr int kDefaultFifoPercent = 40;

enum class RejectReason : std::uint8_t {
  None = 0,
  DuplicateOrderId,
  UnknownOrderId,
  InvalidQuantity,
  InvalidPrice,
  FokUnfillable,
  MarketOrderMustNotRest,
  BookHalted,
};

[[nodiscard]] constexpr const char* to_string(RejectReason r) noexcept {
  switch (r) {
    case RejectReason::None:                    return "none";
    case RejectReason::DuplicateOrderId:        return "duplicate_order_id";
    case RejectReason::UnknownOrderId:          return "unknown_order_id";
    case RejectReason::InvalidQuantity:         return "invalid_quantity";
    case RejectReason::InvalidPrice:            return "invalid_price";
    case RejectReason::FokUnfillable:           return "fok_unfillable";
    case RejectReason::MarketOrderMustNotRest:  return "market_order_must_not_rest";
    case RejectReason::BookHalted:              return "book_halted";
  }
  return "unknown";
}

// Participant identifier, used only for self-trade prevention.
using ParticipantId = std::uint32_t;
inline constexpr ParticipantId kAnonymous = 0;

}  // namespace pricetime
