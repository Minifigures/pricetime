#pragma once

// Append-only input journal, and recovery from it.
//
// Why this exists
// ---------------
// This README opens with three real failures. Two of them were not matching
// bugs at all, they were RECOVERY failures:
//
//   Nasdaq, Facebook IPO 2012. The cross could not converge, so operators cut
//   over to a duplicate matching engine with the validation removed. That
//   engine's state was frozen at 11:11am and the cross finally printed at
//   11:30. 38,000 orders were excluded because the thing they failed over TO
//   did not have the state the thing they failed over FROM had.
//
//   NYSE, January 2023, fined $9M in March 2026. Primary and backup ran
//   simultaneously and 2,800+ opening auctions were treated as already done.
//   Nobody could answer "did the auctions actually happen", because there was
//   no authoritative record to ask.
//
// Both are the same shape: state existed in one place, and there was no way to
// reconstruct it somewhere else and prove the reconstruction was right.
//
// The design
// ----------
// The journal records INPUTS, not outputs. That is the whole trick, and it
// only works because the engine is deterministic: same inputs in the same
// order produce the same book and the same event stream, every time. So the
// inputs are the smaller, more durable thing to persist, and the outputs are
// derivable. This is event sourcing as LMAX uses it, and their phrasing is
// exact: "the current state of the Business Logic Processor is entirely
// derivable by processing the input events."
//
// Crash safety
// ------------
// A process can die between the write() and the fsync(), or midway through a
// record. So every record is length-prefixed and CRC-32 checksummed, and
// recovery stops at the first record that does not verify rather than trying
// to salvage past it. A torn tail is expected, not exceptional: it means the
// process died mid-append, and everything before it is still good.
//
// What this deliberately is NOT: replication, consensus, or failover. One
// journal on one disk. Getting from here to a replicated state machine is the
// next problem and it is a much larger one.

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "pricetime/book.hpp"
#include "pricetime/order.hpp"
#include "pricetime/types.hpp"

namespace pricetime {

// CRC-32 (IEEE 802.3, reflected). Small table, no dependency.
[[nodiscard]] std::uint32_t crc32(const void* data, std::size_t len,
                                  std::uint32_t seed = 0) noexcept;

enum class OpKind : std::uint8_t { New = 1, Cancel = 2, Replace = 3 };

// One journalled instruction. Fixed layout, little-endian, no padding
// surprises: every field is written explicitly rather than memcpy'ing the
// struct, so the on-disk format does not silently change when a compiler
// decides to lay the struct out differently.
struct JournalRecord {
  OpKind       kind = OpKind::New;
  NewOrder     nw;
  CancelOrder  cx;
  ReplaceOrder rp;

  [[nodiscard]] bool operator==(const JournalRecord&) const = default;
};

// Bytes on disk per record: u32 payload_len, payload, u32 crc32(payload).
inline constexpr std::uint32_t kJournalMagic   = 0x50544A31u;  // "PTJ1"
// Written by close() after the last record. Recovery uses it to tell a journal
// that ended because the writer finished from one that ended because the
// process died. Without it, a crash that happens to land on a record boundary
// is indistinguishable from a clean shutdown, and recovery reports "clean"
// while silently having lost every input after the cut. The value is far above
// kMaxRecordBytes so it can never be mistaken for a record length.
inline constexpr std::uint32_t kJournalEnd     = 0x454E4421u;  // "END!"
inline constexpr std::size_t   kMaxRecordBytes = 4096;

class Journal {
 public:
  ~Journal() { close(); }

  // Creates or truncates. Writes the file header.
  [[nodiscard]] bool create(const std::string& path);
  void close();

  // Appends one record. When `durable` is set, fsyncs before returning, which
  // is what a venue would do and is roughly 1000x slower.
  //
  // On by default, because the entire point of journalling before applying is
  // that a record survives the process. With buffering left on, append()
  // returns true as soon as the bytes reach a stdio buffer, so a kill loses
  // every input still in that buffer even though the engine had already
  // matched and acknowledged them. Pass false only where the durability is
  // genuinely not the thing being measured.
  [[nodiscard]] bool append(const JournalRecord& r, bool durable = true);
  [[nodiscard]] std::uint64_t records() const noexcept { return records_; }
  [[nodiscard]] const std::string& error() const noexcept { return error_; }

  // Reads every intact record from `path`. Stops at the first record that
  // fails length or CRC validation and reports how many bytes were discarded,
  // rather than pretending a torn tail did not happen.
  struct RecoveryReport {
    std::vector<JournalRecord> records;
    std::uint64_t bytes_read     = 0;
    std::uint64_t bytes_discarded = 0;
    bool          clean          = true;   // false when a torn tail was found
    // True only when the writer's end marker was found, which means every
    // record it wrote is present. `clean` is weaker: it says the byte stream
    // ended on a record boundary, which a crash can also produce.
    bool          complete       = false;
    std::string   note;
  };
  [[nodiscard]] static RecoveryReport recover(const std::string& path);

  // Replays records into a book, returning the event stream they produce.
  static void replay(const std::vector<JournalRecord>& recs, Book& into,
                     EventLog& out);

 private:
  std::FILE*    f_ = nullptr;
  std::uint64_t records_ = 0;
  bool          poisoned_ = false;
  std::string   error_;
};

}  // namespace pricetime
