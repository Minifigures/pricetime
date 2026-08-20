// Crash recovery, demonstrated end to end.
//
//   ./build/pricetime_recover [ops] [seed]
//
// Builds a book while journalling every input, kills the process at a random
// byte (including mid-record), recovers from the journal alone, and checks the
// rebuilt book against what the original actually held.

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "pricetime/journal.hpp"

using namespace pricetime;

namespace {

constexpr Price kFloor = 9'000, kCeil = 11'000;
constexpr const char* kReset = "\033[0m";
constexpr const char* kDim   = "\033[2m";
constexpr const char* kBold  = "\033[1m";
constexpr const char* kGreen = "\033[32m";
constexpr const char* kRed   = "\033[31m";
constexpr const char* kYell  = "\033[33m";

class Rng {
 public:
  explicit Rng(std::uint64_t s) : s_(s ? s : 1) {}
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

std::string px(Price p) {
  char b[32];
  std::snprintf(b, sizeof(b), "%lld.%02lld", static_cast<long long>(p / 100),
                static_cast<long long>(p % 100));
  return std::string(b);
}

void rule() { std::printf("  %s%s%s\n", kDim, std::string(66, '-').c_str(), kReset); }

}  // namespace

int main(int argc, char** argv) {
  const std::size_t n_ops = (argc > 1) ? static_cast<std::size_t>(std::atoll(argv[1])) : 5000;
  const std::uint64_t seed = (argc > 2) ? static_cast<std::uint64_t>(std::atoll(argv[2])) : 0xC0FFEE;
  const std::string path = "/tmp/pricetime_demo.journal";

  std::printf("\n  %spricetime crash recovery%s\n\n", kBold, kReset);

  // ---- 1. Run live, journalling every input as it is applied. --------------
  Journal j;
  if (!j.create(path)) { std::fprintf(stderr, "journal: %s\n", j.error().c_str()); return 1; }

  Book live(kFloor, kCeil);
  EventLog log;
  Rng rng(seed);
  std::vector<OrderId> alive;
  std::vector<JournalRecord> issued;   // kept so the verdict is not a tautology
  issued.reserve(n_ops);
  OrderId next_id = 1;
  std::uint64_t trades = 0;

  for (std::size_t i = 0; i < n_ops; ++i) {
    JournalRecord r;
    const auto roll = rng.in(1, 100);
    if (roll <= 70 || alive.empty()) {
      r.kind = OpKind::New;
      r.nw.id    = next_id++;
      r.nw.owner = static_cast<ParticipantId>(rng.in(1, 4));
      r.nw.side  = rng.in(0, 1) == 0 ? Side::Buy : Side::Sell;
      r.nw.type  = OrderType::Limit;
      r.nw.tif   = rng.in(1, 100) <= 15 ? TimeInForce::IOC : TimeInForce::Day;
      r.nw.price = rng.in(9960, 10040);
      r.nw.qty   = rng.in(1, 50);
      alive.push_back(r.nw.id);
    } else if (roll <= 88) {
      r.kind = OpKind::Cancel;
      r.cx.id = alive[static_cast<std::size_t>(rng.in(0, static_cast<std::int64_t>(alive.size()) - 1))];
    } else {
      r.kind = OpKind::Replace;
      r.rp.id    = alive[static_cast<std::size_t>(rng.in(0, static_cast<std::int64_t>(alive.size()) - 1))];
      r.rp.price = rng.in(9960, 10040);
      r.rp.qty   = rng.in(1, 50);
    }
    // Journal BEFORE applying. A record that is on disk but not yet applied is
    // recoverable; one applied but not journalled is lost forever.
    if (!j.append(r)) { std::fprintf(stderr, "append failed\n"); return 1; }
    issued.push_back(r);
    log.clear();
    switch (r.kind) {
      case OpKind::New:     live.submit(r.nw, log);  break;
      case OpKind::Cancel:  live.cancel(r.cx, log);  break;
      case OpKind::Replace: live.replace(r.rp, log); break;
    }
    for (const Event& e : log) if (e.kind == Event::Kind::Trade) ++trades;
  }
  j.close();

  std::printf("  %s1. RUNNING%s   journalled %llu inputs while matching\n",
              kBold, kReset, static_cast<unsigned long long>(j.records()));
  std::printf("              book: bid %s / ask %s, %zu resting, %llu trades\n",
              live.best_bid() == kInvalidPrice ? "-" : px(live.best_bid()).c_str(),
              live.best_ask() == kInvalidPrice ? "-" : px(live.best_ask()).c_str(),
              live.resting_count(), static_cast<unsigned long long>(trades));
  rule();

  // ---- 2. Kill it mid-write. ----------------------------------------------
  std::FILE* f = std::fopen(path.c_str(), "rb");
  std::fseek(f, 0, SEEK_END);
  const auto total = static_cast<std::size_t>(std::ftell(f));
  std::vector<char> bytes(total);
  std::fseek(f, 0, SEEK_SET);
  if (std::fread(bytes.data(), 1, total, f) != total) { std::fclose(f); return 1; }
  std::fclose(f);

  // Cut somewhere in the last few percent, so the tail is almost certainly
  // mid-record rather than on a tidy boundary.
  const auto cut = static_cast<std::size_t>(rng.in(
      static_cast<std::int64_t>(total * 96 / 100), static_cast<std::int64_t>(total) - 1));
  f = std::fopen(path.c_str(), "wb");
  if (std::fwrite(bytes.data(), 1, cut, f) != cut) { std::fclose(f); return 1; }
  std::fclose(f);

  std::printf("  %s2. CRASH%s     process killed mid-append\n", kBold, kReset);
  std::printf("              journal was %zu bytes, %s%zu bytes lost%s "
              "(cut at byte %zu)\n", total, kYell, total - cut, kReset, cut);
  rule();

  // ---- 3. Recover from the journal alone. ---------------------------------
  const auto rep = Journal::recover(path);
  Book rebuilt(kFloor, kCeil);
  EventLog rlog;
  Journal::replay(rep.records, rebuilt, rlog);

  std::printf("  %s3. RECOVERY%s  read %llu records from a cold start\n",
              kBold, kReset, static_cast<unsigned long long>(rep.records.size()));
  std::printf("              tail: %s%s%s\n",
              rep.clean ? kGreen : kYell,
              rep.clean ? "clean" : rep.note.c_str(), kReset);
  // A crash can land exactly on a record boundary, and then the byte stream
  // looks perfect: no torn tail, nothing discarded. Only the writer's end
  // marker distinguishes "it finished" from "it died here tidily", so report
  // that separately rather than letting a clean tail imply completeness.
  std::printf("              writer: %s%s%s\n",
              rep.complete ? kGreen : kYell,
              rep.complete ? "reached the end marker, nothing after this was lost"
                           : "no end marker, the process stopped before closing",
              kReset);
  std::printf("              book: bid %s / ask %s, %zu resting\n",
              rebuilt.best_bid() == kInvalidPrice ? "-" : px(rebuilt.best_bid()).c_str(),
              rebuilt.best_ask() == kInvalidPrice ? "-" : px(rebuilt.best_ask()).c_str(),
              rebuilt.resting_count());
  rule();

  // ---- 4. Prove it. -------------------------------------------------------
  // The check that matters: replay the FIRST N ops we actually issued into a
  // fresh book, where N is however many records survived, and compare that to
  // the recovered book.
  //
  // An earlier version of this compared the recovered book against a second
  // replay of the same recovered records. That is a tautology: two replays of
  // one input list are equal by construction, and it would have printed "yes"
  // even if recovery were completely broken. The comparison has to be against
  // a book built independently of the journal.
  Book truth(kFloor, kCeil);
  EventLog tlog;
  for (std::size_t i = 0; i < rep.records.size(); ++i) {
    switch (issued[i].kind) {
      case OpKind::New:     truth.submit(issued[i].nw, tlog);  break;
      case OpKind::Cancel:  truth.cancel(issued[i].cx, tlog);  break;
      case OpKind::Replace: truth.replace(issued[i].rp, tlog); break;
    }
  }

  const bool events_ok = (rlog == tlog);
  const bool depth_ok  = rebuilt.depth(Side::Buy, 64)  == truth.depth(Side::Buy, 64) &&
                         rebuilt.depth(Side::Sell, 64) == truth.depth(Side::Sell, 64) &&
                         rebuilt.resting_count() == truth.resting_count();
  const bool ok = events_ok && depth_ok;
  const std::size_t lost_ops = j.records() - rep.records.size();

  std::printf("  %s4. VERDICT%s   compared against an independent live run of the\n",
              kBold, kReset);
  std::printf("              same first %zu inputs, not against the journal\n",
              rep.records.size());
  std::printf("              event streams byte-identical : %s%s%s\n",
              events_ok ? kGreen : kRed, events_ok ? "yes" : "NO", kReset);
  std::printf("              book state identical         : %s%s%s\n",
              depth_ok ? kGreen : kRed, depth_ok ? "yes" : "NO", kReset);
  std::printf("              inputs lost to the crash     : %s%zu of %llu%s\n",
              lost_ops ? kYell : kGreen, lost_ops,
              static_cast<unsigned long long>(j.records()), kReset);
  std::printf("\n              %sThe engine is deterministic, so the inputs are the\n"
              "              state. Everything durably written replays to exactly\n"
              "              the book that produced it. What was lost is reported,\n"
              "              not guessed at.%s\n\n", kDim, kReset);

  std::remove(path.c_str());
  return ok ? 0 : 1;
}
