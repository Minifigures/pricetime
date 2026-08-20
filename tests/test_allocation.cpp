// Allocation policies.
//
// FIFO is not the only way to divide an incoming order among the resting
// orders at a price. CME exposes the algorithm per instrument in FIX tag 1142
// and runs ten of them. These tests pin down pro-rata's exact semantics, and
// the differential campaign in test_differential.cpp runs the whole fuzz again
// under pro-rata so the fast and reference engines cannot drift apart.

#include "harness.hpp"
#include "pricetime/book.hpp"
#include "pricetime/reference_book.hpp"

using namespace pricetime;

namespace {

constexpr Price kFloor = 9'000, kCeil = 11'000;

NewOrder lim(OrderId id, Side s, Price px, Qty q, ParticipantId owner = kAnonymous) {
  NewOrder o;
  o.id = id; o.side = s; o.price = px; o.qty = q;
  o.type = OrderType::Limit; o.tif = TimeInForce::Day; o.owner = owner;
  return o;
}

// Fills against each resting order id, in the order they were emitted.
std::vector<std::pair<OrderId, Qty>> fills(const EventLog& log) {
  std::vector<std::pair<OrderId, Qty>> out;
  for (const auto& e : log)
    if (e.kind == Event::Kind::Trade) out.emplace_back(e.contra_id, e.qty);
  return out;
}

}  // namespace

TEST(prorata_splits_proportionally_and_rounds_down) {
  // Three equal resting orders of 7 (total 21) against an aggressor of 10.
  // Each is owed 7*10/21 = 3.33, which floors to 3, so 9 of the 10 are placed
  // proportionally and 1 lot remains. The remainder goes FIFO, to the oldest.
  Book b(kFloor, kCeil, SelfTradePolicy::None, 1u << 12,
         Book::kDefaultHotTicks, Allocation::ProRata);
  EventLog log;
  b.submit(lim(1, Side::Sell, 10000, 7), log);
  b.submit(lim(2, Side::Sell, 10000, 7), log);
  b.submit(lim(3, Side::Sell, 10000, 7), log);
  log.clear();
  b.submit(lim(4, Side::Buy, 10000, 10), log);

  const auto f = fills(log);
  Qty to1 = 0, to2 = 0, to3 = 0;
  for (const auto& [id, q] : f) {
    if (id == 1) to1 += q; else if (id == 2) to2 += q; else if (id == 3) to3 += q;
  }
  CHECK_EQ(to1, 4);   // 3 proportional + 1 remainder, oldest first
  CHECK_EQ(to2, 3);
  CHECK_EQ(to3, 3);
  CHECK_EQ(to1 + to2 + to3, 10);
}

TEST(fifo_and_prorata_genuinely_differ_on_the_same_book) {
  auto run = [](Allocation a) {
    Book b(kFloor, kCeil, SelfTradePolicy::None, 1u << 12,
           Book::kDefaultHotTicks, a);
    EventLog log;
    b.submit(lim(1, Side::Sell, 10000, 7), log);
    b.submit(lim(2, Side::Sell, 10000, 7), log);
    b.submit(lim(3, Side::Sell, 10000, 7), log);
    log.clear();
    b.submit(lim(4, Side::Buy, 10000, 10), log);
    return fills(log);
  };
  const auto fifo = run(Allocation::Fifo);
  const auto pro  = run(Allocation::ProRata);

  // FIFO fills the oldest completely before touching the next: 7 then 3.
  CHECK_EQ(fifo.size(), 2u);
  CHECK_EQ(fifo[0].first, 1u); CHECK_EQ(fifo[0].second, 7);
  CHECK_EQ(fifo[1].first, 2u); CHECK_EQ(fifo[1].second, 3);
  // Pro-rata touches all three. If these were equal the policy would be inert.
  CHECK(pro.size() > fifo.size());
}

TEST(prorata_ignores_arrival_order_in_the_proportional_step) {
  // A large order that arrived LAST still outranks small older ones by size.
  // That asymmetry is the entire economic point of pro-rata.
  Book b(kFloor, kCeil, SelfTradePolicy::None, 1u << 12,
         Book::kDefaultHotTicks, Allocation::ProRata);
  EventLog log;
  b.submit(lim(1, Side::Sell, 10000, 10), log);   // oldest, small
  b.submit(lim(2, Side::Sell, 10000, 90), log);   // newest, large
  log.clear();
  b.submit(lim(3, Side::Buy, 10000, 50), log);

  Qty to1 = 0, to2 = 0;
  for (const auto& [id, q] : fills(log)) { if (id == 1) to1 += q; else to2 += q; }
  CHECK_EQ(to1, 5);    // 10/100 of 50
  CHECK_EQ(to2, 45);   // 90/100 of 50, despite being newer
}

TEST(prorata_never_overfills_a_resting_order) {
  Book b(kFloor, kCeil, SelfTradePolicy::None, 1u << 12,
         Book::kDefaultHotTicks, Allocation::ProRata);
  EventLog log;
  b.submit(lim(1, Side::Sell, 10000, 1), log);
  b.submit(lim(2, Side::Sell, 10000, 1), log);
  log.clear();
  b.submit(lim(3, Side::Buy, 10000, 100), log);  // far more than resting

  Qty total = 0;
  for (const auto& [id, q] : fills(log)) { (void)id; total += q; }
  CHECK_EQ(total, 2);                    // only what was actually there
  CHECK_EQ(b.qty_at(Side::Buy, 10000), 98);  // remainder rests
}

TEST(prorata_matches_the_reference_implementation) {
  // Same input, both engines, pro-rata. The full randomized version of this
  // lives in test_differential.cpp; this is the readable one.
  Book fast(kFloor, kCeil, SelfTradePolicy::None, 1u << 12,
            Book::kDefaultHotTicks, Allocation::ProRata);
  ReferenceBook ref(SelfTradePolicy::None, Allocation::ProRata);
  EventLog a, b;
  for (OrderId i = 1; i <= 6; ++i) {
    const auto o = lim(i, Side::Sell, 10000, static_cast<Qty>(i) * 3);
    fast.submit(o, a); ref.submit(o, b);
  }
  a.clear(); b.clear();
  const auto agg = lim(99, Side::Buy, 10000, 40);
  fast.submit(agg, a);
  ref.submit(agg, b);
  CHECK(a == b);
}
