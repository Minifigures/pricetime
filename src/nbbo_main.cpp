// Live cross-venue NBBO from a normalized feed on stdin.
//
//   ./scripts/feed_crypto.py | ./build/pricetime_nbbo
//
// Why stdin and not a websocket client in C++
// -------------------------------------------
// The engine builds with g++ and make and nothing else. Adding a TLS stack and
// a websocket implementation to get a demo working would trade that away for
// something that is not the interesting part. So the transport lives in a
// small helper script and the engine reads a normalized text protocol. The
// boundary is also honest about where the hard part is: parsing five venues'
// incompatible JSON is plumbing, consolidating them correctly is not.
//
// PROTOCOL, newline-delimited, space-separated, integer ticks:
//
//   V <venue_id> <name> <book|quote>     declare a venue
//   Q <venue_id> <bidpx> <bidsz> <askpx> <asksz>    top-of-book update
//   A <venue_id> <orderid> <B|S> <px> <sz>          add order   (book venues)
//   X <venue_id> <orderid>                          cancel order(book venues)
//   T <venue_id> <B|S> <px> <sz>                    trade print
//
// A venue declared `book` gets a real matching engine and its BBO is derived
// from reconstructed depth. A venue declared `quote` publishes top-of-book
// only. Both are valid NBBO inputs; the display says which is which, because
// claiming depth you were never sent is the easy lie here.

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <unordered_map>
#include <vector>

#include "pricetime/consolidator.hpp"

using namespace pricetime;

namespace {

constexpr const char* kReset = "\033[0m";
constexpr const char* kDim   = "\033[2m";
constexpr const char* kBold  = "\033[1m";
constexpr const char* kGreen = "\033[32m";
constexpr const char* kRed   = "\033[31m";
constexpr const char* kYell  = "\033[33m";
constexpr const char* kCyan  = "\033[36m";

// Crypto prices arrive scaled; the feed sends integer ticks and we render with
// two implied decimals, same convention as the rest of the engine.
std::string px(Price p) {
  if (p == kInvalidPrice) return "-";
  char b[48];
  std::snprintf(b, sizeof(b), "%lld.%02lld", static_cast<long long>(p / 100),
                static_cast<long long>(std::llabs(static_cast<long long>(p % 100))));
  return std::string(b);
}
std::string sz(Qty q) {
  char b[32];
  std::snprintf(b, sizeof(b), "%.4f", static_cast<double>(q) / 1e4);
  return std::string(b);
}

struct VenueInfo {
  std::string name;
  bool        book_backed = false;
  std::uint64_t msgs = 0;
};

}  // namespace

int main(int argc, char** argv) {
  const bool once = (argc > 1 && std::strcmp(argv[1], "--once") == 0);

  Consolidator cons;
  std::vector<VenueInfo> venues;
  std::unordered_map<int, VenueId> vid;
  constexpr SymbolId kSym = 1;
  // Crypto books span a wide band; sized generously and the two-tier book
  // keeps the hot ladder cache-resident regardless.
  constexpr Price kFloor = 1, kCeil = 100'000'000;

  std::uint64_t lines = 0, trades = 0, tt_count = 0, locked = 0, crossed = 0;
  Price worst_harm = 0;
  std::vector<std::string> tape;

  std::string line;
  auto render = [&]() {
    std::printf("\033[H\033[J");
    std::printf(" %spricetime%s  %slive cross-venue NBBO%s\n\n", kBold, kReset, kDim, kReset);
    std::printf("  %-12s %-6s %14s %12s   %14s %12s\n",
                "VENUE", "TYPE", "BID", "SIZE", "ASK", "SIZE");
    std::printf("  %s%s%s\n", kDim, std::string(76, '-').c_str(), kReset);
    const Nbbo n = cons.get_nbbo(kSym);
    for (VenueId v = 0; v < static_cast<VenueId>(venues.size()); ++v) {
      const Bbo q = cons.get_exchange_bbo(v, kSym);
      const bool best_bid = (n.bid_venue == v && q.has_bid());
      const bool best_ask = (n.ask_venue == v && q.has_ask());
      std::printf("  %-12s %s%-6s%s %s%14s%s %12s   %s%14s%s %12s\n",
                  venues[v].name.c_str(),
                  kDim, venues[v].book_backed ? "book" : "quote", kReset,
                  best_bid ? kGreen : "", px(q.bid_px).c_str(), best_bid ? kReset : "",
                  q.has_bid() ? sz(q.bid_sz).c_str() : "",
                  best_ask ? kRed : "", px(q.ask_px).c_str(), best_ask ? kReset : "",
                  q.has_ask() ? sz(q.ask_sz).c_str() : "");
    }
    std::printf("  %s%s%s\n", kDim, std::string(76, '-').c_str(), kReset);
    std::printf("  %sNBBO%s        %s%14s%s %12s   %s%14s%s %12s\n",
                kBold, kReset,
                kGreen, px(n.quote.bid_px).c_str(), kReset, sz(n.quote.bid_sz).c_str(),
                kRed,   px(n.quote.ask_px).c_str(), kReset, sz(n.quote.ask_sz).c_str());
    if (n.quote.has_bid() && n.quote.has_ask()) {
      const char* state = n.crossed() ? "CROSSED" : (n.locked() ? "LOCKED" : "");
      const char* col   = n.crossed() ? kRed : kYell;
      std::printf("  spread %s ticks   best bid %s%s%s / best ask %s%s%s   %s%s%s\n",
                  std::to_string(n.spread()).c_str(),
                  kCyan, venues[n.bid_venue].name.c_str(), kReset,
                  kCyan, venues[n.ask_venue].name.c_str(), kReset,
                  col, state, kReset);
    }
    std::printf("\n  messages %llu   trades %llu   locked %llu   crossed %llu   "
                "trade-throughs %llu",
                static_cast<unsigned long long>(lines),
                static_cast<unsigned long long>(trades),
                static_cast<unsigned long long>(locked),
                static_cast<unsigned long long>(crossed),
                static_cast<unsigned long long>(tt_count));
    if (tt_count > 0)
      std::printf("   worst %s ticks", std::to_string(worst_harm).c_str());
    std::printf("\n\n  %sTAPE%s\n", kDim, kReset);
    for (const auto& t : tape) std::printf("   %s\n", t.c_str());
    std::fflush(stdout);
  };

  while (std::getline(std::cin, line)) {
    if (line.empty()) continue;
    ++lines;
    char kind = line[0];
    std::vector<std::string> f;
    {
      std::size_t i = 0;
      while (i < line.size()) {
        while (i < line.size() && line[i] == ' ') ++i;
        std::size_t j = i;
        while (j < line.size() && line[j] != ' ') ++j;
        if (j > i) f.push_back(line.substr(i, j - i));
        i = j;
      }
    }
    auto num = [&](std::size_t i) -> long long {
      return i < f.size() ? std::atoll(f[i].c_str()) : 0;
    };

    if (kind == 'V' && f.size() >= 4) {
      const int raw = static_cast<int>(num(1));
      const auto id = cons.add_venue(f[2]);
      vid[raw] = id;
      venues.push_back({f[2], f[3] == "book", 0});
      if (f[3] == "book") cons.book(id, kSym, kFloor, kCeil);
      continue;
    }
    if (f.size() < 2) continue;
    const auto it = vid.find(static_cast<int>(num(1)));
    if (it == vid.end()) continue;
    const VenueId v = it->second;
    ++venues[v].msgs;

    EventLog log;
    switch (kind) {
      case 'Q': {
        if (f.size() < 6) break;
        Bbo q;
        q.bid_px = num(2) ? static_cast<Price>(num(2)) : kInvalidPrice;
        q.bid_sz = static_cast<Qty>(num(3));
        q.ask_px = num(4) ? static_cast<Price>(num(4)) : kInvalidPrice;
        q.ask_sz = static_cast<Qty>(num(5));
        cons.publish_quote(v, kSym, q);
        break;
      }
      case 'A': {
        if (f.size() < 6 || !venues[v].book_backed) break;
        NewOrder o;
        o.id    = static_cast<OrderId>(num(2));
        o.side  = (f[3] == "B") ? Side::Buy : Side::Sell;
        o.type  = OrderType::Limit;
        o.tif   = TimeInForce::Day;
        o.price = static_cast<Price>(num(4));
        o.qty   = static_cast<Qty>(num(5));
        if (o.qty > 0) cons.book(v, kSym, kFloor, kCeil).submit(o, log);
        break;
      }
      case 'X': {
        if (!venues[v].book_backed) break;
        cons.book(v, kSym, kFloor, kCeil)
            .cancel(CancelOrder{static_cast<OrderId>(num(2)), 0}, log);
        break;
      }
      case 'T': {
        if (f.size() < 5) break;
        ++trades;
        const Side agg = (f[2] == "B") ? Side::Buy : Side::Sell;
        const Price tpx = static_cast<Price>(num(3));
        const Qty   tsz = static_cast<Qty>(num(4));
        TradeThrough tt;
        if (cons.check_trade_through(kSym, v, agg, tpx, tsz, tt)) {
          ++tt_count;
          worst_harm = std::max(worst_harm, tt.harm);
          char b[192];
          std::snprintf(b, sizeof(b),
                        "%sTRADE-THROUGH%s %s printed %s, %s showed %s (%lld ticks)",
                        kRed, kReset, venues[v].name.c_str(), px(tpx).c_str(),
                        venues[tt.better_venue].name.c_str(),
                        px(tt.best_elsewhere).c_str(),
                        static_cast<long long>(tt.harm));
          tape.insert(tape.begin(), b);
        } else {
          char b[160];
          std::snprintf(b, sizeof(b), "%-9s %s%s%s %s @ %s",
                        venues[v].name.c_str(),
                        agg == Side::Buy ? kGreen : kRed,
                        agg == Side::Buy ? "buy " : "sell", kReset,
                        sz(tsz).c_str(), px(tpx).c_str());
          tape.insert(tape.begin(), b);
        }
        if (tape.size() > 10) tape.pop_back();
        break;
      }
      default: break;
    }

    const Nbbo n = cons.get_nbbo(kSym);
    if (n.crossed()) ++crossed; else if (n.locked()) ++locked;
    if (lines % 25 == 0 && !once) render();
  }
  render();
  return 0;
}
