#include "pricetime/journal.hpp"

#include <array>
#include <cstring>

#ifndef _WIN32
#include <unistd.h>
#endif

namespace pricetime {
namespace {

std::array<std::uint32_t, 256> make_crc_table() {
  std::array<std::uint32_t, 256> t{};
  for (std::uint32_t i = 0; i < 256; ++i) {
    std::uint32_t c = i;
    for (int k = 0; k < 8; ++k)
      c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
    t[i] = c;
  }
  return t;
}

const std::array<std::uint32_t, 256>& crc_table() {
  static const auto t = make_crc_table();
  return t;
}

// Explicit little-endian field writers. The on-disk format must not depend on
// struct layout, so nothing here memcpy's a whole struct.
struct Writer {
  std::vector<char>& b;
  void u8(std::uint8_t v)  { b.push_back(static_cast<char>(v)); }
  void u32(std::uint32_t v){ for (int i = 0; i < 4; ++i) u8(static_cast<std::uint8_t>(v >> (8 * i))); }
  void u64(std::uint64_t v){ for (int i = 0; i < 8; ++i) u8(static_cast<std::uint8_t>(v >> (8 * i))); }
  void i64(std::int64_t v) { u64(static_cast<std::uint64_t>(v)); }
};

struct Reader {
  const char* p; const char* end; bool ok = true;
  std::uint8_t u8() {
    if (p >= end) { ok = false; return 0; }
    return static_cast<std::uint8_t>(*p++);
  }
  std::uint32_t u32() { std::uint32_t v = 0; for (int i = 0; i < 4; ++i) v |= static_cast<std::uint32_t>(u8()) << (8 * i); return v; }
  std::uint64_t u64() { std::uint64_t v = 0; for (int i = 0; i < 8; ++i) v |= static_cast<std::uint64_t>(u8()) << (8 * i); return v; }
  std::int64_t  i64() { return static_cast<std::int64_t>(u64()); }
};

void encode(const JournalRecord& r, std::vector<char>& out) {
  Writer w{out};
  w.u8(static_cast<std::uint8_t>(r.kind));
  switch (r.kind) {
    case OpKind::New:
      w.u64(r.nw.id); w.u32(r.nw.owner);
      w.u8(static_cast<std::uint8_t>(r.nw.side));
      w.u8(static_cast<std::uint8_t>(r.nw.type));
      w.u8(static_cast<std::uint8_t>(r.nw.tif));
      w.i64(r.nw.price); w.i64(r.nw.qty); w.i64(r.nw.ts);
      break;
    case OpKind::Cancel:
      w.u64(r.cx.id); w.i64(r.cx.ts);
      break;
    case OpKind::Replace:
      w.u64(r.rp.id); w.i64(r.rp.price); w.i64(r.rp.qty); w.i64(r.rp.ts);
      break;
  }
}

bool decode(const char* p, std::size_t n, JournalRecord& r) {
  Reader rd{p, p + n};
  const auto k = rd.u8();
  if (k < 1 || k > 3) return false;
  r = JournalRecord{};
  r.kind = static_cast<OpKind>(k);
  switch (r.kind) {
    case OpKind::New:
      r.nw.id    = rd.u64();
      r.nw.owner = rd.u32();
      r.nw.side  = static_cast<Side>(rd.u8());
      r.nw.type  = static_cast<OrderType>(rd.u8());
      r.nw.tif   = static_cast<TimeInForce>(rd.u8());
      r.nw.price = rd.i64(); r.nw.qty = rd.i64(); r.nw.ts = rd.i64();
      break;
    case OpKind::Cancel:
      r.cx.id = rd.u64(); r.cx.ts = rd.i64();
      break;
    case OpKind::Replace:
      r.rp.id = rd.u64(); r.rp.price = rd.i64();
      r.rp.qty = rd.i64(); r.rp.ts = rd.i64();
      break;
  }
  return rd.ok;
}

}  // namespace

std::uint32_t crc32(const void* data, std::size_t len, std::uint32_t seed) noexcept {
  const auto& t = crc_table();
  std::uint32_t c = ~seed;
  const auto* p = static_cast<const unsigned char*>(data);
  for (std::size_t i = 0; i < len; ++i) c = t[(c ^ p[i]) & 0xFFu] ^ (c >> 8);
  return ~c;
}

bool Journal::create(const std::string& path) {
  close();
  f_ = std::fopen(path.c_str(), "wb");
  if (f_ == nullptr) { error_ = "cannot open " + path; return false; }
  std::vector<char> hdr;
  Writer{hdr}.u32(kJournalMagic);
  if (std::fwrite(hdr.data(), 1, hdr.size(), f_) != hdr.size()) {
    error_ = "short write on header"; close(); return false;
  }
  records_ = 0;
  poisoned_ = false;
  error_.clear();
  return true;
}

void Journal::close() {
  if (f_ == nullptr) return;
  // Mark the stream finished so recovery can distinguish "the writer stopped
  // here on purpose" from "the process died here". Best effort: if this fails
  // the journal is simply reported incomplete, which is the safe direction.
  if (!poisoned_) {
    std::vector<char> end;
    Writer{end}.u32(kJournalEnd);
    if (std::fwrite(end.data(), 1, end.size(), f_) != end.size())
      error_ = "short write on end marker";
  }
  if (std::fclose(f_) != 0 && error_.empty())
    error_ = "fclose failed, the tail of the journal may not be on disk";
  f_ = nullptr;
}

bool Journal::append(const JournalRecord& r, bool durable) {
  if (f_ == nullptr) { error_ = "journal not open"; return false; }
  // A failed append can leave a partial frame, and recovery stops at the first
  // torn record. Everything written after one would be unreachable, so refuse
  // to keep going rather than accumulate records nobody can ever read back.
  if (poisoned_) { error_ = "journal poisoned by an earlier failed append"; return false; }
  std::vector<char> payload;
  encode(r, payload);

  std::vector<char> frame;
  Writer w{frame};
  w.u32(static_cast<std::uint32_t>(payload.size()));
  frame.insert(frame.end(), payload.begin(), payload.end());
  Writer{frame}.u32(crc32(payload.data(), payload.size()));

  if (std::fwrite(frame.data(), 1, frame.size(), f_) != frame.size()) {
    error_ = "short write"; poisoned_ = true; return false;
  }
  if (durable) {
    // flush the C buffer, then ask the OS to actually put it on the platter.
    // Without the second step the data is in the page cache and a power loss
    // takes it; with it, this is roughly a thousand times slower per record.
    if (std::fflush(f_) != 0) { error_ = "fflush failed"; poisoned_ = true; return false; }
#ifndef _WIN32
    if (::fsync(::fileno(f_)) != 0) { error_ = "fsync failed"; poisoned_ = true; return false; }
#endif
  }
  ++records_;
  return true;
}

Journal::RecoveryReport Journal::recover(const std::string& path) {
  RecoveryReport rep;
  std::FILE* f = std::fopen(path.c_str(), "rb");
  if (f == nullptr) { rep.clean = false; rep.note = "cannot open " + path; return rep; }

  std::vector<char> buf;
  std::fseek(f, 0, SEEK_END);
  const long sz = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  if (sz > 0) {
    buf.resize(static_cast<std::size_t>(sz));
    if (std::fread(buf.data(), 1, buf.size(), f) != buf.size()) {
      std::fclose(f);
      rep.clean = false; rep.note = "short read";
      return rep;
    }
  }
  std::fclose(f);

  std::size_t off = 0;
  if (buf.size() < 4) {
    rep.clean = false;
    rep.bytes_discarded = buf.size();
    rep.note = "file shorter than the header";
    return rep;
  }
  {
    Reader rd{buf.data(), buf.data() + 4};
    if (rd.u32() != kJournalMagic) {
      rep.clean = false;
      rep.bytes_discarded = buf.size();
      rep.note = "bad magic; not a pricetime journal";
      return rep;
    }
  }
  off = 4;
  rep.bytes_read = off;   // the header is read whether or not a record follows

  for (;;) {
    if (off == buf.size()) break;                       // ended on a boundary
    if (off + 4 > buf.size()) {                          // torn length prefix
      rep.clean = false;
      rep.bytes_discarded = buf.size() - off;
      rep.note = "torn length prefix at tail";
      break;
    }
    Reader lr{buf.data() + off, buf.data() + off + 4};
    const std::uint32_t len = lr.u32();
    if (len == kJournalEnd) {          // the writer finished here
      off += 4;
      rep.bytes_read = off;
      rep.complete = true;
      if (off != buf.size()) {
        rep.clean = false;
        rep.bytes_discarded = buf.size() - off;
        rep.note = "trailing bytes after the end marker";
      }
      break;
    }
    if (len == 0 || len > kMaxRecordBytes) {
      rep.clean = false;
      rep.bytes_discarded = buf.size() - off;
      rep.note = "implausible record length at tail";
      break;
    }
    if (off + 4 + len + 4 > buf.size()) {                 // torn payload or crc
      rep.clean = false;
      rep.bytes_discarded = buf.size() - off;
      rep.note = "torn record at tail";
      break;
    }
    const char* payload = buf.data() + off + 4;
    Reader cr{payload + len, payload + len + 4};
    const std::uint32_t want = cr.u32();
    if (crc32(payload, len) != want) {
      rep.clean = false;
      rep.bytes_discarded = buf.size() - off;
      rep.note = "checksum mismatch at tail";
      break;
    }
    JournalRecord rec;
    if (!decode(payload, len, rec)) {
      rep.clean = false;
      rep.bytes_discarded = buf.size() - off;
      rep.note = "undecodable record at tail";
      break;
    }
    rep.records.push_back(rec);
    off += 4 + len + 4;
    rep.bytes_read = off;
  }
  return rep;
}

void Journal::replay(const std::vector<JournalRecord>& recs, Book& into,
                     EventLog& out) {
  for (const JournalRecord& r : recs) {
    switch (r.kind) {
      case OpKind::New:     into.submit(r.nw, out);  break;
      case OpKind::Cancel:  into.cancel(r.cx, out);  break;
      case OpKind::Replace: into.replace(r.rp, out); break;
    }
  }
}

}  // namespace pricetime
