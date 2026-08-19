// Behavioural tests for ReferenceBook -- the specification.
//
// These assert the *rules of a matching engine*, not implementation details.
// Every one of them must also hold for the optimized Book, which is what
// test_differential.cpp enforces mechanically.

#include "harness.hpp"
#include "pricetime/reference_book.hpp"

using namespace pricetime;

namespace {

NewOrder limit(OrderId id, Side s, Price px, Qty q,
               TimeInForce tif = TimeInForce::Day,
               ParticipantId owner = kAnonymous) {
  NewOrder o;
  o.id = id; o.side = s; o.price = px; o.qty = q;
  o.type = OrderType::Limit; o.tif = tif; o.owner = owner;
  return o;
}

NewOrder market(OrderId id, Side s, Qty q, TimeInForce tif = TimeInForce::IOC) {
  NewOrder o;
  o.id = id; o.side = s; o.qty = q;
  o.type = OrderType::Market; o.tif = tif;
  return o;
}

// Count events of a kind, and pull the nth trade, so tests read as assertions
// about behaviour instead of about vector indices.
std::size_t count(const EventLog& log, Event::Kind k) {
  std::size_t n = 0;
  for (const auto& e : log) if (e.kind == k) ++n;
  return n;
}
Event nth_trade(const EventLog& log, std::size_t n) {
  for (const auto& e : log)
    if (e.kind == Event::Kind::Trade && n-- == 0) return e;
  return Event{};
}

}  // namespace

TEST(empty_book_has_no_touch) {
  ReferenceBook b;
  CHECK(b.best_bid() == kInvalidPrice);
  CHECK(b.best_ask() == kInvalidPrice);
  CHECK_EQ(b.resting_count(), 0u);
}

TEST(limit_order_rests_when_it_does_not_cross) {
  ReferenceBook b; EventLog log;
  b.submit(limit(1, Side::Buy, 100, 50), log);
  CHECK_EQ(b.best_bid(), 100);
  CHECK_EQ(b.qty_at(Side::Buy, 100), 50);
  CHECK_EQ(count(log, Event::Kind::Trade), 0u);
  CHECK_EQ(count(log, Event::Kind::Accepted), 1u);
}

TEST(crossing_order_fills_completely) {
  ReferenceBook b; EventLog log;
  b.submit(limit(1, Side::Sell, 100, 50), log);
  log.clear();
  b.submit(limit(2, Side::Buy, 100, 50), log);
  CHECK_EQ(count(log, Event::Kind::Trade), 1u);
  CHECK_EQ(nth_trade(log, 0).qty, 50);
  CHECK_EQ(b.resting_count(), 0u);
}

// The classic first-engine bug: printing the trade at the aggressor's limit
// instead of the resting price silently overcharges every taker.
TEST(trade_prints_at_resting_price_not_aggressor_price) {
  ReferenceBook b; EventLog log;
  b.submit(limit(1, Side::Sell, 100, 10), log);
  log.clear();
  b.submit(limit(2, Side::Buy, 105, 10), log);  // willing to pay 105
  CHECK_EQ(nth_trade(log, 0).price, 100);        // gets price improvement
}

TEST(aggressor_larger_than_resting_leaves_remainder_on_book) {
  ReferenceBook b; EventLog log;
  b.submit(limit(1, Side::Sell, 100, 30), log);
  log.clear();
  b.submit(limit(2, Side::Buy, 100, 50), log);
  CHECK_EQ(count(log, Event::Kind::Trade), 1u);
  CHECK_EQ(nth_trade(log, 0).qty, 30);
  CHECK_EQ(b.best_bid(), 100);
  CHECK_EQ(b.qty_at(Side::Buy, 100), 20);
}

TEST(resting_larger_than_aggressor_keeps_remainder_resting) {
  ReferenceBook b; EventLog log;
  b.submit(limit(1, Side::Sell, 100, 80), log);
  log.clear();
  b.submit(limit(2, Side::Buy, 100, 30), log);
  CHECK_EQ(nth_trade(log, 0).qty, 30);
  CHECK_EQ(b.qty_at(Side::Sell, 100), 50);
}

TEST(time_priority_older_order_at_same_price_fills_first) {
  ReferenceBook b; EventLog log;
  b.submit(limit(1, Side::Sell, 100, 10), log);   // older
  b.submit(limit(2, Side::Sell, 100, 10), log);   // newer
  log.clear();
  b.submit(limit(3, Side::Buy, 100, 10), log);
  CHECK_EQ(nth_trade(log, 0).contra_id, 1u);      // the older one
}

TEST(price_priority_better_price_fills_before_worse) {
  ReferenceBook b; EventLog log;
  b.submit(limit(1, Side::Sell, 101, 10), log);
  b.submit(limit(2, Side::Sell, 100, 10), log);   // better for a buyer
  log.clear();
  b.submit(limit(3, Side::Buy, 101, 20), log);
  CHECK_EQ(nth_trade(log, 0).contra_id, 2u);
  CHECK_EQ(nth_trade(log, 0).price, 100);
  CHECK_EQ(nth_trade(log, 1).contra_id, 1u);
  CHECK_EQ(nth_trade(log, 1).price, 101);
}

TEST(market_order_sweeps_multiple_levels) {
  ReferenceBook b; EventLog log;
  b.submit(limit(1, Side::Sell, 100, 10), log);
  b.submit(limit(2, Side::Sell, 101, 10), log);
  b.submit(limit(3, Side::Sell, 102, 10), log);
  log.clear();
  b.submit(market(4, Side::Buy, 25), log);
  CHECK_EQ(count(log, Event::Kind::Trade), 3u);
  CHECK_EQ(nth_trade(log, 0).price, 100);
  CHECK_EQ(nth_trade(log, 2).price, 102);
  CHECK_EQ(nth_trade(log, 2).qty, 5);
}

TEST(market_order_with_day_tif_is_rejected) {
  ReferenceBook b; EventLog log;
  b.submit(market(1, Side::Buy, 10, TimeInForce::Day), log);
  CHECK_EQ(count(log, Event::Kind::Rejected), 1u);
  CHECK_EQ(static_cast<int>(log[0].reason),
           static_cast<int>(RejectReason::MarketOrderMustNotRest));
}

TEST(ioc_cancels_its_unfilled_remainder) {
  ReferenceBook b; EventLog log;
  b.submit(limit(1, Side::Sell, 100, 10), log);
  log.clear();
  b.submit(limit(2, Side::Buy, 100, 30, TimeInForce::IOC), log);
  CHECK_EQ(count(log, Event::Kind::Trade), 1u);
  CHECK_EQ(count(log, Event::Kind::Canceled), 1u);
  CHECK_EQ(b.resting_count(), 0u);
}

TEST(fok_is_rejected_when_book_cannot_fill_it_entirely) {
  ReferenceBook b; EventLog log;
  b.submit(limit(1, Side::Sell, 100, 10), log);
  log.clear();
  b.submit(limit(2, Side::Buy, 100, 30, TimeInForce::FOK), log);
  CHECK_EQ(count(log, Event::Kind::Trade), 0u);   // nothing at all happened
  CHECK_EQ(count(log, Event::Kind::Rejected), 1u);
  CHECK_EQ(b.qty_at(Side::Sell, 100), 10);        // book untouched
}

TEST(fok_fills_when_book_has_exactly_enough) {
  ReferenceBook b; EventLog log;
  b.submit(limit(1, Side::Sell, 100, 10), log);
  b.submit(limit(2, Side::Sell, 101, 20), log);
  log.clear();
  b.submit(limit(3, Side::Buy, 101, 30, TimeInForce::FOK), log);
  CHECK_EQ(count(log, Event::Kind::Trade), 2u);
  CHECK_EQ(b.resting_count(), 0u);
}

TEST(cancel_removes_the_order) {
  ReferenceBook b; EventLog log;
  b.submit(limit(1, Side::Buy, 100, 10), log);
  log.clear();
  b.cancel(CancelOrder{1, 0}, log);
  CHECK_EQ(count(log, Event::Kind::Canceled), 1u);
  CHECK_EQ(log[0].qty, 10);
  CHECK_EQ(b.resting_count(), 0u);
  CHECK(b.best_bid() == kInvalidPrice);
}

TEST(cancel_of_unknown_id_is_rejected) {
  ReferenceBook b; EventLog log;
  b.cancel(CancelOrder{999, 0}, log);
  CHECK_EQ(count(log, Event::Kind::Rejected), 1u);
  CHECK_EQ(static_cast<int>(log[0].reason),
           static_cast<int>(RejectReason::UnknownOrderId));
}

TEST(duplicate_order_id_is_rejected) {
  ReferenceBook b; EventLog log;
  b.submit(limit(1, Side::Buy, 100, 10), log);
  log.clear();
  b.submit(limit(1, Side::Buy, 100, 10), log);
  CHECK_EQ(count(log, Event::Kind::Rejected), 1u);
  CHECK_EQ(static_cast<int>(log[0].reason),
           static_cast<int>(RejectReason::DuplicateOrderId));
}

TEST(non_positive_quantity_is_rejected) {
  ReferenceBook b; EventLog log;
  b.submit(limit(1, Side::Buy, 100, 0), log);
  b.submit(limit(2, Side::Buy, 100, -5), log);
  CHECK_EQ(count(log, Event::Kind::Rejected), 2u);
}

TEST(replace_that_only_reduces_size_keeps_queue_position) {
  ReferenceBook b; EventLog log;
  b.submit(limit(1, Side::Buy, 100, 10), log);   // ahead in queue
  b.submit(limit(2, Side::Buy, 100, 10), log);
  log.clear();
  b.replace(ReplaceOrder{1, 100, 5, 0}, log);    // shrink, same price
  CHECK_EQ(count(log, Event::Kind::Replaced), 1u);
  log.clear();
  b.submit(limit(3, Side::Sell, 100, 5), log);
  CHECK_EQ(nth_trade(log, 0).contra_id, 1u);     // still first
}

TEST(replace_that_increases_size_loses_queue_position) {
  ReferenceBook b; EventLog log;
  b.submit(limit(1, Side::Buy, 100, 10), log);
  b.submit(limit(2, Side::Buy, 100, 10), log);
  log.clear();
  b.replace(ReplaceOrder{1, 100, 20, 0}, log);   // grow: goes to the back
  log.clear();
  b.submit(limit(3, Side::Sell, 100, 5), log);
  CHECK_EQ(nth_trade(log, 0).contra_id, 2u);     // order 2 is now first
}

TEST(replace_that_changes_price_loses_queue_position) {
  ReferenceBook b; EventLog log;
  b.submit(limit(1, Side::Buy, 100, 10), log);
  log.clear();
  b.replace(ReplaceOrder{1, 99, 10, 0}, log);
  CHECK_EQ(b.best_bid(), 99);
  CHECK_EQ(b.qty_at(Side::Buy, 100), 0);
}

TEST(stp_cancel_resting_removes_the_maker_and_aggressor_continues) {
  ReferenceBook b(SelfTradePolicy::CancelResting); EventLog log;
  b.submit(limit(1, Side::Sell, 100, 10, TimeInForce::Day, 7), log);
  b.submit(limit(2, Side::Sell, 100, 10, TimeInForce::Day, 9), log);
  log.clear();
  b.submit(limit(3, Side::Buy, 100, 20, TimeInForce::Day, 7), log);
  CHECK_EQ(count(log, Event::Kind::Canceled), 1u);  // participant 7's maker
  CHECK_EQ(count(log, Event::Kind::Trade), 1u);     // then trades with 9
  CHECK_EQ(nth_trade(log, 0).contra_id, 2u);
}

TEST(stp_cancel_aggressor_stops_the_taker) {
  ReferenceBook b(SelfTradePolicy::CancelAggressor); EventLog log;
  b.submit(limit(1, Side::Sell, 100, 10, TimeInForce::Day, 7), log);
  log.clear();
  b.submit(limit(2, Side::Buy, 100, 10, TimeInForce::Day, 7), log);
  CHECK_EQ(count(log, Event::Kind::Trade), 0u);
  CHECK_EQ(count(log, Event::Kind::Canceled), 1u);
  CHECK_EQ(b.qty_at(Side::Sell, 100), 10);          // maker survives
}

TEST(stp_none_permits_self_matching) {
  ReferenceBook b(SelfTradePolicy::None); EventLog log;
  b.submit(limit(1, Side::Sell, 100, 10, TimeInForce::Day, 7), log);
  log.clear();
  b.submit(limit(2, Side::Buy, 100, 10, TimeInForce::Day, 7), log);
  CHECK_EQ(count(log, Event::Kind::Trade), 1u);
}
