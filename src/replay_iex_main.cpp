// Replay a real IEX DEEP+ trading day through the engine.
//
//   ./build/pricetime_iex <file.pcap.gz> [SYMBOL]
//
// What this does and does not claim
// ---------------------------------
// DEEP+ publishes every displayed order added, modified, deleted, and executed
// on the IEX book. What it does NOT publish is the *aggressing* order: an
// execution message tells you a resting order traded, never who crossed into
// it. A feed replay therefore cannot, on its own, exercise a matching engine's
// matching path at all -- the venue's book is never crossed, so nothing ever
// matches.
//
// So each Order Executed message is turned back into the aggressor that must
// have caused it: an IOC order on the opposite side, at the reported price, for
// the reported size, from a different participant. That drives the real match
// path, and it gives a genuine correctness check against reality -- our engine
// must produce a trade against the same order ID, for the same size, at the
// same price the exchange reported.
//
// Price improvement is the interesting exception. DEEP+ v1.04 warns that "the
// executed price given by this message may differ from the original displayed
// price of the order, due to system price improvement or price sliding." Our
// engine always prints at the resting order's price, so those are counted and
// reported separately rather than scored as failures.
//
//   Data provided for free by IEX. By accessing or using IEX Historical Data,
//   you agree to the IEX Historical Data Terms of Use.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "pricetime/book.hpp"
#include "pricetime/iex.hpp"
#include "pricetime/surveillance.hpp"

using namespace pricetime;
using Clock = std::chrono::steady_clock;

namespace {

struct Rec {
  iex::MsgType  type;
  std::uint64_t oid;
  Side          side;
  Qty           size;
  Price         price;
  bool          maintain_priority;
  Nanos         ts;   // venue timestamp, used by surveillance only
};

std::string fmt_px(Price p) {
  char b[32];
  std::snprintf(b, sizeof(b), "%lld.%04lld",
                static_cast<long long>(p / iex::kPriceScale),
                static_cast<long long>(p % iex::kPriceScale));
  return std::string(b);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr,
                 "usage: %s <IEX_DPLS.pcap.gz> [SYMBOL]\n"
                 "  Downloads live at https://iextrading.com/api/1.0/hist\n"
                 "  (feed \"DEEP+\"). Run scripts/fetch_iex.sh to get one.\n",
                 argv[0]);
    return 2;
  }
  const std::string path = argv[1];
  const std::string want = (argc > 2) ? argv[2] : std::string();
  // Optional 3rd arg: clamp the ladder to N ticks centred on the median price,
  // dropping messages outside it. This exists to TEST the hypothesis that the
  // flat ladder's cost is dominated by band width (TLB/cache), not by
  // algorithmic work. It is a diagnostic, not a feature: dropping real
  // messages would be dishonest as a production path, and the count dropped is
  // always reported.
  const Price band_arg = (argc > 3) ? std::atoll(argv[3]) : 0;

  // ---- Pass 1: decode, pick the symbol, keep its messages in memory. -------
  // Decoding is deliberately outside the timed region. This benchmark measures
  // the matching engine, not a gzip pipe and a pcap parser; those get their own
  // number below.
  iex::DeepPlusReader rd;
  if (!rd.open(path)) {
    std::fprintf(stderr, "error: %s\n", rd.error().c_str());
    return 1;
  }

  std::unordered_map<std::string, std::uint64_t> per_symbol;
  std::vector<Rec>  recs;
  std::string       chosen = want;
  Price lo = 0, hi = 0;
  bool  have_range = false;

  iex::Decoded d;
  const auto decode_start = Clock::now();
  while (rd.next(d)) {
    const std::string sym = d.symbol.str();
    ++per_symbol[sym];
    if (!chosen.empty() && sym != chosen) continue;
    if (chosen.empty()) continue;  // resolved after the scan when unspecified
    if (d.price > 0) {
      if (!have_range) { lo = hi = d.price; have_range = true; }
      lo = std::min(lo, d.price);
      hi = std::max(hi, d.price);
    }
    recs.push_back({d.type, d.order_id, d.side, d.size, d.price,
                    d.maintain_priority, d.ts});
  }
  const double decode_secs =
      std::chrono::duration<double>(Clock::now() - decode_start).count();

  std::printf("IEX DEEP+ replay\n");
  std::printf("  file            : %s\n", path.c_str());
  std::printf("  packets         : %llu\n",
              static_cast<unsigned long long>(rd.packets()));
  std::printf("  order messages  : %llu decoded, %llu skipped (auction/system)\n",
              static_cast<unsigned long long>(rd.messages()),
              static_cast<unsigned long long>(rd.skipped()));
  std::printf("  decode rate     : %.2f M msg/sec (gunzip + pcap + IEX-TP)\n",
              static_cast<double>(rd.messages()) / decode_secs / 1e6);
  std::printf("  symbols seen    : %zu\n", per_symbol.size());

  if (chosen.empty()) {
    auto best = std::max_element(
        per_symbol.begin(), per_symbol.end(),
        [](const auto& a, const auto& b) { return a.second < b.second; });
    if (best == per_symbol.end()) {
      std::fprintf(stderr, "no messages decoded\n");
      return 1;
    }
    std::printf("\n  no symbol given; busiest is %s (%llu msgs).\n"
                "  re-run as: %s %s %s\n",
                best->first.c_str(),
                static_cast<unsigned long long>(best->second), argv[0],
                path.c_str(), best->first.c_str());
    std::printf("\n  top symbols by message count:\n");
    std::vector<std::pair<std::string, std::uint64_t>> v(per_symbol.begin(),
                                                         per_symbol.end());
    std::sort(v.begin(), v.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });
    for (std::size_t i = 0; i < std::min<std::size_t>(12, v.size()); ++i)
      std::printf("    %-8s %llu\n", v[i].first.c_str(),
                  static_cast<unsigned long long>(v[i].second));
    return 0;
  }

  if (recs.empty() || !have_range) {
    std::fprintf(stderr, "\nno messages for symbol %s\n", chosen.c_str());
    return 1;
  }

  // A flat ladder needs a bounded band. It is derived from the symbol's own
  // observed range with 5 percent of headroom, which is the honest way to size
  // it: no hand-tuned constant, and it is reported so the memory cost is
  // visible rather than hidden.
  Price floor, ceil;
  if (band_arg > 0) {
    const Price mid = (lo + hi) / 2;
    floor = std::max<Price>(1, mid - band_arg / 2);
    ceil  = mid + band_arg / 2;
  } else {
    const Price pad = std::max<Price>((hi - lo) / 20, 100);
    floor = std::max<Price>(1, lo - pad);
    ceil  = hi + pad;
  }
  // The accepted band is what the engine will take orders in. The HOT band is
  // the flat ladder; anything outside it goes to the ordered-map tier. Only
  // the hot band costs resident memory, so it is the one that matters.
  const Price hot_ticks = std::min<Price>(Book::kDefaultHotTicks, ceil - floor + 1);
  const double ladder_mb =
      static_cast<double>(hot_ticks) * 2.0 * 16.0 / (1024.0 * 1024.0);

  std::printf("\n  symbol          : %s\n", chosen.c_str());
  std::printf("  messages        : %zu\n", recs.size());
  std::printf("  price range     : %s .. %s\n", fmt_px(lo).c_str(),
              fmt_px(hi).c_str());
  std::printf("  accepted band   : %lld ticks\n",
              static_cast<long long>(ceil - floor + 1));
  std::printf("  hot ladder      : %lld ticks (~%.1f MB, both sides); rest "
              "served by the cold tier\n",
              static_cast<long long>(hot_ticks), ladder_mb);

  // ---- Pass 2: drive the engine. ------------------------------------------
  Book book(floor, ceil, SelfTradePolicy::None,
            std::max<std::size_t>(1u << 16, recs.size() / 4));
  EventLog log;
  log.reserve(64);

  std::uint64_t adds = 0, mods = 0, dels = 0, execs = 0;
  std::uint64_t exec_matched = 0, exec_unknown = 0, exec_price_improved = 0,
                exec_size_mismatch = 0;
  std::uint64_t synth_id = (1ull << 62);  // aggressor ids cannot collide

  // Surveillance runs alongside, reading the engine's own output. It never
  // feeds back into a matching decision, and it is excluded from the timed
  // region below so it cannot flatter or penalise the latency numbers.
  Surveillance surv(chosen);

  std::vector<double> ns;
  ns.reserve(recs.size());

  std::uint64_t out_of_band = 0;
  const auto t_start = Clock::now();
  for (const Rec& r : recs) {
    if (band_arg > 0 && r.price > 0 && (r.price < floor || r.price > ceil)) {
      ++out_of_band;
      continue;
    }
    const auto t0 = Clock::now();
    log.clear();
    switch (r.type) {
      case iex::MsgType::AddOrder: {
        NewOrder o;
        o.id = r.oid; o.owner = 1; o.side = r.side;
        o.type = OrderType::Limit; o.tif = TimeInForce::Day;
        o.price = r.price; o.qty = r.size;
        book.submit(o, log);
        ++adds;
        break;
      }
      case iex::MsgType::OrderModify:
        book.replace(ReplaceOrder{r.oid, r.price, r.size, 0}, log);
        ++mods;
        break;
      case iex::MsgType::OrderDelete:
        book.cancel(CancelOrder{r.oid, 0}, log);
        ++dels;
        break;
      case iex::MsgType::OrderExecuted: {
        // Reconstruct the aggressor the feed does not publish.
        const Qty resting = book.qty_at(Side::Buy, r.price) +
                            book.qty_at(Side::Sell, r.price);
        if (resting == 0) { ++exec_unknown; ++execs; break; }
        const Side maker = book.qty_at(Side::Buy, r.price) > 0 ? Side::Buy
                                                               : Side::Sell;
        NewOrder agg;
        agg.id = ++synth_id; agg.owner = 2; agg.side = opposite(maker);
        agg.type = OrderType::Limit; agg.tif = TimeInForce::IOC;
        agg.price = r.price; agg.qty = r.size;
        book.submit(agg, log);
        ++execs;

        Qty filled = 0;
        bool hit = false;
        for (const Event& e : log) {
          if (e.kind != Event::Kind::Trade) continue;
          filled += e.qty;
          if (e.contra_id == r.oid) hit = true;
          if (e.price != r.price) ++exec_price_improved;
        }
        if (hit) ++exec_matched;
        if (filled != r.size) ++exec_size_mismatch;
        break;
      }
      default: break;
    }
    const auto t1 = Clock::now();
    ns.push_back(static_cast<double>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count()));

    for (const Event& e : log) surv.observe(e, r.ts);
  }
  const double secs = std::chrono::duration<double>(Clock::now() - t_start).count();

  std::sort(ns.begin(), ns.end());
  auto pick = [&](double q) {
    return ns.empty() ? 0.0
                      : ns[static_cast<std::size_t>(q * static_cast<double>(ns.size() - 1))];
  };

  std::printf("\n  applied         : %llu add / %llu modify / %llu delete / %llu execute\n",
              static_cast<unsigned long long>(adds),
              static_cast<unsigned long long>(mods),
              static_cast<unsigned long long>(dels),
              static_cast<unsigned long long>(execs));
  std::printf("  engine latency  : p50 %.0f  p90 %.0f  p99 %.0f  p99.9 %.0f ns\n",
              pick(0.50), pick(0.90), pick(0.99), pick(0.999));
  std::printf("  throughput      : %.2f M msg/sec on real exchange flow\n",
              static_cast<double>(recs.size() - out_of_band) / secs / 1e6);
  if (band_arg > 0)
    std::printf("  DIAGNOSTIC MODE : band clamped to %lld ticks; %llu of %zu "
                "messages dropped as out-of-band\n",
                static_cast<long long>(band_arg),
                static_cast<unsigned long long>(out_of_band), recs.size());
  std::printf("  final book      : bid %s / ask %s, %zu resting\n",
              book.best_bid() == kInvalidPrice ? "-" : fmt_px(book.best_bid()).c_str(),
              book.best_ask() == kInvalidPrice ? "-" : fmt_px(book.best_ask()).c_str(),
              book.resting_count());

  std::printf("\n  VALIDATION against the venue's own execution reports\n");
  std::printf("    executions replayed        : %llu\n",
              static_cast<unsigned long long>(execs));
  std::printf("    matched the reported order : %llu (%.1f%%)\n",
              static_cast<unsigned long long>(exec_matched),
              execs ? 100.0 * static_cast<double>(exec_matched) /
                          static_cast<double>(execs) : 0.0);
  std::printf("    order not in our book      : %llu\n",
              static_cast<unsigned long long>(exec_unknown));
  std::printf("    filled size != reported    : %llu\n",
              static_cast<unsigned long long>(exec_size_mismatch));
  std::printf("    printed off reported price : %llu  (IEX price improvement /\n"
              "                                      price sliding; expected)\n",
              static_cast<unsigned long long>(exec_price_improved));
  std::printf("\n%s\n", surv.report().c_str());

  // Machine-readable findings for the optional narrative stage. Written to a
  // file rather than stdout so the human report above stays readable.
  if (FILE* jf = std::fopen("surveillance.json", "w")) {
    std::fprintf(jf, "%s\n", surv.findings_json().c_str());
    std::fclose(jf);
    std::printf("  findings written to surveillance.json\n"
                "  optional narrative: ./scripts/explain.sh surveillance.json\n\n");
  }
  return 0;
}
