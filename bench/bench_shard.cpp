// Sharding scalability.
//
// Reported with its caveats attached, because a scaling chart without them is
// marketing. This runs under WSL2 on a consumer CPU: no core pinning, no
// isolated cores, no NUMA control, no huge pages, and the machine has 6
// performance cores plus 4 efficiency cores presenting as 12 hardware threads.
// Efficiency cores are materially slower, so scaling is expected to bend well
// before 12x and the shape past ~6 threads says more about the scheduler than
// about the engine.
//
// What the number IS good for: showing the design has no shared write path, so
// throughput moves with cores at all. A locked design would flatten or invert.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <thread>
#include <vector>

#include "pricetime/sharded.hpp"

using namespace pricetime;
using Clock = std::chrono::steady_clock;

namespace {

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

constexpr Price kFloor = 9'000, kCeil = 11'000;

std::vector<ShardedMsg> make_flow(std::size_t n, VenueId venues,
                                  SymbolId symbols, bool hot_symbol) {
  Rng rng(0xA11CE);
  std::vector<ShardedMsg> out;
  out.reserve(n);
  std::vector<OrderId> live;
  OrderId next_id = 1;
  for (std::size_t i = 0; i < n; ++i) {
    ShardedMsg m;
    m.venue = static_cast<VenueId>(rng.in(0, venues - 1));
    // "hot" models the real world: SPY and QQQ carry orders of magnitude more
    // traffic than a typical name, so uniform symbol distribution flatters any
    // sharding scheme. Here 60% of flow lands on one symbol.
    m.symbol = (hot_symbol && rng.in(1, 100) <= 60)
                   ? 0u
                   : static_cast<SymbolId>(rng.in(0, symbols - 1));
    const auto roll = rng.in(1, 100);
    if (roll <= 60 || live.empty()) {
      m.kind = ShardedMsg::Kind::New;
      m.nw.id = next_id++;
      m.nw.side = rng.in(0, 1) == 0 ? Side::Buy : Side::Sell;
      m.nw.type = OrderType::Limit;
      m.nw.tif  = TimeInForce::Day;
      m.nw.price = rng.in(9960, 10040);
      m.nw.qty   = rng.in(1, 50);
      live.push_back(m.nw.id);
    } else {
      m.kind = ShardedMsg::Kind::Cancel;
      m.cx.id = live[static_cast<std::size_t>(rng.in(0, static_cast<std::int64_t>(live.size()) - 1))];
    }
    out.push_back(m);
  }
  return out;
}

double run_once(std::uint32_t shards, const std::vector<ShardedMsg>& flow) {
  ShardedReplay r(shards, kFloor, kCeil);
  const auto t0 = Clock::now();
  r.run(flow);
  const double secs = std::chrono::duration<double>(Clock::now() - t0).count();
  return static_cast<double>(flow.size()) / secs / 1e6;
}

}  // namespace

int main() {
  constexpr std::size_t kOps = 4'000'000;
  std::printf("pricetime sharding scalability\n");
  std::printf("  hardware_concurrency : %u\n", std::thread::hardware_concurrency());
  std::printf("  messages per run     : %zu\n", kOps);
  std::printf("  CAVEATS: WSL2, no core pinning, no core isolation, no huge\n");
  std::printf("           pages, heterogeneous P/E cores. Treat the shape as\n");
  std::printf("           indicative, not as a clean scaling curve.\n\n");

  struct Case { const char* name; bool hot; };
  const Case cases[] = {
      {"uniform (256 symbols, 4 venues)", false},
      {"hot symbol (60% of flow on one)", true},
  };

  for (const Case& c : cases) {
    const auto flow = make_flow(kOps, 4, 256, c.hot);
    std::printf("  %s\n", c.name);
    std::printf("    %-8s %14s %10s\n", "shards", "M msg/sec", "speedup");
    double base = 0.0;
    for (std::uint32_t n : {1u, 2u, 4u, 6u, 8u, 12u}) {
      run_once(n, flow);                       // warm
      const double best = std::max({run_once(n, flow), run_once(n, flow),
                                    run_once(n, flow)});
      if (n == 1) base = best;
      std::printf("    %-8u %14.2f %9.2fx\n", n, best,
                  base > 0 ? best / base : 0.0);
    }
    std::printf("\n");
  }
  return 0;
}
