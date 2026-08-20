// The DEEP+ decoder is the only component here that parses untrusted input:
// binary capture files, where every length, count and offset is a number taken
// from the file. It had no tests at all, which is backwards. These build real
// captures byte by byte, gzip them, and read them back.
//
// The first test is a regression for a specific bug. Message Count was being
// read from IEX-TP offset 16, which is the low half of Stream Offset, not the
// count. It looked fine because the decode loop is also bounded by the block
// size, so a garbage count usually acts as a harmless large ceiling. It is not
// harmless when the low 16 bits of the stream offset happen to be zero (the
// whole packet is dropped) or smaller than the true count (the tail is
// dropped). Both cases are pinned below.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "harness.hpp"
#include "pricetime/iex.hpp"

using namespace pricetime;

namespace {

void put16(std::vector<unsigned char>& v, std::uint16_t x) {
  v.push_back(static_cast<unsigned char>(x & 0xFF));
  v.push_back(static_cast<unsigned char>(x >> 8));
}
void put32(std::vector<unsigned char>& v, std::uint32_t x) {
  for (int i = 0; i < 4; ++i)
    v.push_back(static_cast<unsigned char>((x >> (8 * i)) & 0xFF));
}
void put64(std::vector<unsigned char>& v, std::uint64_t x) {
  for (int i = 0; i < 8; ++i)
    v.push_back(static_cast<unsigned char>((x >> (8 * i)) & 0xFF));
}

// One DEEP+ Add Order, 38 bytes. Layout per v1.04: type, flags, timestamp,
// symbol, order id, size, price.
std::vector<unsigned char> add_order(std::uint64_t id, std::int64_t px,
                                     std::uint32_t size, bool buy,
                                     const char* sym) {
  std::vector<unsigned char> m;
  m.push_back('a');
  m.push_back(buy ? '8' : '5');
  put64(m, 0);                                    // timestamp
  const std::string tag(sym);                     // symbol, space padded to 8
  for (std::size_t i = 0; i < 8; ++i)
    m.push_back(static_cast<unsigned char>(i < tag.size() ? tag[i] : ' '));
  put64(m, id);
  put32(m, size);
  put64(m, static_cast<std::uint64_t>(px));
  return m;  // 1+1+8+8+8+4+8 = 38
}

// Wrap messages in IEX-TP, UDP, IPv4, Ethernet, then a pcap record.
// count_field and stream_offset are separate parameters on purpose: telling
// them apart is exactly what the regression test needs.
std::vector<unsigned char> packet(const std::vector<std::vector<unsigned char>>& msgs,
                                  std::uint16_t count_field,
                                  std::uint64_t stream_offset) {
  std::vector<unsigned char> block;
  for (const auto& m : msgs) {
    put16(block, static_cast<std::uint16_t>(m.size()));
    block.insert(block.end(), m.begin(), m.end());
  }

  std::vector<unsigned char> tp;
  tp.push_back(1);                                    // version
  tp.push_back(0);                                    // reserved
  put16(tp, 0x8005);                                  // protocol: DEEP+
  put32(tp, 1);                                       // channel
  put32(tp, 42);                                      // session
  put16(tp, static_cast<std::uint16_t>(block.size()));  // payload length @12
  put16(tp, count_field);                             // message count   @14
  put64(tp, stream_offset);                           // stream offset   @16
  put64(tp, 1);                                       // first sequence  @24
  put64(tp, 0);                                       // send time       @32
  tp.insert(tp.end(), block.begin(), block.end());

  std::vector<unsigned char> frame(14, 0);
  frame[12] = 0x08; frame[13] = 0x00;                 // ethertype IPv4
  std::vector<unsigned char> ip(20, 0);
  ip[0] = 0x45;                                       // version 4, IHL 5
  ip[9] = 17;                                         // UDP
  frame.insert(frame.end(), ip.begin(), ip.end());
  std::vector<unsigned char> udp(8, 0);
  frame.insert(frame.end(), udp.begin(), udp.end());
  frame.insert(frame.end(), tp.begin(), tp.end());

  std::vector<unsigned char> rec;
  put32(rec, 0); put32(rec, 0);                       // ts sec, usec
  put32(rec, static_cast<std::uint32_t>(frame.size()));  // incl
  put32(rec, static_cast<std::uint32_t>(frame.size()));  // orig
  rec.insert(rec.end(), frame.begin(), frame.end());
  return rec;
}

std::vector<unsigned char> pcap_header() {
  std::vector<unsigned char> h;
  put32(h, 0xA1B2C3D4u);
  put16(h, 2); put16(h, 4);
  put32(h, 0); put32(h, 0);
  put32(h, 65535);
  put32(h, 1);                                        // LINKTYPE_ETHERNET
  return h;
}

std::string tmp_path(const char* tag) {
  return std::string("/tmp/pricetime_iex_test_") + tag + ".pcap.gz";
}

// Write bytes and gzip them, since the reader always goes through gunzip.
// truncate_to > 0 cuts the *uncompressed* stream first, which is what a
// capture interrupted mid-record looks like.
bool write_gz(const std::string& path, const std::vector<unsigned char>& bytes,
              std::size_t truncate_to = 0) {
  const std::string raw = path + ".raw";
  std::FILE* f = std::fopen(raw.c_str(), "wb");
  if (f == nullptr) return false;
  const std::size_t n = truncate_to ? std::min(truncate_to, bytes.size())
                                    : bytes.size();
  if (n > 0) std::fwrite(bytes.data(), 1, n, f);
  std::fclose(f);
  const std::string cmd = "gzip -c '" + raw + "' > '" + path + "' 2>/dev/null";
  const int rc = std::system(cmd.c_str());
  std::remove(raw.c_str());
  return rc == 0;
}

std::size_t decode_count(const std::string& path, bool* truncated = nullptr) {
  iex::DeepPlusReader rd;
  if (!rd.open(path)) { if (truncated) *truncated = true; return 0; }
  iex::Decoded d;
  std::size_t n = 0;
  while (rd.next(d)) ++n;
  if (truncated != nullptr) *truncated = rd.truncated();
  return n;
}

std::vector<unsigned char> three_message_capture(std::uint16_t count_field,
                                                 std::uint64_t stream_offset) {
  std::vector<std::vector<unsigned char>> msgs{
      add_order(1, 9900, 100, true,  "AAPL"),
      add_order(2, 9901, 200, false, "AAPL"),
      add_order(3, 9902, 300, true,  "AAPL"),
  };
  auto out = pcap_header();
  const auto p = packet(msgs, count_field, stream_offset);
  out.insert(out.end(), p.begin(), p.end());
  return out;
}

}  // namespace

// The bug: with the count read from offset 16, a stream offset whose low 16
// bits are zero made the decoder drop the entire packet.
TEST(iex_message_count_is_read_from_its_own_field) {
  const auto path = tmp_path("count");
  CHECK(write_gz(path, three_message_capture(3, 0)));
  CHECK_EQ(decode_count(path), 3u);
  std::remove(path.c_str());
}

// Same capture, same true count, several unrelated stream offsets. The decode
// must not depend on the stream offset at all.
TEST(iex_decode_is_independent_of_stream_offset) {
  for (std::uint64_t off : {0ull, 1ull, 7ull, 65536ull, 4294967296ull}) {
    const auto path = tmp_path("off");
    CHECK(write_gz(path, three_message_capture(3, off)));
    CHECK_EQ(decode_count(path), 3u);
    std::remove(path.c_str());
  }
}

// And the count field must actually be honoured: a packet claiming one
// message yields one, not the three that are physically present.
TEST(iex_honours_a_short_message_count) {
  const auto path = tmp_path("short");
  CHECK(write_gz(path, three_message_capture(1, 999)));
  CHECK_EQ(decode_count(path), 1u);
  std::remove(path.c_str());
}

TEST(iex_decodes_add_order_fields) {
  const auto path = tmp_path("fields");
  std::vector<std::vector<unsigned char>> msgs{
      add_order(77, 12345, 250, true, "MSFT")};
  auto bytes = pcap_header();
  const auto p = packet(msgs, 1, 0);
  bytes.insert(bytes.end(), p.begin(), p.end());
  CHECK(write_gz(path, bytes));

  iex::DeepPlusReader rd;
  CHECK(rd.open(path));
  iex::Decoded d;
  CHECK(rd.next(d));
  CHECK(d.type == iex::MsgType::AddOrder);
  CHECK_EQ(d.order_id, 77ull);
  CHECK_EQ(d.price, static_cast<Price>(12345));
  CHECK_EQ(d.size, static_cast<Qty>(250));
  CHECK(d.side == Side::Buy);
  CHECK(std::string(d.symbol.str()) == "MSFT");
  CHECK(!rd.truncated());
  std::remove(path.c_str());
}

// A capture cut mid-record must be reported, not silently treated as the end.
// This is the failure mode that let a half-read file produce a confident
// match percentage over an unknown fraction of a session.
TEST(iex_reports_a_capture_cut_mid_record) {
  const auto full = three_message_capture(3, 7);
  const auto path = tmp_path("trunc");
  // Keep the pcap header and part of the record, then stop.
  CHECK(write_gz(path, full, 24 + 30));
  bool truncated = false;
  decode_count(path, &truncated);
  CHECK(truncated);
  std::remove(path.c_str());
}

TEST(iex_clean_end_of_file_is_not_reported_as_truncation) {
  const auto path = tmp_path("clean");
  CHECK(write_gz(path, three_message_capture(3, 7)));
  bool truncated = true;
  CHECK_EQ(decode_count(path, &truncated), 3u);
  CHECK(!truncated);
  std::remove(path.c_str());
}

// A header-only capture is legal and empty, not an error.
TEST(iex_header_only_capture_decodes_nothing_cleanly) {
  const auto path = tmp_path("empty");
  CHECK(write_gz(path, pcap_header()));
  bool truncated = true;
  CHECK_EQ(decode_count(path, &truncated), 0u);
  CHECK(!truncated);
  std::remove(path.c_str());
}

// A message declaring zero length must not stall the decoder. The counter is
// decremented before the length is examined, which is what makes this
// terminate; the test exists to keep that ordering.
TEST(iex_zero_length_messages_terminate) {
  std::vector<unsigned char> block;
  for (int i = 0; i < 50; ++i) put16(block, 0);
  const auto real = add_order(9, 9950, 10, true, "AAPL");
  put16(block, static_cast<std::uint16_t>(real.size()));
  block.insert(block.end(), real.begin(), real.end());

  std::vector<unsigned char> tp;
  tp.push_back(1); tp.push_back(0);
  put16(tp, 0x8005); put32(tp, 1); put32(tp, 42);
  put16(tp, static_cast<std::uint16_t>(block.size()));
  put16(tp, 65535);                       // absurd count, must not overrun
  put64(tp, 0); put64(tp, 1); put64(tp, 0);
  tp.insert(tp.end(), block.begin(), block.end());

  std::vector<unsigned char> frame(14, 0);
  frame[12] = 0x08; frame[13] = 0x00;
  std::vector<unsigned char> ip(20, 0); ip[0] = 0x45; ip[9] = 17;
  frame.insert(frame.end(), ip.begin(), ip.end());
  frame.insert(frame.end(), 8, 0);
  frame.insert(frame.end(), tp.begin(), tp.end());

  auto bytes = pcap_header();
  put32(bytes, 0); put32(bytes, 0);
  put32(bytes, static_cast<std::uint32_t>(frame.size()));
  put32(bytes, static_cast<std::uint32_t>(frame.size()));
  bytes.insert(bytes.end(), frame.begin(), frame.end());

  const auto path = tmp_path("zerolen");
  CHECK(write_gz(path, bytes));
  CHECK_EQ(decode_count(path), 1u);       // terminates, finds the real one
  std::remove(path.c_str());
}

TEST(iex_rejects_a_file_that_is_not_a_capture) {
  const auto path = tmp_path("garbage");
  std::vector<unsigned char> junk(64, 0xAB);
  CHECK(write_gz(path, junk));
  iex::DeepPlusReader rd;
  CHECK(!rd.open(path));
  CHECK(!rd.error().empty());
  std::remove(path.c_str());
}
