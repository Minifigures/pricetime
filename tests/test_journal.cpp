// Journalling and crash recovery.
//
// The central claim is that the engine's state is entirely derivable from its
// inputs. These tests try to falsify that, and the main one does it by killing
// the process at every possible instant: truncate the journal at EVERY byte
// offset, recover, and demand the result is exactly what running that prefix
// live would have produced. If any offset yields a book that disagrees, the
// recovery story is false.

#include <cstdio>
#include <string>
#include <vector>

#include "harness.hpp"
#include "pricetime/journal.hpp"
#include "pricetime/reference_book.hpp"

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
  std::int64_t in(std::int64_t a, std::int64_t b) {
    return a + static_cast<std::int64_t>(next() %
           static_cast<std::uint64_t>(b - a + 1));
  }
 private:
  std::uint64_t s_;
};

std::string tmp_path(const char* tag) {
  return std::string("/tmp/pricetime_journal_") + tag + ".bin";
}

std::vector<JournalRecord> make_ops(std::uint64_t seed, std::size_t n) {
  Rng rng(seed);
  std::vector<JournalRecord> out;
  std::vector<OrderId> live;
  OrderId next_id = 1;
  for (std::size_t i = 0; i < n; ++i) {
    JournalRecord r;
    const auto roll = rng.in(1, 100);
    if (roll <= 70 || live.empty()) {
      r.kind = OpKind::New;
      r.nw.id    = next_id++;
      r.nw.owner = static_cast<ParticipantId>(rng.in(1, 4));
      r.nw.side  = rng.in(0, 1) == 0 ? Side::Buy : Side::Sell;
      r.nw.type  = OrderType::Limit;
      r.nw.tif   = rng.in(1, 100) <= 15 ? TimeInForce::IOC : TimeInForce::Day;
      r.nw.price = rng.in(9950, 10050);
      r.nw.qty   = rng.in(1, 40);
      r.nw.ts    = static_cast<Nanos>(i) * 1000;
      live.push_back(r.nw.id);
    } else if (roll <= 88) {
      r.kind = OpKind::Cancel;
      r.cx.id = live[static_cast<std::size_t>(rng.in(0, static_cast<std::int64_t>(live.size()) - 1))];
      r.cx.ts = static_cast<Nanos>(i) * 1000;
    } else {
      r.kind = OpKind::Replace;
      r.rp.id    = live[static_cast<std::size_t>(rng.in(0, static_cast<std::int64_t>(live.size()) - 1))];
      r.rp.price = rng.in(9950, 10050);
      r.rp.qty   = rng.in(1, 40);
      r.rp.ts    = static_cast<Nanos>(i) * 1000;
    }
    out.push_back(r);
  }
  return out;
}

// Runs ops live and returns (event stream, final depth snapshot).
struct LiveResult {
  EventLog log;
  std::vector<std::pair<Price, Qty>> bids, asks;
  std::size_t resting = 0;
};

LiveResult run_live(const std::vector<JournalRecord>& ops, std::size_t upto) {
  LiveResult r;
  Book b(kFloor, kCeil);
  for (std::size_t i = 0; i < upto && i < ops.size(); ++i) {
    switch (ops[i].kind) {
      case OpKind::New:     b.submit(ops[i].nw, r.log);  break;
      case OpKind::Cancel:  b.cancel(ops[i].cx, r.log);  break;
      case OpKind::Replace: b.replace(ops[i].rp, r.log); break;
    }
  }
  r.bids = b.depth(Side::Buy, 32);
  r.asks = b.depth(Side::Sell, 32);
  r.resting = b.resting_count();
  return r;
}

bool write_journal(const std::string& path, const std::vector<JournalRecord>& ops) {
  Journal j;
  if (!j.create(path)) return false;
  for (const auto& r : ops) if (!j.append(r)) return false;
  j.close();
  return true;
}

std::vector<char> read_all(const std::string& path) {
  std::vector<char> b;
  std::FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return b;
  std::fseek(f, 0, SEEK_END); const long n = std::ftell(f); std::fseek(f, 0, SEEK_SET);
  if (n > 0) {
    b.resize(static_cast<std::size_t>(n));
    if (std::fread(b.data(), 1, b.size(), f) != b.size()) b.clear();
  }
  std::fclose(f);
  return b;
}

void write_bytes(const std::string& path, const char* p, std::size_t n) {
  std::FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) return;
  if (n > 0 && std::fwrite(p, 1, n, f) != n) { /* test helper; caller asserts */ }
  std::fclose(f);
}

}  // namespace

TEST(crc32_matches_known_vectors) {
  // Standard IEEE 802.3 check values.
  CHECK_EQ(crc32("", 0), 0x00000000u);
  CHECK_EQ(crc32("a", 1), 0xE8B7BE43u);
  CHECK_EQ(crc32("123456789", 9), 0xCBF43926u);
}

TEST(journal_roundtrip_reproduces_state_and_events_exactly) {
  for (std::uint64_t seed = 1; seed <= 8; ++seed) {
    const auto ops = make_ops(seed, 2000);
    const auto path = tmp_path("rt");
    CHECK(write_journal(path, ops));

    const auto rep = Journal::recover(path);
    CHECK(rep.clean);
    CHECK_EQ(rep.records.size(), ops.size());
    if (rep.records != ops) { CHECK(false); return; }

    const LiveResult live = run_live(ops, ops.size());
    Book recovered(kFloor, kCeil);
    EventLog rlog;
    Journal::replay(rep.records, recovered, rlog);

    // Same events, same book. Not equivalent: identical.
    if (rlog != live.log) { CHECK(false); return; }
    CHECK(recovered.depth(Side::Buy, 32)  == live.bids);
    CHECK(recovered.depth(Side::Sell, 32) == live.asks);
    CHECK_EQ(recovered.resting_count(), live.resting);
    std::remove(path.c_str());
  }
  CHECK(true);
}

// THE test. Kill the process at every possible byte.
TEST(recovery_is_correct_from_a_crash_at_every_byte_offset) {
  const auto ops = make_ops(42, 400);
  const auto full = tmp_path("full");
  CHECK(write_journal(full, ops));
  const std::vector<char> bytes = read_all(full);
  CHECK(bytes.size() > 100);

  // Walk the frame boundaries once, so each cut has an INDEPENDENTLY computed
  // expected record count. Deriving the expectation from rep.records.size(),
  // as this test used to, means a recover() that silently drops records still
  // passes: the expected state is recomputed to match whatever came back.
  std::vector<std::size_t> boundary;      // byte offset after each whole record
  {
    std::size_t off = 4;
    while (off + 4 <= bytes.size()) {
      std::uint32_t len = 0;
      for (int i = 0; i < 4; ++i)
        len |= static_cast<std::uint32_t>(
                   static_cast<unsigned char>(bytes[off + static_cast<std::size_t>(i)]))
               << (8 * i);
      if (len == kJournalEnd || len == 0 || len > kMaxRecordBytes) break;
      off += 4 + len + 4;
      if (off > bytes.size()) break;
      boundary.push_back(off);
    }
  }
  CHECK_EQ(boundary.size(), ops.size());

  const auto part = tmp_path("part");
  std::size_t checked = 0, torn = 0;

  for (std::size_t cut = 0; cut <= bytes.size(); ++cut) {
    write_bytes(part, bytes.data(), cut);
    const auto rep = Journal::recover(part);

    // How many whole records physically fit inside this prefix.
    std::size_t expect = 0;
    while (expect < boundary.size() && boundary[expect] <= cut) ++expect;
    if (rep.records.size() != expect) {
      std::fprintf(stderr,
                   "\n      cut at %zu: recovered %zu records, %zu were intact\n",
                   cut, rep.records.size(), expect);
      CHECK(false); return;
    }

    // Only a prefix cut at exactly the end can be complete.
    if (rep.complete && cut != bytes.size()) {
      std::fprintf(stderr, "\n      cut at %zu reported complete\n", cut);
      CHECK(false); return;
    }

    // Whatever survived must be a PREFIX of what was written, and replaying it
    // must equal running exactly that many ops live. A recovery that invents,
    // drops from the middle, or reorders would fail here.
    if (rep.records.size() > ops.size()) { CHECK(false); return; }
    for (std::size_t i = 0; i < rep.records.size(); ++i) {
      if (!(rep.records[i] == ops[i])) {
        std::fprintf(stderr, "\n      record %zu differs after cut at %zu\n", i, cut);
        CHECK(false); return;
      }
    }

    const LiveResult live = run_live(ops, rep.records.size());
    Book recovered(kFloor, kCeil);
    EventLog rlog;
    Journal::replay(rep.records, recovered, rlog);
    if (rlog != live.log ||
        recovered.depth(Side::Buy, 32)  != live.bids ||
        recovered.depth(Side::Sell, 32) != live.asks) {
      std::fprintf(stderr,
                   "\n      state diverged after truncation at byte %zu "
                   "(%zu records recovered)\n", cut, rep.records.size());
      CHECK(false); return;
    }
    if (!rep.clean) ++torn;
    ++checked;
  }
  std::fprintf(stderr, "\n      [%zu cut points checked, %zu had a torn tail] ",
               checked, torn);
  std::remove(full.c_str());
  std::remove(part.c_str());
  CHECK(checked == bytes.size() + 1);
}

TEST(a_single_flipped_bit_is_caught_by_the_checksum) {
  const auto ops = make_ops(7, 50);
  const auto path = tmp_path("bitflip");
  CHECK(write_journal(path, ops));
  std::vector<char> bytes = read_all(path);

  // Corrupt a byte inside the first record's payload, not the tail.
  CHECK(bytes.size() > 20);
  bytes[12] = static_cast<char>(bytes[12] ^ 0x40);
  write_bytes(path, bytes.data(), bytes.size());

  const auto rep = Journal::recover(path);
  CHECK(!rep.clean);
  // Recovery must stop AT the corruption, not read past it.
  CHECK(rep.records.empty());
  CHECK(rep.note.find("checksum") != std::string::npos);
  std::remove(path.c_str());
}

TEST(a_foreign_file_is_rejected_rather_than_misread) {
  const auto path = tmp_path("foreign");
  const char junk[] = "this is not a journal, it is a text file";
  write_bytes(path, junk, sizeof(junk));
  const auto rep = Journal::recover(path);
  CHECK(!rep.clean);
  CHECK(rep.records.empty());
  CHECK(rep.note.find("magic") != std::string::npos);
  std::remove(path.c_str());
}

TEST(an_empty_journal_recovers_to_an_empty_book) {
  const auto path = tmp_path("empty");
  Journal j;
  CHECK(j.create(path));
  j.close();
  const auto rep = Journal::recover(path);
  CHECK(rep.clean);
  CHECK(rep.records.empty());
  std::remove(path.c_str());
}

// A journal the writer closed normally carries its end marker, so recovery can
// state positively that every record written is present.
TEST(a_normally_closed_journal_recovers_as_complete) {
  const auto ops = make_ops(7, 60);
  const auto path = tmp_path("complete");
  CHECK(write_journal(path, ops));
  const auto rep = Journal::recover(path);
  CHECK(rep.complete);
  CHECK(rep.clean);
  CHECK_EQ(rep.records.size(), ops.size());
  std::remove(path.c_str());
}

// The one that mattered. A crash can land exactly on a record boundary, and
// then the byte stream looks structurally perfect: no torn tail, no bad CRC,
// nothing discarded. Recovery used to call that "clean" and say nothing else,
// so an operator reading the report concluded nothing had rewound while the
// inputs after the cut were gone. `clean` still describes the bytes. Only
// `complete` says the writer got to the end.
TEST(a_crash_on_a_record_boundary_is_clean_but_not_complete) {
  const auto ops = make_ops(11, 80);
  const auto full = tmp_path("boundary_full");
  CHECK(write_journal(full, ops));
  const std::vector<char> bytes = read_all(full);

  // Find a boundary partway through and cut exactly there.
  std::size_t off = 4, cut = 0, kept = 0;
  while (off + 4 <= bytes.size()) {
    std::uint32_t len = 0;
    for (int i = 0; i < 4; ++i)
      len |= static_cast<std::uint32_t>(
                 static_cast<unsigned char>(bytes[off + static_cast<std::size_t>(i)]))
             << (8 * i);
    if (len == kJournalEnd || len == 0 || len > kMaxRecordBytes) break;
    off += 4 + len + 4;
    ++kept;
    if (kept == ops.size() / 2) { cut = off; break; }
  }
  CHECK(cut > 4);

  const auto part = tmp_path("boundary_part");
  write_bytes(part, bytes.data(), cut);
  const auto rep = Journal::recover(part);

  CHECK_EQ(rep.records.size(), kept);
  CHECK(rep.clean);              // the bytes really do end on a boundary
  CHECK(!rep.complete);          // but the writer never got to say it was done
  CHECK(kept < ops.size());      // and inputs really were lost
  std::remove(full.c_str());
  std::remove(part.c_str());
}

// Accounting has to add up, including when nothing recovers. bytes_read used
// to omit the four-byte file header, so the invariant broke on every prefix
// that contained the header and no complete record.
TEST(recovery_byte_accounting_covers_the_whole_file) {
  const auto ops = make_ops(3, 40);
  const auto full = tmp_path("acct_full");
  CHECK(write_journal(full, ops));
  const std::vector<char> bytes = read_all(full);

  const auto part = tmp_path("acct_part");
  for (std::size_t cut = 4; cut <= bytes.size(); ++cut) {
    write_bytes(part, bytes.data(), cut);
    const auto rep = Journal::recover(part);
    if (rep.bytes_read + rep.bytes_discarded != cut) {
      std::fprintf(stderr,
                   "\n      cut %zu: read %llu + discarded %llu != %zu\n", cut,
                   static_cast<unsigned long long>(rep.bytes_read),
                   static_cast<unsigned long long>(rep.bytes_discarded), cut);
      CHECK(false); return;
    }
  }
  CHECK(true);
  std::remove(full.c_str());
  std::remove(part.c_str());
}

// An append that fails leaves a partial frame, and recovery stops at the first
// torn record, so anything written after one would be unreachable. The journal
// refuses rather than accumulating records nobody can read back.
TEST(a_poisoned_journal_refuses_further_appends) {
  const auto path = tmp_path("poison");
  Journal j;
  CHECK(j.create(path));
  const auto ops = make_ops(5, 3);
  for (const auto& r : ops) CHECK(j.append(r));
  j.close();
  // Appending to a closed journal must fail rather than silently succeed.
  CHECK(!j.append(ops[0]));
  std::remove(path.c_str());
}
