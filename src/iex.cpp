#include "pricetime/iex.hpp"

#include <algorithm>
#include <cstring>
#include <vector>

namespace pricetime::iex {
namespace {

// All reads go through memcpy rather than a reinterpret_cast to a wider type.
// Packet payloads are not guaranteed aligned, and a misaligned load is
// undefined behaviour that happens to work on x86 right up until it does not.
// The compiler folds these to a single mov at -O2.
template <class T>
[[nodiscard]] T le(const char* p) noexcept {
  T v;
  std::memcpy(&v, p, sizeof(T));
  return v;  // host is little-endian on every target this is built for
}

[[nodiscard]] std::uint16_t be16(const char* p) noexcept {
  return static_cast<std::uint16_t>((static_cast<unsigned char>(p[0]) << 8) |
                                     static_cast<unsigned char>(p[1]));
}

constexpr std::size_t kPcapGlobalHeader = 24;
constexpr std::size_t kPcapRecordHeader = 16;
constexpr std::size_t kEthHeader        = 14;
constexpr std::size_t kUdpHeader        = 8;
constexpr std::size_t kIexTpHeader      = 40;
constexpr std::uint32_t kPcapMagicLE    = 0xA1B2C3D4u;
constexpr std::uint32_t kPcapNgSHB      = 0x0A0D0D0Au;  // Section Header Block
constexpr std::uint32_t kPcapNgEPB      = 0x00000006u;  // Enhanced Packet Block

bool read_exact(std::FILE* f, char* dst, std::size_t n) {
  return std::fread(dst, 1, n, f) == n;
}

}  // namespace

bool DeepPlusReader::open(const std::string& path) {
  close();
  error_.clear();
  truncated_ = false;
  at_record_boundary_ = true;
  // Single-quote the path and escape embedded quotes; these files live under
  // user-chosen directories.
  std::string safe;
  safe.reserve(path.size() + 8);
  for (char c : path) {
    if (c == '\'') safe += "'\\''";
    else safe += c;
  }
  const std::string cmd = "gunzip -c '" + safe + "' 2>/dev/null";
  pipe_ = ::popen(cmd.c_str(), "r");
  if (pipe_ == nullptr) {
    error_ = "popen failed for: " + path;
    return false;
  }
  char head[8];
  if (!read_exact(pipe_, head, 8)) {
    error_ = "short read on capture header (is the file a gzipped capture?)";
    close();
    return false;
  }
  const auto magic = le<std::uint32_t>(head);
  if (magic == kPcapMagicLE) {
    container_ = Container::Pcap;
    char rest[kPcapGlobalHeader - 8];
    if (!read_exact(pipe_, rest, sizeof(rest))) {
      error_ = "short read on pcap global header";
      close();
      return false;
    }
    return true;
  }
  if (magic == kPcapNgSHB) {
    container_ = Container::PcapNg;
    // head holds Block Type + Block Total Length. Consume the remainder of the
    // Section Header Block; its options are not needed.
    const auto total = le<std::uint32_t>(head + 4);
    if (total < 12 || total > (1u << 20)) {
      error_ = "implausible pcapng section header length";
      close();
      return false;
    }
    std::vector<char> skip(total - 8);
    if (!read_exact(pipe_, skip.data(), skip.size())) {
      error_ = "short read on pcapng section header";
      close();
      return false;
    }
    return true;
  }
  char buf[128];
  std::snprintf(buf, sizeof(buf),
                "unrecognised capture magic 0x%08X "
                "(expected 0x%08X classic pcap or 0x%08X pcapng)",
                magic, kPcapMagicLE, kPcapNgSHB);
  error_ = buf;
  close();
  return false;
}

void DeepPlusReader::close() {
  if (pipe_ != nullptr) {
    const int status = ::pclose(pipe_);
    pipe_ = nullptr;
    // Only meaningful if the reader ran to what it took for a clean end. A
    // caller that stops early leaves gunzip to die of SIGPIPE, which is not
    // corruption. gzip reports CRC and length failures only in its exit code,
    // and that code was previously discarded.
    if (status != 0 && at_record_boundary_ && !truncated_) {
      truncated_ = true;
      if (error_.empty())
        error_ = "decompressor exited non-zero: the capture is corrupt or "
                 "truncated, so the decoded stream is incomplete";
    }
  }
  pkt_.clear();
  pkt_pos_ = 0;
  remaining_in_packet_ = 0;
}

// Classic pcap: a 16-byte record header then the frame.
bool DeepPlusReader::next_frame_pcap(std::vector<char>& frame) {
  char rh[kPcapRecordHeader];
  at_record_boundary_ = true;
  if (!read_exact(pipe_, rh, kPcapRecordHeader)) return false;  // clean end
  at_record_boundary_ = false;
  const auto incl = le<std::uint32_t>(rh + 8);
  if (incl == 0 || incl > (1u << 20)) {
    truncated_ = true;
    error_ = "implausible record length in capture, stopping";
    return false;
  }
  frame.resize(incl);
  if (!read_exact(pipe_, frame.data(), incl)) {
    truncated_ = true;
    error_ = "capture ends mid-record";
    return false;
  }
  at_record_boundary_ = true;
  return true;
}

// pcapng: a stream of typed blocks. Only Enhanced Packet Blocks carry frames;
// everything else (interface descriptions, statistics, name resolution) is
// skipped by its declared length. Frame data is padded to a 32-bit boundary.
bool DeepPlusReader::next_frame_pcapng(std::vector<char>& frame) {
  for (;;) {
    char bh[8];
    at_record_boundary_ = true;
    if (!read_exact(pipe_, bh, 8)) return false;  // clean end
    at_record_boundary_ = false;
    const auto type  = le<std::uint32_t>(bh);
    const auto total = le<std::uint32_t>(bh + 4);
    if (total < 12 || total > (1u << 24)) {
      truncated_ = true;
      error_ = "implausible pcapng block length, stopping";
      return false;
    }

    std::vector<char> body(total - 8);
    if (!read_exact(pipe_, body.data(), body.size())) {
      truncated_ = true;
      error_ = "capture ends mid-block";
      return false;
    }
    at_record_boundary_ = true;
    if (type != kPcapNgEPB) continue;
    if (body.size() < 20) continue;

    const auto caplen = le<std::uint32_t>(body.data() + 12);
    if (caplen == 0 || caplen > body.size() - 20) continue;
    frame.assign(body.begin() + 20,
                 body.begin() + 20 + static_cast<std::ptrdiff_t>(caplen));
    return true;
  }
}

bool DeepPlusReader::fill_packet() {
  std::vector<char> frame;
  for (;;) {
    const bool ok = (container_ == Container::Pcap) ? next_frame_pcap(frame)
                                                    : next_frame_pcapng(frame);
    if (!ok) return false;
    const auto incl = static_cast<std::uint32_t>(frame.size());
    ++packets_;

    if (incl < kEthHeader + 20 + kUdpHeader + kIexTpHeader) continue;
    if (be16(frame.data() + 12) != 0x0800) continue;  // not IPv4

    const auto ihl = static_cast<std::size_t>(
        (static_cast<unsigned char>(frame[kEthHeader]) & 0x0F) * 4);
    const std::size_t udp_body = kEthHeader + ihl + kUdpHeader;
    if (udp_body + kIexTpHeader > incl) continue;

    const char* tp = frame.data() + udp_body;
    if (le<std::uint16_t>(tp + 2) != kProtocolDeepPlus) continue;

    // IEX-TP v1 header, 40 bytes: version(0) reserved(1) protocol(2..3)
    // channel(4..7) session(8..11) payload length(12..13) message count(14..15)
    // stream offset(16..23) first sequence(24..31) send time(32..39). Message
    // count was previously read from offset 16, which is the low half of the
    // stream offset. That number happens to be a large no-op ceiling most of
    // the time, because the loop is also bounded by the block size, so the
    // decode looked correct. It is not: whenever the low 16 bits of the stream
    // offset came out zero the whole packet was dropped, and whenever they came
    // out below the true count the tail of the packet was dropped.
    const auto payload_len = le<std::uint16_t>(tp + 12);
    const auto msg_count = le<std::uint16_t>(tp + 14);
    if (msg_count == 0) continue;

    // Prefer the length the protocol states over what the capture happens to
    // carry, so Ethernet padding or an included FCS cannot become message
    // bytes. Clamp to what is actually present: the header is not trusted.
    const std::size_t avail = static_cast<std::size_t>(incl) - udp_body -
                              kIexTpHeader;
    const std::size_t block = payload_len > 0
                                  ? std::min(static_cast<std::size_t>(payload_len), avail)
                                  : avail;
    pkt_.assign(tp + kIexTpHeader, block);  // just the message block
    pkt_pos_ = 0;
    remaining_in_packet_ = msg_count;
    return true;
  }
}

bool DeepPlusReader::next(Decoded& out) {
  if (pipe_ == nullptr) return false;
  for (;;) {
    if (remaining_in_packet_ == 0) {
      if (!fill_packet()) return false;
    }
    if (pkt_pos_ + 2 > pkt_.size()) { remaining_in_packet_ = 0; continue; }
    const auto len = le<std::uint16_t>(pkt_.data() + pkt_pos_);
    pkt_pos_ += 2;
    if (pkt_pos_ + len > pkt_.size()) { remaining_in_packet_ = 0; continue; }
    const char* m = pkt_.data() + pkt_pos_;
    pkt_pos_ += len;
    --remaining_in_packet_;
    if (len == 0) continue;

    const char t = m[0];
    // Offsets are straight out of DEEP+ v1.04. Add/Modify/Executed are all
    // 38 bytes with an identical prefix layout; Delete is 26 and stops after
    // the order ID.
    switch (t) {
      case 'a': {
        if (len < 38) { ++skipped_; continue; }
        out.type     = MsgType::AddOrder;
        out.side     = (m[1] == '8') ? Side::Buy : Side::Sell;
        out.ts       = le<std::int64_t>(m + 2);
        std::memcpy(out.symbol.c, m + 10, 8);
        out.symbol.c[8] = '\0';
        out.order_id = le<std::uint64_t>(m + 18);
        out.size     = static_cast<Qty>(le<std::uint32_t>(m + 26));
        out.price    = le<std::int64_t>(m + 30);
        out.maintain_priority = false;
        ++messages_;
        return true;
      }
      case 'M': {
        if (len < 38) { ++skipped_; continue; }
        out.type = MsgType::OrderModify;
        // Bit 0: 0 = Reset Priority, 1 = Maintain Priority. The venue tells us
        // outright whether the modify keeps queue position, so the engine
        // honours the flag instead of re-deriving the rule from the diff.
        out.maintain_priority = (static_cast<unsigned char>(m[1]) & 0x01u) != 0;
        out.ts   = le<std::int64_t>(m + 2);
        std::memcpy(out.symbol.c, m + 10, 8);
        out.symbol.c[8] = '\0';
        out.order_id = le<std::uint64_t>(m + 18);
        out.size     = static_cast<Qty>(le<std::uint32_t>(m + 26));
        out.price    = le<std::int64_t>(m + 30);
        ++messages_;
        return true;
      }
      case 'R': {
        if (len < 26) { ++skipped_; continue; }
        out.type = MsgType::OrderDelete;
        out.ts   = le<std::int64_t>(m + 2);
        std::memcpy(out.symbol.c, m + 10, 8);
        out.symbol.c[8] = '\0';
        out.order_id = le<std::uint64_t>(m + 18);
        out.size  = 0;
        out.price = 0;
        ++messages_;
        return true;
      }
      case 'L': {
        if (len < 38) { ++skipped_; continue; }
        out.type = MsgType::OrderExecuted;
        out.ts   = le<std::int64_t>(m + 2);
        std::memcpy(out.symbol.c, m + 10, 8);
        out.symbol.c[8] = '\0';
        out.order_id = le<std::uint64_t>(m + 18);
        out.size     = static_cast<Qty>(le<std::uint32_t>(m + 26));
        out.price    = le<std::int64_t>(m + 30);
        ++messages_;
        return true;
      }
      default:
        ++skipped_;  // auction, system event, trading status, trade report
        continue;
    }
  }
}

}  // namespace pricetime::iex
