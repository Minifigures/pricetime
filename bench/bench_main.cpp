// Latency and throughput harness.
//
// Methodology, stated up front because a benchmark you cannot audit is a
// marketing number:
//
//   * Every operation is timed individually with a steady_clock read on each
//     side. That read is not free (~20ns here), so it is measured on an empty
//     loop first and reported as `timer overhead` -- the numbers below are NOT
//     overhead-adjusted, so the true engine cost is lower than what prints.
//   * Order flow is generated ahead of time into a vector, so RNG cost is not
//     inside the measured region.
//   * A warmup pass runs first so the node pool, the ladder pages, and the
//     branch predictors are all in steady state. A cold first-touch page fault
//     is a real cost but it is not the cost anyone is asking about here.
//   * Percentiles come from every sample, not from a sampled subset. p99.9 is
//     the number that matters in this domain; a good mean with a bad tail is
//     how you lose money on the one message that mattered.
//   * The reference engine runs the identical flow so the speedup is a
//     like-for-like comparison, not a comparison against a strawman.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>

#include "pricetime/book.hpp"
#include "pricetime/reference_book.hpp"

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
    return lo + static_cast<std::int64_t>(next() % static_cast<std::uint64_t>(hi - lo + 1));
  }
 private:
  std::uint64_t s_;
};

constexpr Price kFloor = 9'000, kCeil = 11'000, kMid = 10'000;

struct Op {
  enum class K : std::uint8_t { Submit, Cancel } k = K::Submit;
  NewOrder     n;
  CancelOrder  c;
};

// Order flow shaped like a real book rather than uniform noise: prices cluster
// tightly around the touch (most quoting happens within a few ticks), and
// cancels dominate submits, which is what actual venue traffic looks like --
// the overwhelming majority of messages an exchange receives are cancels.
std::vector<Op> generate(std::size_t n, std::uint64_t seed) {
  Rng rng(seed);
  std::vector<Op> ops;
  ops.reserve(n);
  std::vector<OrderId> live;
  OrderId next_id = 1;

  for (std::size_t i = 0; i < n; ++i) {
    Op op;
    if (!live.empty() && rng.in(1, 100) <= 45) {
      op.k = Op::K::Cancel;
      const auto k = static_cast<std::size_t>(rng.in(0, static_cast<std::int64_t>(live.size()) - 1));
      op.c.id = live[k];
      live[k] = live.back();
      live.pop_back();
    } else {
      op.k = Op::K::Submit;
      op.n.id    = next_id++;
      op.n.owner = static_cast<ParticipantId>(rng.in(1, 8));
      op.n.side  = rng.in(0, 1) == 0 ? Side::Buy : Side::Sell;
      op.n.type  = OrderType::Limit;
      op.n.tif   = TimeInForce::Day;
      const std::int64_t offset = rng.in(-12, 12);
      op.n.price = std::clamp<Price>(kMid + offset, kFloor, kCeil);
      op.n.qty   = rng.in(1, 100);
      live.push_back(op.n.id);
    }
    ops.push_back(op);
  }
  return ops;
}

struct Stats {
  double p50 = 0, p90 = 0, p99 = 0, p999 = 0, max = 0, mean = 0;
  double ops_per_sec = 0;
};

Stats summarize(std::vector<double>& ns, double total_seconds, std::size_t n) {
  std::sort(ns.begin(), ns.end());
  auto pick = [&](double q) {
    if (ns.empty()) return 0.0;
    auto i = static_cast<std::size_t>(q * static_cast<double>(ns.size() - 1));
    return ns[i];
  };
  Stats s;
  s.p50 = pick(0.50); s.p90 = pick(0.90);
  s.p99 = pick(0.99); s.p999 = pick(0.999);
  s.max = ns.empty() ? 0.0 : ns.back();
  double sum = 0; for (double v : ns) sum += v;
  s.mean = ns.empty() ? 0.0 : sum / static_cast<double>(ns.size());
  s.ops_per_sec = total_seconds > 0 ? static_cast<double>(n) / total_seconds : 0;
  return s;
}

void print_row(const char* label, const Stats& s) {
  std::printf("  %-22s %8.0f %8.0f %8.0f %8.0f %9.0f %12.0f\n", label, s.p50,
              s.p90, s.p99, s.p999, s.mean, s.ops_per_sec);
}

double measure_timer_overhead() {
  constexpr int kIters = 200000;
  std::vector<double> ns;
  ns.reserve(kIters);
  for (int i = 0; i < kIters; ++i) {
    const auto a = Clock::now();
    const auto b = Clock::now();
    ns.push_back(static_cast<double>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count()));
  }
  std::sort(ns.begin(), ns.end());
  return ns[ns.size() / 2];
}

template <class Engine>
Stats run(Engine& eng, const std::vector<Op>& ops, bool collect) {
  std::vector<double> ns;
  if (collect) ns.reserve(ops.size());
  EventLog log;
  log.reserve(64);

  const auto wall_start = Clock::now();
  for (const Op& op : ops) {
    log.clear();
    const auto t0 = Clock::now();
    if (op.k == Op::K::Submit) eng.submit(op.n, log);
    else                       eng.cancel(op.c, log);
    const auto t1 = Clock::now();
    if (collect)
      ns.push_back(static_cast<double>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count()));
  }
  const auto wall_end = Clock::now();
  const double secs =
      std::chrono::duration<double>(wall_end - wall_start).count();
  return summarize(ns, secs, ops.size());
}

}  // namespace

int main() {
  constexpr std::size_t kWarmup = 200'000;
  constexpr std::size_t kOps    = 2'000'000;

  std::printf("pricetime benchmark\n");
  std::printf("  operations   : %zu (after %zu warmup)\n", kOps, kWarmup);
  std::printf("  price band   : [%lld, %lld] ticks\n",
              static_cast<long long>(kFloor), static_cast<long long>(kCeil));
  std::printf("  flow         : 55%% submit / 45%% cancel, prices within "
              "12 ticks of mid\n");
  std::printf("  timer overhead (median of paired clock reads): %.0f ns\n",
              measure_timer_overhead());
  std::printf("  NOTE: latencies below INCLUDE that overhead; they are not "
              "adjusted.\n\n");

  const auto warm = generate(kWarmup, 0xC0FFEE);
  const auto flow = generate(kOps, 0xBEEF);

  std::printf("  %-22s %8s %8s %8s %8s %9s %12s\n", "engine (ns/op)", "p50",
              "p90", "p99", "p99.9", "mean", "ops/sec");
  std::printf("  %s\n", std::string(82, '-').c_str());

  {
    Book b(kFloor, kCeil);
    run(b, warm, false);
    Book fresh(kFloor, kCeil);
    run(fresh, warm, false);
    const Stats s = run(fresh, flow, true);
    print_row("Book (optimized)", s);
  }
  {
    ReferenceBook r;
    run(r, warm, false);
    ReferenceBook fresh;
    run(fresh, warm, false);
    const Stats s = run(fresh, flow, true);
    print_row("ReferenceBook", s);
  }
  std::printf("\n");
  return 0;
}
