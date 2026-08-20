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

TEST(split_allocates_the_configured_percentage_fifo_then_the_rest_prorata) {
  // 40 percent FIFO, CME's own production setting on grain and oilseed.
  // Aggressor 100 against three resting orders of 100 each.
  //   FIFO step:     40 lots to the oldest
  //   Pro-rata step: remaining 60 split over 60/100/100 -> 60*60/260 = 13,
  //                  100*60/260 = 23, 100*60/260 = 23, total 59
  //   FIFO remainder: the last lot to the oldest still resting
  Book b(kFloor, kCeil, SelfTradePolicy::None, 1u << 12,
         Book::kDefaultHotTicks, Allocation::Split, 40);
  EventLog log;
  b.submit(lim(1, Side::Sell, 10000, 100), log);
  b.submit(lim(2, Side::Sell, 10000, 100), log);
  b.submit(lim(3, Side::Sell, 10000, 100), log);
  log.clear();
  b.submit(lim(4, Side::Buy, 10000, 100), log);

  Qty to1 = 0, to2 = 0, to3 = 0, total = 0;
  for (const auto& [id, q] : fills(log)) {
    total += q;
    if (id == 1) to1 += q; else if (id == 2) to2 += q; else to3 += q;
  }
  CHECK_EQ(total, 100);
  CHECK(to1 > to2);            // the FIFO step favours the oldest
  CHECK(to2 > 0 && to3 > 0);   // but the others still participate
  CHECK_EQ(to2, to3);          // equal size, equal proportional share
}

TEST(split_at_100_percent_is_exactly_fifo) {
  auto run = [](Allocation a, int pct) {
    Book b(kFloor, kCeil, SelfTradePolicy::None, 1u << 12,
           Book::kDefaultHotTicks, a, pct);
    EventLog log;
    b.submit(lim(1, Side::Sell, 10000, 30), log);
    b.submit(lim(2, Side::Sell, 10000, 30), log);
    log.clear();
    b.submit(lim(3, Side::Buy, 10000, 40), log);
    return fills(log);
  };
  CHECK(run(Allocation::Split, 100) == run(Allocation::Fifo, 0));
}

TEST(split_at_0_percent_is_exactly_prorata) {
  auto run = [](Allocation a, int pct) {
    Book b(kFloor, kCeil, SelfTradePolicy::None, 1u << 12,
           Book::kDefaultHotTicks, a, pct);
    EventLog log;
    b.submit(lim(1, Side::Sell, 10000, 30), log);
    b.submit(lim(2, Side::Sell, 10000, 30), log);
    log.clear();
    b.submit(lim(3, Side::Buy, 10000, 40), log);
    return fills(log);
  };
  CHECK(run(Allocation::Split, 0) == run(Allocation::ProRata, 0));
}

// The time-weighted kernel: f_j(k) = (Q_j^k - Q_{j+1}^k) / V^k over the
// cumulative volume from order j onward, in time order.
//
// One formula covers five published exchange algorithms. k=1 is pure pro-rata
// (CME 'C', Eurex Pro-Rata), k=2 is Eurex Time-Pro-Rata and ICE's Euribor and
// SARON and SONIA and SOFR, k=4 is ICE Short Sterling and Euroswiss, and the
// limit as k grows is FIFO.
TEST(time_weighted_at_k1_is_exactly_prorata) {
  // The degenerate case. At k=1 the kernel telescopes to q_j / V, which IS
  // pro-rata. If these differ, the kernel is not what it claims to be.
  auto run = [](Allocation a, int k) {
    Book b(kFloor, kCeil, SelfTradePolicy::None, 1u << 12,
           Book::kDefaultHotTicks, a, 40, k);
    EventLog log;
    b.submit(lim(1, Side::Sell, 10000, 20), log);
    b.submit(lim(2, Side::Sell, 10000, 20), log);
    b.submit(lim(3, Side::Sell, 10000, 50), log);
    log.clear();
    b.submit(lim(4, Side::Buy, 10000, 25), log);
    return fills(log);
  };
  CHECK(run(Allocation::TimeWeighted, 1) == run(Allocation::ProRata, 1));
}

TEST(time_weighted_shifts_toward_the_front_of_the_queue_as_k_rises) {
  // Same book as Eurex's own published Example 7-7 (20, 20, 50 in time order,
  // aggressor 25) so the progression is checkable against their table.
  auto oldest_gets = [](int k) {
    Book b(kFloor, kCeil, SelfTradePolicy::None, 1u << 12,
           Book::kDefaultHotTicks, Allocation::TimeWeighted, 40, k);
    EventLog log;
    b.submit(lim(1, Side::Sell, 10000, 20), log);
    b.submit(lim(2, Side::Sell, 10000, 20), log);
    b.submit(lim(3, Side::Sell, 10000, 50), log);
    log.clear();
    b.submit(lim(4, Side::Buy, 10000, 25), log);
    Qty to1 = 0;
    for (const auto& [id, q] : fills(log)) if (id == 1) to1 += q;
    return to1;
  };
  const Qty k1 = oldest_gets(1), k2 = oldest_gets(2), k4 = oldest_gets(4);
  CHECK_EQ(k1, 7);    // pro-rata: size wins
  CHECK_EQ(k2, 11);   // Eurex TPR / ICE Euribor
  CHECK_EQ(k4, 17);   // ICE Short Sterling: much closer to FIFO
  CHECK(k1 < k2 && k2 < k4);   // monotone toward time priority
}

TEST(time_weighted_at_large_k_converges_to_fifo) {
  auto run = [](Allocation a, int k) {
    Book b(kFloor, kCeil, SelfTradePolicy::None, 1u << 12,
           Book::kDefaultHotTicks, a, 40, k);
    EventLog log;
    b.submit(lim(1, Side::Sell, 10000, 20), log);
    b.submit(lim(2, Side::Sell, 10000, 20), log);
    b.submit(lim(3, Side::Sell, 10000, 50), log);
    log.clear();
    b.submit(lim(4, Side::Buy, 10000, 25), log);
    return fills(log);
  };
  CHECK(run(Allocation::TimeWeighted, 32) == run(Allocation::Fifo, 1));
}

// CME's FIFO Exception, verbatim: "In the scenario where an aggressing order
// quantity is greater than or equal to the displayed quantity in an instrument
// at a given price level, for matching efficiency, CME Globex applies FIFO in
// lieu of the designated product algorithm."
TEST(an_order_sweeping_the_whole_level_fills_everything_under_every_policy) {
  for (auto a : {Allocation::Fifo, Allocation::ProRata, Allocation::Split,
                 Allocation::TimeWeighted}) {
    Book b(kFloor, kCeil, SelfTradePolicy::None, 1u << 12,
           Book::kDefaultHotTicks, a, 40, 2);
    EventLog log;
    b.submit(lim(1, Side::Sell, 10000, 20), log);
    b.submit(lim(2, Side::Sell, 10000, 30), log);
    b.submit(lim(3, Side::Sell, 10000, 50), log);
    log.clear();
    b.submit(lim(4, Side::Buy, 10000, 100), log);   // exactly the level total
    Qty total = 0;
    for (const auto& [id, q] : fills(log)) { (void)id; total += q; }
    CHECK_EQ(total, 100);
    CHECK_EQ(b.qty_at(Side::Sell, 10000), 0);
  }
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
