#pragma once

// Decoder for the IEX DEEP+ (DPLS) historical feed.
//
// DEEP+ is order-by-order (L3/MBO) market data: every displayed order that is
// added, modified, deleted, or executed on the IEX Order Book, each carrying a
// real exchange-assigned Order ID. That is the only kind of feed a matching
// engine can actually be driven by, and IEX is, as far as this project could
// establish, the only US equities venue that publishes it free AND permits
// redistribution.
//
//   Data provided for free by IEX. By accessing or using IEX Historical Data,
//   you agree to the IEX Historical Data Terms of Use.
//   https://iextrading.com/iex-historical-data-terms/
//
// Two capture container formats appear in the archive and both are supported.
// IEX's early DEEP+ files are classic libpcap (magic 0xA1B2C3D4); the current
// ones are pcapng (magic 0x0A0D0D0A). Assuming either one alone fails loudly on
// the first byte, which is at least an honest failure, but supporting both is
// twenty lines and the archive genuinely contains both.
//
// Layering, outermost first:
//   capture container  (classic pcap OR pcapng)
//     Ethernet II  ->  IPv4  ->  UDP
//       IEX-TP v1 header (40 bytes, little-endian, protocol 0x8005 = DEEP+)
//         N x [ uint16 length | message payload ]
//
// Every multi-byte field is LITTLE-endian. This is worth stating because the
// other feed everyone reaches for, Nasdaq ITCH, is big-endian with a 2-byte
// length prefix, and mixing the two up produces prices in the trillions rather
// than an obvious crash.
//
// Field offsets below are transcribed from IEX DEEP+ Specification v1.04.

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "pricetime/types.hpp"

namespace pricetime::iex {

inline constexpr std::uint16_t kProtocolDeepPlus = 0x8005;

// IEX prices are fixed-point with 4 implied decimals, so $99.05 is 990500.
// pricetime consumes them as ticks directly: one tick is $0.0001, which is
// the real sub-penny increment IEX quotes at, so no precision is discarded.
inline constexpr std::int64_t kPriceScale = 10'000;

enum class MsgType : char {
  AddOrder      = 'a',  // 0x61, 38 bytes
  OrderModify   = 'M',  // 0x4D, 38 bytes
  OrderDelete   = 'R',  // 0x52, 26 bytes
  OrderExecuted = 'L',  // 0x4C, 38 bytes
  TradeReport   = 'T',  // 0x54
};

struct Symbol {
  char c[9] = {};  // 8 bytes, space-padded, NUL-terminated here
  [[nodiscard]] std::string str() const {
    std::string s(c);
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
  }
  [[nodiscard]] bool operator==(const Symbol& o) const noexcept {
    return __builtin_memcmp(c, o.c, 8) == 0;
  }
};

struct Decoded {
  MsgType       type   = MsgType::AddOrder;
  Nanos         ts     = 0;
  Symbol        symbol;
  std::uint64_t order_id = 0;
  Side          side   = Side::Buy;   // AddOrder only
  Qty           size   = 0;           // Add / Modify / Executed
  Price         price  = 0;           // Add / Modify / Executed
  bool          maintain_priority = false;  // Modify only, flags bit 0
};

// Streams a gzipped pcap through `gunzip -c`, so the build stays free of zlib
// and of every other dependency. The cost is a fork and a pipe; the benefit is
// that `make` still works on a machine with nothing installed.
class DeepPlusReader {
 public:
  ~DeepPlusReader() { close(); }

  bool open(const std::string& path);
  void close();

  // Reads the next DEEP+ message of interest. Returns false at end of stream.
  // Messages this decoder does not model (auction, system, trading status)
  // are skipped internally rather than surfaced.
  bool next(Decoded& out);

  [[nodiscard]] std::uint64_t packets()  const noexcept { return packets_; }
  [[nodiscard]] std::uint64_t messages() const noexcept { return messages_; }
  [[nodiscard]] std::uint64_t skipped()  const noexcept { return skipped_; }
  [[nodiscard]] const std::string& error() const noexcept { return error_; }

 private:
  enum class Container { Pcap, PcapNg };

  bool fill_packet();
  bool next_frame_pcap(std::vector<char>& frame);
  bool next_frame_pcapng(std::vector<char>& frame);

  Container   container_ = Container::Pcap;
  std::FILE*  pipe_ = nullptr;
  std::string error_;

  std::string pkt_;        // current packet payload (UDP body)
  std::size_t pkt_pos_ = 0;
  std::uint16_t remaining_in_packet_ = 0;

  std::uint64_t packets_  = 0;
  std::uint64_t messages_ = 0;
  std::uint64_t skipped_  = 0;
};

}  // namespace pricetime::iex
