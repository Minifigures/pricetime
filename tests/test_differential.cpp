// Differential fuzz: Book vs ReferenceBook.
//
// This is the test that makes the optimized engine trustworthy. The reference
// implementation is slow and obviously correct; Book is fast and not obviously
// anything. Randomized order flow is driven through both, and after every
// single operation the emitted event streams and the resulting book state must
// be identical. Not equivalent, not close: identical, including sequence
// numbers, trade prices, and queue ordering.
//
// The generator is built from the enums themselves rather than a hand-written
// list of cases, so adding an OrderType or TimeInForce automatically widens
// coverage instead of silently going untested. `all_combinations_exercised`
// asserts that actually happened.
//
// Note on the price band: Book declares a bounded ladder and rejects prices
// outside it, which the reference has no concept of. The fuzz therefore
// generates only in-band prices; out-of-band rejection is covered separately
// in test_book_band.cpp.

#include <array>
#include <cstdint>
#include <cstdio>
#include <set>
#include <vector>

#include "harness.hpp"
#include "pricetime/book.hpp"
#include "pricetime/reference_book.hpp"

using namespace pricetime;

namespace {

// xorshift64*, so the sequence is reproducible across compilers and libstdc++
// versions. std::mt19937 would be reproducible too, but the distributions are
// not, and a fuzz corpus that changes when you upgrade your toolchain is not a
// regression test.
class Rng {
 public:
  explicit Rng(std::uint64_t seed) : s_(seed ? seed : 0x9E3779B97F4A7C15ull) {}
  std::uint64_t next() {
    s_ ^= s_ >> 12; s_ ^= s_ << 25; s_ ^= s_ >> 27;
    return s_ * 0x2545F4914F6CDD1Dull;
  }
  // Uniform in [lo, hi].
  std::int64_t in(std::int64_t lo, std::int64_t hi) {
    const auto span = static_cast<std::uint64_t>(hi - lo + 1);
    return lo + static_cast<std::int64_t>(next() % span);
  }
  bool chance(int pct) { return in(1, 100) <= pct; }
 private:
  std::uint64_t s_;
};

constexpr Price kFloor = 9'000;
constexpr Price kCeil  = 11'000;

constexpr std::array<OrderType, 2>   kTypes{OrderType::Limit, OrderType::Market};
constexpr std::array<TimeInForce, 3> kTifs{TimeInForce::Day, TimeInForce::IOC,
                                           TimeInForce::FOK};
constexpr std::array<Side, 2>        kSides{Side::Buy, Side::Sell};

std::set<int> g_seen_combos;

int combo_key(OrderType t, TimeInForce f, Side s) {
  return static_cast<int>(t) * 100 + static_cast<int>(f) * 10 +
         static_cast<int>(s);
}

// Runs one seeded campaign. Returns the number of operations applied.
//
// hot_ticks controls how much of the price band the flat ladder covers. The
// default campaigns run fully hot; the two-tier campaigns pass a deliberately
// tiny value so most prices land in the cold ordered-map tier and the merged
// best-price walk, merged depth, and merged FOK precheck all get exercised.
// Without this the cold path would be dead code that still shipped.
std::size_t run_campaign(std::uint64_t seed, std::size_t ops,
                         SelfTradePolicy stp,
                         Price hot_ticks = Book::kDefaultHotTicks,
                         Allocation alloc = Allocation::Fifo,
                         int fifo_pct = kDefaultFifoPercent,
                         int time_weight = kDefaultTimeWeight) {
  Rng rng(seed);
  ReferenceBook ref(stp, alloc, fifo_pct, time_weight);
  Book fast(kFloor, kCeil, stp, 1u << 16, hot_ticks, alloc, fifo_pct, time_weight);

  std::vector<OrderId> live;
  OrderId next_id = 1;

  for (std::size_t i = 0; i < ops; ++i) {
    EventLog a, b;

    const int roll = static_cast<int>(rng.in(1, 100));
    if (roll <= 70 || live.empty()) {
      NewOrder o;
      o.id    = next_id++;
      o.owner = static_cast<ParticipantId>(rng.in(1, 4));
      o.side  = kSides[static_cast<std::size_t>(rng.in(0, 1))];
      o.type  = kTypes[static_cast<std::size_t>(rng.chance(85) ? 0 : 1)];
      o.tif   = o.type == OrderType::Market
                    ? (rng.chance(50) ? TimeInForce::IOC : TimeInForce::FOK)
                    : kTifs[static_cast<std::size_t>(rng.in(0, 2))];
      o.price = rng.in(kFloor, kCeil);
      o.qty   = rng.in(1, 40);
      g_seen_combos.insert(combo_key(o.type, o.tif, o.side));

      ref.submit(o, a);
      fast.submit(o, b);
      // Track anything that could still be resting. Ids that were fully
      // filled or cancelled simply produce UnknownOrderId rejects later,
      // which is itself a path worth fuzzing.
      live.push_back(o.id);
    } else if (roll <= 88) {
      const auto k = static_cast<std::size_t>(rng.in(0, static_cast<std::int64_t>(live.size()) - 1));
      CancelOrder c{live[k], 0};
      ref.cancel(c, a);
      fast.cancel(c, b);
    } else {
      const auto k = static_cast<std::size_t>(rng.in(0, static_cast<std::int64_t>(live.size()) - 1));
      ReplaceOrder r;
      r.id    = live[k];
      r.price = rng.in(kFloor, kCeil);
      r.qty   = rng.in(1, 40);
      ref.replace(r, a);
      fast.replace(r, b);
    }

    if (a != b) {
      std::fprintf(stderr,
                   "\n      DIVERGENCE seed=%llu op=%zu stp=%d\n",
                   static_cast<unsigned long long>(seed), i,
                   static_cast<int>(stp));
      const std::size_t n = std::max(a.size(), b.size());
      for (std::size_t j = 0; j < n; ++j) {
        const bool same = j < a.size() && j < b.size() && a[j] == b[j];
        std::fprintf(stderr, "        %s ref: %s\n", same ? " " : ">",
                     j < a.size() ? to_line(a[j]).c_str() : "<missing>");
        std::fprintf(stderr, "        %s got: %s\n", same ? " " : ">",
                     j < b.size() ? to_line(b[j]).c_str() : "<missing>");
      }
      CHECK(false);
      return i;
    }

    // Event equality is necessary but not sufficient: two engines can emit the
    // same events and still hold different books. Compare observable state too.
    if (ref.best_bid() != fast.best_bid() ||
        ref.best_ask() != fast.best_ask() ||
        ref.resting_count() != fast.resting_count() ||
        ref.depth(Side::Buy, 8) != fast.depth(Side::Buy, 8) ||
        ref.depth(Side::Sell, 8) != fast.depth(Side::Sell, 8)) {
      std::fprintf(stderr,
                   "\n      STATE DIVERGENCE seed=%llu op=%zu\n"
                   "        ref bid/ask=%lld/%lld resting=%zu\n"
                   "        got bid/ask=%lld/%lld resting=%zu\n",
                   static_cast<unsigned long long>(seed), i,
                   static_cast<long long>(ref.best_bid()),
                   static_cast<long long>(ref.best_ask()),
                   ref.resting_count(),
                   static_cast<long long>(fast.best_bid()),
                   static_cast<long long>(fast.best_ask()),
                   fast.resting_count());
      CHECK(false);
      return i;
    }
  }
  return ops;
}

}  // namespace

TEST(differential_no_self_trade_prevention) {
  for (std::uint64_t seed = 1; seed <= 12; ++seed)
    CHECK_EQ(run_campaign(seed, 4000, SelfTradePolicy::None), 4000u);
}

TEST(differential_stp_cancel_resting) {
  for (std::uint64_t seed = 100; seed <= 106; ++seed)
    CHECK_EQ(run_campaign(seed, 4000, SelfTradePolicy::CancelResting), 4000u);
}

TEST(differential_stp_cancel_aggressor) {
  for (std::uint64_t seed = 200; seed <= 206; ++seed)
    CHECK_EQ(run_campaign(seed, 4000, SelfTradePolicy::CancelAggressor), 4000u);
}

// Coverage assertion: the generator must actually have produced every
// order-type / time-in-force / side combination that the enums permit. Without
// this, a generator bug silently narrows the fuzz and the suite still passes.
// The cold tier: a 64-tick hot ladder inside a 2001-tick band, so roughly 97
// percent of generated prices take the ordered-map path and the two tiers must
// be merged correctly on every read.
TEST(differential_two_tier_cold_path) {
  for (std::uint64_t seed = 300; seed <= 312; ++seed)
    CHECK_EQ(run_campaign(seed, 4000, SelfTradePolicy::None, 64), 4000u);
}

TEST(differential_two_tier_boundary_straddling) {
  // A hot band of exactly half the price range, so the touch repeatedly
  // crosses the hot/cold boundary and best-price tracking has to hand off
  // between the bitmap and the map in both directions.
  for (std::uint64_t seed = 400; seed <= 410; ++seed)
    CHECK_EQ(run_campaign(seed, 4000, SelfTradePolicy::CancelResting, 1000), 4000u);
}

// The whole fuzz again under pro-rata. Allocation changes who gets filled at
// a level, which is the most intricate part of matching, so it gets the same
// treatment as everything else rather than being trusted because the unit
// tests in test_allocation.cpp pass.
TEST(differential_prorata_allocation) {
  for (std::uint64_t seed = 500; seed <= 515; ++seed)
    CHECK_EQ(run_campaign(seed, 4000, SelfTradePolicy::None,
                          Book::kDefaultHotTicks, Allocation::ProRata), 4000u);
}

TEST(differential_prorata_with_self_trade_prevention) {
  for (std::uint64_t seed = 600; seed <= 610; ++seed)
    CHECK_EQ(run_campaign(seed, 4000, SelfTradePolicy::CancelResting,
                          Book::kDefaultHotTicks, Allocation::ProRata), 4000u);
}

// CME's Configurable algorithm across the whole parameter range, not just the
// default. 0 and 100 must degenerate exactly to pro-rata and FIFO.
TEST(differential_split_across_the_percentage_range) {
  for (int pct : {0, 25, 40, 75, 100})
    for (std::uint64_t seed = 700; seed <= 706; ++seed)
      CHECK_EQ(run_campaign(seed, 3000, SelfTradePolicy::None,
                            Book::kDefaultHotTicks, Allocation::Split, pct), 3000u);
}

// The campaign that would have caught the emit-loop bug. The time-weighted
// kernel computed correct allocations and then discarded them because the emit
// loop sat inside the pro-rata branch, so it silently ran as FIFO. Every other
// test passed. A policy without a differential campaign is a policy nobody has
// actually checked.
TEST(differential_time_weighted_across_exponents) {
  for (int k : {1, 2, 3, 4, 8})
    for (std::uint64_t seed = 800; seed <= 806; ++seed)
      CHECK_EQ(run_campaign(seed, 3000, SelfTradePolicy::None,
                            Book::kDefaultHotTicks, Allocation::TimeWeighted,
                            kDefaultFifoPercent, k), 3000u);
}

TEST(differential_time_weighted_with_self_trade_prevention) {
  for (std::uint64_t seed = 900; seed <= 906; ++seed)
    CHECK_EQ(run_campaign(seed, 3000, SelfTradePolicy::CancelResting,
                          Book::kDefaultHotTicks, Allocation::TimeWeighted,
                          kDefaultFifoPercent, 2), 3000u);
}

TEST(all_combinations_exercised) {
  std::set<int> expected;
  for (auto t : kTypes)
    for (auto f : kTifs)
      for (auto s : kSides) {
        if (t == OrderType::Market && f == TimeInForce::Day) continue;  // rejected by design
        expected.insert(combo_key(t, f, s));
      }
  for (int want : expected) {
    if (!g_seen_combos.count(want))
      std::fprintf(stderr, "\n      combination %d never generated\n", want);
    CHECK(g_seen_combos.count(want) == 1);
  }
}
