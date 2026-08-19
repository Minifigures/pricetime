#include "pricetime/iex.hpp"

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

bool read_exact(std::FILE* f, char* dst, std::size_t n) {
  return std::fread(dst, 1, n, f) == n;
}

}  // namespace

bool DeepPlusReader::open(const std::string& path) {
  close();
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
  char gh[kPcapGlobalHeader];
  if (!read_exact(pipe_, gh, kPcapGlobalHeader)) {
    error_ = "short read on pcap global header (is the file gzipped pcap?)";
    close();
    return false;
  }
  const auto magic = le<std::uint32_t>(gh);
  if (magic != kPcapMagicLE) {
    char buf[96];
    std::snprintf(buf, sizeof(buf),
                  "unexpected pcap magic 0x%08X (expected 0x%08X, classic LE)",
                  magic, kPcapMagicLE);
    error_ = buf;
    close();
    return false;
  }
  return true;
}

void DeepPlusReader::close() {
  if (pipe_ != nullptr) {
    ::pclose(pipe_);
    pipe_ = nullptr;
  }
  pkt_.clear();
  pkt_pos_ = 0;
  remaining_in_packet_ = 0;
}

bool DeepPlusReader::fill_packet() {
  for (;;) {
    char rh[kPcapRecordHeader];
    if (!read_exact(pipe_, rh, kPcapRecordHeader)) return false;
    const auto incl = le<std::uint32_t>(rh + 8);
    if (incl == 0 || incl > (1u << 20)) return false;  // corrupt or absurd

    std::vector<char> frame(incl);
    if (!read_exact(pipe_, frame.data(), incl)) return false;
    ++packets_;

    if (incl < kEthHeader + 20 + kUdpHeader + kIexTpHeader) continue;
    if (be16(frame.data() + 12) != 0x0800) continue;  // not IPv4

    const auto ihl = static_cast<std::size_t>(
        (static_cast<unsigned char>(frame[kEthHeader]) & 0x0F) * 4);
    const std::size_t udp_body = kEthHeader + ihl + kUdpHeader;
    if (udp_body + kIexTpHeader > incl) continue;

    const char* tp = frame.data() + udp_body;
    if (le<std::uint16_t>(tp + 2) != kProtocolDeepPlus) continue;

    const auto msg_count = le<std::uint16_t>(tp + 16);
    if (msg_count == 0) continue;

    const std::size_t block = static_cast<std::size_t>(incl) - udp_body -
                              kIexTpHeader;
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
