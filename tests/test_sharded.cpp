// Sharding tests. The determinism test is the acceptance criterion: if the
// same input replayed on N threads does not produce byte-identical per-book
// event streams to the same input replayed on one, the sharding is wrong and
// no throughput number redeems it.

#include <atomic>
#include <thread>
#include <vector>

#include "harness.hpp"
#include "pricetime/sharded.hpp"

using namespace pricetime;

namespace {

constexpr Price kFloor = 9'000, kCeil = 11'000;

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

std::vector<ShardedMsg> make_flow(std::uint64_t seed, std::size_t n,
                                  VenueId venues, SymbolId symbols) {
  Rng rng(seed);
  std::vector<ShardedMsg> out;
  out.reserve(n);
  std::vector<OrderId> live;
  OrderId next_id = 1;

  for (std::size_t i = 0; i < n; ++i) {
    ShardedMsg m;
    m.venue  = static_cast<VenueId>(rng.in(0, venues - 1));
    m.symbol = static_cast<SymbolId>(rng.in(0, symbols - 1));
    const auto roll = rng.in(1, 100);
    if (roll <= 70 || live.empty()) {
      m.kind = ShardedMsg::Kind::New;
      m.nw.id    = next_id++;
      m.nw.owner = static_cast<ParticipantId>(rng.in(1, 4));
      m.nw.side  = rng.in(0, 1) == 0 ? Side::Buy : Side::Sell;
      m.nw.type  = OrderType::Limit;
      m.nw.tif   = rng.in(1, 100) <= 15 ? TimeInForce::IOC : TimeInForce::Day;
      m.nw.price = rng.in(9950, 10050);
      m.nw.qty   = rng.in(1, 40);
      live.push_back(m.nw.id);
    } else if (roll <= 88) {
      m.kind = ShardedMsg::Kind::Cancel;
      m.cx.id = live[static_cast<std::size_t>(rng.in(0, static_cast<std::int64_t>(live.size()) - 1))];
    } else {
      m.kind = ShardedMsg::Kind::Replace;
      m.rp.id    = live[static_cast<std::size_t>(rng.in(0, static_cast<std::int64_t>(live.size()) - 1))];
      m.rp.price = rng.in(9950, 10050);
      m.rp.qty   = rng.in(1, 40);
    }
    out.push_back(m);
  }
  return out;
}

}  // namespace

TEST(shard_routing_is_stable_and_within_range) {
  for (std::uint32_t n : {1u, 2u, 4u, 8u, 12u}) {
    for (VenueId v = 0; v < 8; ++v) {
      for (SymbolId s = 0; s < 64; ++s) {
        const auto a = shard_of(v, s, n);
        const auto b = shard_of(v, s, n);
        CHECK(a < n);
        CHECK_EQ(a, b);  // same key always lands on the same shard
      }
    }
  }
}

TEST(shard_routing_spreads_keys_across_shards) {
  constexpr std::uint32_t kShards = 8;
  std::vector<int> hits(kShards, 0);
  for (VenueId v = 0; v < 4; ++v)
    for (SymbolId s = 0; s < 256; ++s)
      ++hits[shard_of(v, s, kShards)];
  // 1024 keys over 8 shards. A hash that clustered would leave one empty.
  for (std::uint32_t i = 0; i < kShards; ++i) CHECK(hits[i] > 50);
}

TEST(seqlock_bbo_read_never_tears_under_concurrent_writes) {
  BboSlot slot;
  std::atomic<bool> stop{false};
  std::atomic<std::uint64_t> torn{0}, reads{0};

  // Seed a valid quote BEFORE either thread starts.
  //
  // Without this the reader can win the race to its first load and observe the
  // default-constructed slot, where bid_px is kInvalidPrice rather than any
  // value satisfying the invariant below. That is not a torn read, it is a
  // read of a state the writer never published, and counting it as a failure
  // makes the test schedule-dependent: g++ happened to start the writer first
  // and passed, clang started the reader first and reported 142,695 "tears".
  // The seqlock was never at fault. Publishing a known-good value first
  // removes the ambiguity instead of papering over it with a sentinel check.
  {
    Bbo seed;
    seed.bid_px = 10'000; seed.bid_sz = 20'000;
    seed.ask_px = 10'010; seed.ask_sz = 30'000;
    slot.publish(seed);
  }

  // Writer publishes only self-consistent quotes: ask is always bid + 10.
  std::thread writer([&] {
    for (Price p = 10'000; !stop.load(std::memory_order_relaxed); ++p) {
      Bbo b;
      b.bid_px = p;      b.bid_sz = p * 2;
      b.ask_px = p + 10; b.ask_sz = p * 3;
      slot.publish(b);
    }
  });

  // Any reader that observes ask != bid + 10 has seen a torn struct.
  std::thread reader([&] {
    for (int i = 0; i < 400'000; ++i) {
      const Bbo b = slot.read();
      reads.fetch_add(1, std::memory_order_relaxed);
      // Every published quote satisfies ask == bid + 10, bid_sz == bid * 2 and
      // ask_sz == bid * 3. Any read violating that saw fields from two
      // different publishes.
      if (b.ask_px != b.bid_px + 10 || b.bid_sz != b.bid_px * 2 ||
          b.ask_sz != b.bid_px * 3)
        torn.fetch_add(1, std::memory_order_relaxed);
    }
  });

  reader.join();
  stop.store(true, std::memory_order_relaxed);
  writer.join();

  CHECK(reads.load() > 0);
  CHECK_EQ(torn.load(), 0u);
}

// THE acceptance criterion.
TEST(sharded_replay_is_identical_to_serial_replay) {
  for (std::uint64_t seed = 1; seed <= 6; ++seed) {
    const auto flow = make_flow(seed, 20'000, 4, 40);

    ShardedReplay serial(1, kFloor, kCeil);
    serial.run(flow);

    for (std::uint32_t n : {2u, 4u, 8u}) {
      ShardedReplay parallel(n, kFloor, kCeil);
      parallel.run(flow);

      CHECK_EQ(parallel.applied(), serial.applied());
      CHECK_EQ(parallel.trades(),  serial.trades());

      for (VenueId v = 0; v < 4; ++v) {
        for (SymbolId s = 0; s < 40; ++s) {
          const EventLog* a = serial.log_for(v, s);
          const EventLog* b = parallel.log_for(v, s);
          if ((a == nullptr) != (b == nullptr)) { CHECK(false); return; }
          if (a == nullptr) continue;
          if (*a != *b) {
            std::fprintf(stderr,
                         "\n      SHARDING DIVERGENCE seed=%llu shards=%u "
                         "venue=%u symbol=%u\n"
                         "        serial %zu events, parallel %zu events\n",
                         static_cast<unsigned long long>(seed), n, v, s,
                         a->size(), b->size());
            for (std::size_t i = 0; i < std::min(a->size(), b->size()); ++i) {
              if ((*a)[i] == (*b)[i]) continue;
              std::fprintf(stderr, "        first diff at %zu:\n"
                                   "          serial   %s\n"
                                   "          parallel %s\n",
                           i, to_line((*a)[i]).c_str(),
                           to_line((*b)[i]).c_str());
              break;
            }
            CHECK(false);
            return;
          }
        }
      }
    }
  }
  CHECK(true);
}

TEST(every_message_lands_somewhere_regardless_of_shard_count) {
  const auto flow = make_flow(99, 5'000, 3, 20);
  for (std::uint32_t n : {1u, 2u, 3u, 5u, 8u, 16u}) {
    ShardedReplay r(n, kFloor, kCeil);
    r.run(flow);
    CHECK_EQ(r.applied(), flow.size());
  }
}
