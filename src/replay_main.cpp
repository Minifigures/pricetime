// Terminal replay: watch the book breathe.
//
// Renders a live depth ladder and trade tape while synthetic order flow is
// matched in real time. This is a visualization of the same engine the
// benchmark measures, not a mock -- every level drawn came out of Book::depth
// and every print on the tape came out of a Trade event.

#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <chrono>
#include <cstdlib>
#include <deque>
#include <string>
#include <thread>
#include <vector>

#include "pricetime/book.hpp"

using namespace pricetime;

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

constexpr const char* kReset = "\033[0m";
constexpr const char* kDim   = "\033[2m";
constexpr const char* kRed   = "\033[31m";
constexpr const char* kGreen = "\033[32m";
constexpr const char* kBold  = "\033[1m";

std::string bar(Qty q, Qty scale, int width) {
  if (scale <= 0) return std::string();
  auto n = static_cast<int>((static_cast<double>(q) / static_cast<double>(scale)) *
                            static_cast<double>(width));
  n = std::clamp(n, q > 0 ? 1 : 0, width);
  return std::string(static_cast<std::size_t>(n), '#');
}

std::string px(Price p) {
  char b[32];
  std::snprintf(b, sizeof(b), "%lld.%02lld", static_cast<long long>(p / 100),
                static_cast<long long>(p % 100));
  return std::string(b);
}

}  // namespace

int main(int argc, char** argv) {
  const int frames = (argc > 1) ? std::atoi(argv[1]) : 240;
  constexpr Price kFloor = 9'000, kCeil = 11'000, kMid = 10'000;
  constexpr int   kLevels = 10;

  Book book(kFloor, kCeil);
  Rng  rng(0xDEC0DE);
  std::deque<std::string> tape;

  std::vector<OrderId> live;
  OrderId next_id = 1;
  Price   drift   = kMid;
  std::uint64_t msgs = 0, trades = 0;
  Qty volume = 0;

  std::printf("\033[2J");  // clear once; frames redraw in place
  for (int f = 0; f < frames; ++f) {
    EventLog log;
    // A burst of flow per rendered frame, so the ladder visibly moves.
    for (int k = 0; k < 40; ++k) {
      drift = std::clamp<Price>(drift + rng.in(-1, 1), kFloor + 50, kCeil - 50);
      if (!live.empty() && rng.in(1, 100) <= 40) {
        const auto i = static_cast<std::size_t>(rng.in(0, static_cast<std::int64_t>(live.size()) - 1));
        book.cancel(CancelOrder{live[i], 0}, log);
        live[i] = live.back();
        live.pop_back();
      } else {
        NewOrder o;
        o.id    = next_id++;
        o.side  = rng.in(0, 1) == 0 ? Side::Buy : Side::Sell;
        o.type  = OrderType::Limit;
        // Most flow is passive: bids post at or below the drifting mid, asks
        // at or above it, which is how a book accumulates depth. A minority
        // is aggressive and crosses the spread, which is what prints trades.
        // Generating both sides from the same symmetric offset (the obvious
        // first attempt) makes every order cross instantly and the book never
        // builds a ladder at all.
        const bool aggressive = rng.in(1, 100) <= 18;
        o.tif = aggressive ? TimeInForce::IOC : TimeInForce::Day;
        const std::int64_t passive_off = rng.in(1, 9);
        const std::int64_t cross_off   = rng.in(0, 3);
        o.price = (o.side == Side::Buy)
                      ? (aggressive ? drift + cross_off : drift - passive_off)
                      : (aggressive ? drift - cross_off : drift + passive_off);
        o.price = std::clamp<Price>(o.price, kFloor, kCeil);
        o.qty   = rng.in(1, 60);
        book.submit(o, log);
        if (!aggressive) live.push_back(o.id);
      }
      ++msgs;
    }
    for (const Event& e : log) {
      if (e.kind != Event::Kind::Trade) continue;
      ++trades;
      volume += e.qty;
      char b[96];
      std::snprintf(b, sizeof(b), "%s%6lld%s @ %s%8s%s  %s",
                    kBold, static_cast<long long>(e.qty), kReset,
                    e.side == Side::Buy ? kGreen : kRed, px(e.price).c_str(),
                    kReset, e.side == Side::Buy ? "buy aggressor" : "sell aggressor");
      tape.push_front(b);
      if (tape.size() > 12) tape.pop_back();
    }

    const auto bids = book.depth(Side::Buy, kLevels);
    const auto asks = book.depth(Side::Sell, kLevels);
    Qty scale = 1;
    for (const auto& [p, q] : bids) scale = std::max(scale, q);
    for (const auto& [p, q] : asks) scale = std::max(scale, q);

    std::printf("\033[H");  // home
    std::printf("%s pricetime %s  live order book replay%s\n\n", kBold, kDim, kReset);
    std::printf("  %s%28s | %-28s%s\n", kDim, "BIDS", "ASKS", kReset);
    for (int i = kLevels - 1; i >= 0; --i) {
      std::string lb, la;
      std::string lpx = "        ", rpx = "        ";
      if (static_cast<std::size_t>(i) < bids.size()) {
        lb  = bar(bids[static_cast<std::size_t>(i)].second, scale, 16);
        lpx = px(bids[static_cast<std::size_t>(i)].first);
      }
      if (static_cast<std::size_t>(i) < asks.size()) {
        la  = bar(asks[static_cast<std::size_t>(i)].second, scale, 16);
        rpx = px(asks[static_cast<std::size_t>(i)].first);
      }
      std::printf("  %s%16s%s %8s | %-8s %s%-16s%s\033[K\n",
                  kGreen, lb.c_str(), kReset, lpx.c_str(), rpx.c_str(),
                  kRed, la.c_str(), kReset);
    }
    const Price bb = book.best_bid(), ba = book.best_ask();
    std::printf("\n  spread: %s%s\033[K\n",
                (bb != kInvalidPrice && ba != kInvalidPrice)
                    ? (px(ba) + " - " + px(bb) + "  =  " +
                       std::to_string(ba - bb) + " ticks").c_str()
                    : "(one-sided)", kReset);
    std::printf("  messages %llu   trades %llu   volume %lld   resting %zu\033[K\n\n",
                static_cast<unsigned long long>(msgs),
                static_cast<unsigned long long>(trades),
                static_cast<long long>(volume), book.resting_count());
    std::printf("  %sTAPE%s\033[K\n", kDim, kReset);
    for (const auto& t : tape) std::printf("   %s\033[K\n", t.c_str());
    for (std::size_t i = tape.size(); i < 12; ++i) std::printf("\033[K\n");
    std::fflush(stdout);
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
  }
  std::printf("\n");
  return 0;
}
