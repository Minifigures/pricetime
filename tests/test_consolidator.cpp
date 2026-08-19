// Cross-venue consolidation tests.
//
// The optimized get_nbbo() reads each venue's maintained touch. The reference
// below rebuilds the same answer from ReferenceBook depth snapshots, walking
// every level of every venue with no maintained state at all. They must agree.

#include <map>
#include <vector>

#include "harness.hpp"
#include "pricetime/consolidator.hpp"
#include "pricetime/reference_book.hpp"

using namespace pricetime;

namespace {

constexpr Price kFloor = 9'000, kCeil = 11'000;

NewOrder lim(OrderId id, Side s, Price px, Qty q) {
  NewOrder o;
  o.id = id; o.side = s; o.price = px; o.qty = q;
  o.type = OrderType::Limit; o.tif = TimeInForce::Day;
  return o;
}

// Independent NBBO: no maintained touch, no bitmap, just walk every level of
// every venue's reference book and take the extremes.
Nbbo naive_nbbo(const std::vector<ReferenceBook*>& venues) {
  Nbbo n;
  for (std::size_t v = 0; v < venues.size(); ++v) {
    const auto bids = venues[v]->depth(Side::Buy, 4096);
    for (const auto& [px, sz] : bids) {
      if (sz <= 0) continue;
      if (!n.quote.has_bid() || px > n.quote.bid_px) {
        n.quote.bid_px = px; n.quote.bid_sz = sz;
        n.bid_venue = static_cast<VenueId>(v);
      } else if (px == n.quote.bid_px) {
        n.quote.bid_sz += sz;
      }
      break;  // depth() is best-first; only the touch matters
    }
    const auto asks = venues[v]->depth(Side::Sell, 4096);
    for (const auto& [px, sz] : asks) {
      if (sz <= 0) continue;
      if (!n.quote.has_ask() || px < n.quote.ask_px) {
        n.quote.ask_px = px; n.quote.ask_sz = sz;
        n.ask_venue = static_cast<VenueId>(v);
      } else if (px == n.quote.ask_px) {
        n.quote.ask_sz += sz;
      }
      break;
    }
  }
  return n;
}

class Rng {
 public:
  explicit Rng(std::uint64_t s) : s_(s) {}
  std::uint64_t next() {
    s_ ^= s_ >> 12; s_ ^= s_ << 25; s_ ^= s_ >> 27;
    return s_ * 0x2545F4914F6CDD1Dull;
  }
  std::int64_t in(std::int64_t lo, std::int64_t hi) {
    return lo + static_cast<std::int64_t>(next() %
           static_cast<std::uint64_t>(hi - lo + 1));
  }

 private:
  std::uint64_t s_;
};

}  // namespace

TEST(nbbo_of_one_venue_is_that_venues_bbo) {
  Consolidator c;
  const auto v = c.add_venue("IEX");
  EventLog log;
  auto& b = c.book(v, 1, kFloor, kCeil);
  b.submit(lim(1, Side::Buy, 9990, 100), log);
  b.submit(lim(2, Side::Sell, 10010, 200), log);

  const auto n = c.get_nbbo(1);
  CHECK_EQ(n.quote.bid_px, 9990);
  CHECK_EQ(n.quote.ask_px, 10010);
  CHECK_EQ(n.quote.bid_sz, 100);
  CHECK_EQ(n.quote.ask_sz, 200);
  CHECK_EQ(n.bid_venue, v);
  CHECK_EQ(n.spread(), 20);
}

TEST(nbbo_takes_the_best_side_from_each_venue) {
  Consolidator c;
  const auto a = c.add_venue("VENUE_A");
  const auto b = c.add_venue("VENUE_B");
  EventLog log;
  c.book(a, 1, kFloor, kCeil).submit(lim(1, Side::Buy, 9990, 100), log);
  c.book(a, 1, kFloor, kCeil).submit(lim(2, Side::Sell, 10050, 100), log);
  c.book(b, 1, kFloor, kCeil).submit(lim(3, Side::Buy, 9980, 100), log);
  c.book(b, 1, kFloor, kCeil).submit(lim(4, Side::Sell, 10010, 100), log);

  const auto n = c.get_nbbo(1);
  CHECK_EQ(n.quote.bid_px, 9990);   // A has the better bid
  CHECK_EQ(n.bid_venue, a);
  CHECK_EQ(n.quote.ask_px, 10010);  // B has the better offer
  CHECK_EQ(n.ask_venue, b);
}

TEST(size_at_the_nbbo_aggregates_across_tied_venues) {
  Consolidator c;
  const auto a = c.add_venue("A");
  const auto b = c.add_venue("B");
  EventLog log;
  c.book(a, 1, kFloor, kCeil).submit(lim(1, Side::Buy, 10000, 300), log);
  c.book(b, 1, kFloor, kCeil).submit(lim(2, Side::Buy, 10000, 700), log);

  const auto n = c.get_nbbo(1);
  CHECK_EQ(n.quote.bid_px, 10000);
  CHECK_EQ(n.quote.bid_sz, 1000);  // a sweep can lift both
  CHECK_EQ(n.bid_venue, a);        // first venue keeps attribution on a tie
}

TEST(locked_market_is_detected) {
  Consolidator c;
  const auto a = c.add_venue("A");
  const auto b = c.add_venue("B");
  EventLog log;
  c.book(a, 1, kFloor, kCeil).submit(lim(1, Side::Buy, 10000, 100), log);
  c.book(b, 1, kFloor, kCeil).submit(lim(2, Side::Sell, 10000, 100), log);

  const auto n = c.get_nbbo(1);
  CHECK(n.locked());
  CHECK(!n.crossed());
  CHECK_EQ(n.spread(), 0);
}

TEST(crossed_market_is_detected) {
  Consolidator c;
  const auto a = c.add_venue("A");
  const auto b = c.add_venue("B");
  EventLog log;
  c.book(a, 1, kFloor, kCeil).submit(lim(1, Side::Buy, 10010, 100), log);
  c.book(b, 1, kFloor, kCeil).submit(lim(2, Side::Sell, 9990, 100), log);

  const auto n = c.get_nbbo(1);
  CHECK(n.crossed());
  CHECK(!n.locked());
  CHECK_EQ(n.spread(), -20);  // negative: the book is inverted across venues
}

TEST(within_one_venue_a_cross_never_rests_so_cannot_lock_itself) {
  Consolidator c;
  const auto a = c.add_venue("A");
  EventLog log;
  auto& b = c.book(a, 1, kFloor, kCeil);
  b.submit(lim(1, Side::Sell, 10000, 100), log);
  b.submit(lim(2, Side::Buy, 10000, 100), log);  // trades instead of resting
  const auto n = c.get_nbbo(1);
  CHECK(!n.locked());
  CHECK(!n.crossed());
}

TEST(buy_trade_through_is_detected_and_harm_quantified) {
  Consolidator c;
  const auto a = c.add_venue("A");
  const auto b = c.add_venue("B");
  EventLog log;
  c.book(b, 1, kFloor, kCeil).submit(lim(1, Side::Sell, 10005, 500), log);

  TradeThrough tt;
  // A buyer paid 10020 on venue A while B was offering 10005.
  CHECK(c.check_trade_through(1, a, Side::Buy, 10020, 100, tt));
  CHECK_EQ(tt.best_elsewhere, 10005);
  CHECK_EQ(tt.better_venue, b);
  CHECK_EQ(tt.harm, 15);
  CHECK_EQ(tt.qty, 100);
}

TEST(sell_trade_through_is_detected) {
  Consolidator c;
  const auto a = c.add_venue("A");
  const auto b = c.add_venue("B");
  EventLog log;
  c.book(b, 1, kFloor, kCeil).submit(lim(1, Side::Buy, 10015, 500), log);

  TradeThrough tt;
  CHECK(c.check_trade_through(1, a, Side::Sell, 10000, 50, tt));
  CHECK_EQ(tt.best_elsewhere, 10015);
  CHECK_EQ(tt.harm, 15);
}

TEST(no_trade_through_when_the_print_was_the_best_available) {
  Consolidator c;
  const auto a = c.add_venue("A");
  const auto b = c.add_venue("B");
  EventLog log;
  c.book(b, 1, kFloor, kCeil).submit(lim(1, Side::Sell, 10020, 500), log);

  TradeThrough tt;
  CHECK(!c.check_trade_through(1, a, Side::Buy, 10010, 100, tt));
}

TEST(a_venues_own_quote_never_counts_as_trading_through_itself) {
  Consolidator c;
  const auto a = c.add_venue("A");
  EventLog log;
  c.book(a, 1, kFloor, kCeil).submit(lim(1, Side::Sell, 10000, 500), log);
  TradeThrough tt;
  CHECK(!c.check_trade_through(1, a, Side::Buy, 10050, 100, tt));
}

// The real test: random books across four venues, optimized NBBO against a
// naive walk of every level of every reference book.
TEST(differential_nbbo_across_four_venues) {
  constexpr int kVenues = 4;
  for (std::uint64_t seed = 1; seed <= 40; ++seed) {
    Rng rng(seed);
    Consolidator c;
    std::vector<ReferenceBook> refs(kVenues);
    std::vector<ReferenceBook*> refp;
    std::vector<VenueId> vids;
    for (int i = 0; i < kVenues; ++i) {
      vids.push_back(c.add_venue("V" + std::to_string(i)));
      refp.push_back(&refs[static_cast<std::size_t>(i)]);
    }

    OrderId next_id = 1;
    for (int step = 0; step < 300; ++step) {
      const auto vi = static_cast<std::size_t>(rng.in(0, kVenues - 1));
      EventLog a, b;
      const NewOrder o = lim(next_id++,
                             rng.in(0, 1) == 0 ? Side::Buy : Side::Sell,
                             rng.in(9950, 10050), rng.in(1, 50));
      c.book(vids[vi], 7, kFloor, kCeil).submit(o, a);
      refs[vi].submit(o, b);
      if (a != b) { CHECK(false); return; }

      const Nbbo fast = c.get_nbbo(7);
      const Nbbo slow = naive_nbbo(refp);
      if (fast != slow) {
        std::fprintf(stderr,
                     "\n      NBBO DIVERGENCE seed=%llu step=%d\n"
                     "        fast bid %lld x%lld @v%u | ask %lld x%lld @v%u\n"
                     "        slow bid %lld x%lld @v%u | ask %lld x%lld @v%u\n",
                     static_cast<unsigned long long>(seed), step,
                     static_cast<long long>(fast.quote.bid_px),
                     static_cast<long long>(fast.quote.bid_sz), fast.bid_venue,
                     static_cast<long long>(fast.quote.ask_px),
                     static_cast<long long>(fast.quote.ask_sz), fast.ask_venue,
                     static_cast<long long>(slow.quote.bid_px),
                     static_cast<long long>(slow.quote.bid_sz), slow.bid_venue,
                     static_cast<long long>(slow.quote.ask_px),
                     static_cast<long long>(slow.quote.ask_sz), slow.ask_venue);
        CHECK(false);
        return;
      }
    }
  }
  CHECK(true);
}
