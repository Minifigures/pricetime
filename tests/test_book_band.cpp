// The price band is the one place Book deliberately departs from
// ReferenceBook: Book declares a bounded ladder and rejects prices outside it,
// and the reference has no such concept. The differential fuzz therefore
// generates only in-band prices, and a comment in test_differential.cpp has
// long said out-of-band behaviour is "covered separately in test_book_band.cpp".
//
// That file did not exist. This is it.
//
// Its absence is what let a real bug ship: an out-of-band replace emitted an
// affirmative Replaced, then unlinked and freed the order with no Canceled
// event, so a consumer reading the event stream believed the order was still
// live and the two engines burned a different number of sequence numbers from
// that point on. That case is pinned below.

#include <cstdint>

#include "harness.hpp"
#include "pricetime/book.hpp"

using namespace pricetime;

namespace {

constexpr Price kFloor = 9'000;
constexpr Price kCeil  = 11'000;

NewOrder limit(OrderId id, Side s, Price px, Qty q) {
  NewOrder o;
  o.id = id; o.side = s; o.type = OrderType::Limit;
  o.tif = TimeInForce::Day; o.price = px; o.qty = q;
  return o;
}

bool has_reject(const EventLog& log, RejectReason why) {
  for (const Event& e : log)
    if (e.kind == Event::Kind::Rejected && e.reason == why) return true;
  return false;
}

bool has_kind(const EventLog& log, Event::Kind k) {
  for (const Event& e : log)
    if (e.kind == k) return true;
  return false;
}

}  // namespace

TEST(band_rejects_a_submit_below_the_floor) {
  Book b(kFloor, kCeil);
  EventLog log;
  b.submit(limit(1, Side::Buy, kFloor - 1, 10), log);
  CHECK(has_reject(log, RejectReason::InvalidPrice));
  CHECK_EQ(b.resting_count(), 0u);
}

TEST(band_rejects_a_submit_above_the_ceiling) {
  Book b(kFloor, kCeil);
  EventLog log;
  b.submit(limit(1, Side::Sell, kCeil + 1, 10), log);
  CHECK(has_reject(log, RejectReason::InvalidPrice));
  CHECK_EQ(b.resting_count(), 0u);
}

// The band is inclusive at both ends. An off-by-one here would silently
// shrink every book by two ticks.
TEST(band_accepts_both_endpoints) {
  Book b(kFloor, kCeil);
  EventLog log;
  b.submit(limit(1, Side::Buy,  kFloor, 10), log);
  b.submit(limit(2, Side::Sell, kCeil,  10), log);
  CHECK(!has_reject(log, RejectReason::InvalidPrice));
  CHECK_EQ(b.resting_count(), 2u);
  CHECK_EQ(b.best_bid(), kFloor);
  CHECK_EQ(b.best_ask(), kCeil);
}

// The regression. A replace to an out-of-band price must be refused before
// anything is emitted or unlinked, and the resting order must survive intact.
TEST(band_rejects_an_out_of_band_replace_and_keeps_the_order) {
  Book b(kFloor, kCeil);
  EventLog rest;
  b.submit(limit(1, Side::Buy, 10'000, 10), rest);
  CHECK_EQ(b.resting_count(), 1u);

  EventLog log;
  b.replace(ReplaceOrder{1, kCeil + 1'345, 10, 0}, log);

  CHECK(has_reject(log, RejectReason::InvalidPrice));
  // No affirmative acknowledgement for something that did not happen.
  CHECK(!has_kind(log, Event::Kind::Replaced));
  // And the order is still there, at its original price.
  CHECK_EQ(b.resting_count(), 1u);
  CHECK_EQ(b.best_bid(), static_cast<Price>(10'000));
}

// Following on: the order the rejected replace left alone must still be
// cancellable. Before the fix this returned UnknownOrderId, because the
// replace had already freed it.
TEST(band_order_survives_a_rejected_replace_and_can_still_be_cancelled) {
  Book b(kFloor, kCeil);
  EventLog log;
  b.submit(limit(1, Side::Buy, 10'000, 10), log);
  b.replace(ReplaceOrder{1, kFloor - 500, 10, 0}, log);

  EventLog cx;
  b.cancel(CancelOrder{1, 0}, cx);
  CHECK(has_kind(cx, Event::Kind::Canceled));
  CHECK(!has_reject(cx, RejectReason::UnknownOrderId));
  CHECK_EQ(b.resting_count(), 0u);
}

// An in-band replace still works normally; the guard must not have caught it.
TEST(band_permits_an_in_band_replace) {
  Book b(kFloor, kCeil);
  EventLog log;
  b.submit(limit(1, Side::Buy, 10'000, 10), log);
  EventLog rep;
  b.replace(ReplaceOrder{1, 10'050, 10, 0}, rep);
  CHECK(has_kind(rep, Event::Kind::Replaced));
  CHECK(!has_reject(rep, RejectReason::InvalidPrice));
  CHECK_EQ(b.best_bid(), static_cast<Price>(10'050));
}

// span_ is a 32-bit ladder index while the band endpoints are 64-bit. A band
// wider than the index type used to truncate the ladder while is_hot() kept
// claiming those prices were hot, so the first order indexed past the end of
// the vector. This is a heap buffer overflow under ASan without the clamp.
TEST(band_wider_than_the_ladder_index_does_not_overflow) {
  const Price huge = static_cast<Price>(4'294'967'305LL);  // 2^32 + 9
  Book b(0, huge, SelfTradePolicy::None, 64, huge + 1);

  EventLog log;
  b.submit(limit(1, Side::Buy, 1'000, 5), log);
  CHECK_EQ(b.resting_count(), 1u);

  // And a price far outside the hot ladder still works, via the cold tier.
  EventLog far;
  b.submit(limit(2, Side::Sell, static_cast<Price>(4'294'967'000LL), 5), far);
  CHECK_EQ(b.resting_count(), 2u);
  CHECK_EQ(b.best_bid(), static_cast<Price>(1'000));
}

// Degenerate bands must construct and behave rather than divide by zero or
// index an empty ladder.
TEST(band_of_a_single_tick_works) {
  Book b(10'000, 10'000);
  EventLog log;
  b.submit(limit(1, Side::Buy, 10'000, 5), log);
  CHECK_EQ(b.resting_count(), 1u);
  b.submit(limit(2, Side::Buy, 10'001, 5), log);
  CHECK(has_reject(log, RejectReason::InvalidPrice));
  CHECK_EQ(b.resting_count(), 1u);
}

TEST(band_with_zero_and_negative_hot_ticks_still_matches) {
  for (Price hot : {Price{0}, Price{-5}, Price{1}}) {
    Book b(kFloor, kCeil, SelfTradePolicy::None, 64, hot);
    EventLog log;
    b.submit(limit(1, Side::Sell, 10'000, 10), log);
    b.submit(limit(2, Side::Buy,  10'000, 10), log);
    CHECK(has_kind(log, Event::Kind::Trade));
    CHECK_EQ(b.resting_count(), 0u);
  }
}
