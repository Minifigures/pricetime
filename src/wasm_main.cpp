// WebAssembly binding: the real engine, in a browser.
//
// This is not a reimplementation or a simulation. It compiles the same
// Book, the same matching logic, and the same event stream that the native
// binary and the differential fuzz exercise. The only thing this file adds is
// a C-ABI surface for JavaScript to call, plus a JSON serializer for the
// depth snapshot, because passing structs across the WASM boundary is not
// worth the ceremony for a demo.
//
// Build:  em++ -O3 -std=c++20 -Iinclude \
//              src/wasm_main.cpp src/book.cpp src/reference_book.cpp \
//              -o web/public/pricetime.js -sEXPORTED_RUNTIME_METHODS=ccall,cwrap \
//              -sMODULARIZE -sEXPORT_NAME=PricetimeModule -sALLOW_MEMORY_GROWTH
//
// em++, not emcc: emcc links as C and leaves operator delete undefined.

#include <emscripten/emscripten.h>

#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>

#include "pricetime/book.hpp"
#include "pricetime/reference_book.hpp"

using namespace pricetime;

namespace {

constexpr Price kFloor = 9'000, kCeil = 11'000, kMid = 10'000;

// Same xorshift64* the native benchmarks use, so the browser and the terminal
// produce the same sequence from the same seed.
class Rng {
 public:
  explicit Rng(std::uint64_t s) : s_(s ? s : 0x9E3779B97F4A7C15ull) {}
  std::uint64_t next() {
    s_ ^= s_ >> 12; s_ ^= s_ << 25; s_ ^= s_ >> 27;
    return s_ * 0x2545F4914F6CDD1Dull;
  }
  std::int64_t in(std::int64_t a, std::int64_t b) {
    return a + static_cast<std::int64_t>(next() %
           static_cast<std::uint64_t>(b - a + 1));
  }
 private:
  std::uint64_t s_;
};

struct Sim {
  // BOTH engines, driven by identical input. This is the project's thesis
  // running in a browser rather than asserted in a README: the optimized book
  // is not trusted because it looks correct, it is trusted because it produces
  // byte-identical output to an implementation that is obviously correct.
  Book          book{kFloor, kCeil};
  ReferenceBook ref{};
  std::uint64_t compared = 0;   // events checked event-for-event
  std::uint64_t diverged = 0;   // must stay zero
  std::string   last_line;      // most recent event, rendered
  Rng  rng{0xDEC0DE};
  std::vector<OrderId> live;
  OrderId next_id = 1;
  Price   drift = kMid;
  std::uint64_t msgs = 0, trades = 0, rejects = 0;
  Qty vol = 0;
  double last_ns = 0.0;
  std::vector<std::pair<Price, Qty>> tape;
  std::string json;
};

Sim& sim() { static Sim s; return s; }

// snprintf truncates silently on overflow. The numeric line below outgrew a
// 96-byte buffer once the counters reached seven digits, which cut the JSON
// mid-key ("diverged": with no value) and left JSON.parse throwing on every
// tick after a few minutes. Size for the worst case and refuse to append a
// fragment rather than emit malformed JSON.
template <typename... Args>
void append_fmt(std::string& out, const char* fmt, Args... args) {
  char buf[512];
  const int n = std::snprintf(buf, sizeof(buf), fmt, args...);
  if (n > 0 && static_cast<std::size_t>(n) < sizeof(buf)) out += buf;
}


}  // namespace

extern "C" {

EMSCRIPTEN_KEEPALIVE void pt_reset(int seed) {
  Sim& s = sim();
  s.book = Book(kFloor, kCeil);
  s.ref  = ReferenceBook();
  s.compared = 0;
  s.diverged = 0;
  s.last_line.clear();
  s.rng = Rng(static_cast<std::uint64_t>(seed));
  s.live.clear(); s.next_id = 1; s.drift = kMid;
  s.msgs = s.trades = s.rejects = 0; s.vol = 0; s.tape.clear();
}

// Applies `n` messages and returns nothing; call pt_snapshot() to read state.
EMSCRIPTEN_KEEPALIVE void pt_step(int n) {
  Sim& s = sim();
  EventLog log, rlog;
  log.reserve(64);
  rlog.reserve(64);
  const double t0 = emscripten_get_now();
  for (int k = 0; k < n; ++k) {
    log.clear();
    rlog.clear();
    s.drift = std::clamp<Price>(s.drift + s.rng.in(-1, 1), kFloor + 50, kCeil - 50);
    if (!s.live.empty() && s.rng.in(1, 100) <= 40) {
      const auto i = static_cast<std::size_t>(
          s.rng.in(0, static_cast<std::int64_t>(s.live.size()) - 1));
      const CancelOrder cx{s.live[i], 0};
      s.book.cancel(cx, log);
      s.ref.cancel(cx, rlog);
      s.live[i] = s.live.back();
      s.live.pop_back();
    } else {
      NewOrder o;
      o.id = s.next_id++;
      o.side = s.rng.in(0, 1) == 0 ? Side::Buy : Side::Sell;
      o.type = OrderType::Limit;
      const bool agg = s.rng.in(1, 100) <= 18;
      o.tif = agg ? TimeInForce::IOC : TimeInForce::Day;
      const std::int64_t po = s.rng.in(1, 9), co = s.rng.in(0, 3);
      o.price = (o.side == Side::Buy) ? (agg ? s.drift + co : s.drift - po)
                                      : (agg ? s.drift - co : s.drift + po);
      o.price = std::clamp<Price>(o.price, kFloor, kCeil);
      o.qty = s.rng.in(1, 60);
      s.book.submit(o, log);
      s.ref.submit(o, rlog);
      if (!agg) s.live.push_back(o.id);
    }
    ++s.msgs;

    // Compare the two streams event for event, exactly as the differential
    // test does natively. A mismatch here would mean the engine in this
    // browser tab disagrees with its own specification.
    if (log.size() != rlog.size()) {
      ++s.diverged;
    } else {
      for (std::size_t i = 0; i < log.size(); ++i) {
        ++s.compared;
        if (!(log[i] == rlog[i])) ++s.diverged;
      }
    }
    if (!log.empty()) s.last_line = to_line(log.back());

    for (const Event& e : log) {
      if (e.kind == Event::Kind::Trade) {
        ++s.trades; s.vol += e.qty;
        s.tape.insert(s.tape.begin(), {e.price, e.qty});
        if (s.tape.size() > 8) s.tape.pop_back();
      } else if (e.kind == Event::Kind::Rejected) {
        ++s.rejects;
      }
    }
  }
  const double t1 = emscripten_get_now();
  // emscripten_get_now() is millisecond-resolution wall clock, and browsers
  // deliberately coarsen it against timing attacks. This is an order-of-
  // magnitude indication only; the real measurements are native and live in
  // the README.
  if (n > 0) s.last_ns = (t1 - t0) * 1e6 / static_cast<double>(n);
}

EMSCRIPTEN_KEEPALIVE const char* pt_snapshot() {
  Sim& s = sim();
  const auto b = s.book.depth(Side::Buy, 10);
  const auto a = s.book.depth(Side::Sell, 10);
  std::string& j = s.json;
  j = "{\"bids\":[";
  for (std::size_t i = 0; i < b.size(); ++i) {
    append_fmt(j, "%s[%lld,%lld]", i ? "," : "",
               static_cast<long long>(b[i].first),
               static_cast<long long>(b[i].second));
  }
  j += "],\"asks\":[";
  for (std::size_t i = 0; i < a.size(); ++i) {
    append_fmt(j, "%s[%lld,%lld]", i ? "," : "",
               static_cast<long long>(a[i].first),
               static_cast<long long>(a[i].second));
  }
  j += "],\"tape\":[";
  for (std::size_t i = 0; i < s.tape.size(); ++i) {
    append_fmt(j, "%s[%lld,%lld]", i ? "," : "",
               static_cast<long long>(s.tape[i].first),
               static_cast<long long>(s.tape[i].second));
  }
  const Price bb = s.book.best_bid(), ba = s.book.best_ask();
  append_fmt(j, "],\"bid\":%lld,\"ask\":%lld,",
             static_cast<long long>(bb == kInvalidPrice ? 0 : bb),
             static_cast<long long>(ba == kInvalidPrice ? 0 : ba));
  append_fmt(j, "\"msgs\":%llu,\"trades\":%llu,\"vol\":%lld,\"resting\":%zu,",
             static_cast<unsigned long long>(s.msgs),
             static_cast<unsigned long long>(s.trades),
             static_cast<long long>(s.vol), s.book.resting_count());
  append_fmt(j, "\"ns\":%.0f,\"compared\":%llu,\"diverged\":%llu,",
             s.last_ns,
             static_cast<unsigned long long>(s.compared),
             static_cast<unsigned long long>(s.diverged));
  j += "\"last\":\"";
  for (char c : s.last_line) { if (c != '"' && c != '\\') j += c; }
  j += "\"}";
  return j.c_str();
}

}  // extern "C"
