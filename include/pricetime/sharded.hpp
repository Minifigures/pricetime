#pragma once

// Sharded execution: many books across many cores, without a single lock.
//
// The design rule is the single-writer principle, which is the thing LMAX
// built their architecture around and the reason their business logic runs on
// one thread. Locks are not slow because of contention alone: LMAX measured
// 500M increments at 300ms on one thread, 10,000ms with a lock, and 224,000ms
// with two threads contending that lock. Three orders of magnitude, to protect
// an increment.
//
// So no book is ever written by more than one thread. A book belongs to
// exactly one shard, a shard belongs to exactly one thread, and a message is
// routed to its shard by hashing (venue, symbol). Inside a shard the engine is
// the same single-threaded code that the differential fuzz already validated,
// which means threading adds no new matching semantics to get wrong.
//
// The one genuinely concurrent thing is reading. Computing an NBBO means
// reading the top of book of every venue for a symbol, and those books live on
// different threads. That is solved with a seqlock rather than a mutex: the
// writer bumps an odd version, writes, then bumps it even; a reader takes the
// version, reads, and re-reads the version, retrying if it changed or was odd.
// Readers never block writers, which matters because the writer is the hot
// path and the reader is not.
//
// DETERMINISM IS THE ACCEPTANCE CRITERION. Replaying the same input on one
// thread and on N threads must produce byte-identical per-book event streams.
// If it does not, the sharding is wrong, and no throughput number would make
// that acceptable. See tests/test_sharded.cpp.

#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

#include "pricetime/book.hpp"
#include "pricetime/consolidator.hpp"

namespace pricetime {

// A top-of-book snapshot published across threads.
//
// Bbo is 32 bytes, so it cannot ride in a single atomic. A seqlock gives a
// consistent read of the whole struct without the reader ever taking a lock,
// and without the writer ever waiting on a reader.
class BboSlot {
 public:
  void publish(const Bbo& v) noexcept {
    // Odd version means "write in progress".
    const auto v0 = version_.load(std::memory_order_relaxed);
    version_.store(v0 + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    bid_px_.store(v.bid_px, std::memory_order_relaxed);
    bid_sz_.store(v.bid_sz, std::memory_order_relaxed);
    ask_px_.store(v.ask_px, std::memory_order_relaxed);
    ask_sz_.store(v.ask_sz, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    version_.store(v0 + 2, std::memory_order_relaxed);
  }

  [[nodiscard]] Bbo read() const noexcept {
    for (;;) {
      const auto v1 = version_.load(std::memory_order_acquire);
      if ((v1 & 1u) != 0u) continue;           // writer mid-update
      Bbo out;
      out.bid_px = bid_px_.load(std::memory_order_relaxed);
      out.bid_sz = bid_sz_.load(std::memory_order_relaxed);
      out.ask_px = ask_px_.load(std::memory_order_relaxed);
      out.ask_sz = ask_sz_.load(std::memory_order_relaxed);
      std::atomic_thread_fence(std::memory_order_acquire);
      const auto v2 = version_.load(std::memory_order_relaxed);
      if (v1 == v2) return out;                // no write intervened
    }
  }

 private:
  // The payload fields are atomic, and this is not pedantry.
  //
  // The obvious seqlock stores a plain struct and relies on the version
  // counter to make torn reads impossible. On x86 that works, and a test that
  // hammers it with 400,000 concurrent reads will report zero tears -- mine
  // did. It is still undefined behaviour: the C++ memory model says concurrent
  // non-atomic access is a data race regardless of any protocol layered on
  // top, and the compiler is entitled to assume races do not happen.
  //
  // ThreadSanitizer caught exactly this and the plain-struct version is gone.
  // Relaxed atomics make each access well-defined while compiling to the same
  // plain loads and stores on x86; the two fences, not the individual
  // operations, are what order the payload against the version counter.
  std::atomic<std::uint64_t> version_{0};
  std::atomic<Price> bid_px_{kInvalidPrice};
  std::atomic<Qty>   bid_sz_{0};
  std::atomic<Price> ask_px_{kInvalidPrice};
  std::atomic<Qty>   ask_sz_{0};
};

// One shard: a thread, and every book it exclusively owns.
//
// alignas(64) keeps two shards' hot state off the same cache line. Without it
// two threads incrementing adjacent counters ping-pong that line between cores
// and the "parallel" version runs slower than the serial one. This is the
// single most common way a first threading attempt loses.
struct alignas(64) ShardStats {
  std::uint64_t applied = 0;
  std::uint64_t trades  = 0;
  char _pad[64 - 2 * sizeof(std::uint64_t)] = {};
};

// Routing. Venue and symbol ids are small and dense, so a naive combination
// collides in exactly the pattern a power-of-two shard count is worst at.
// splitmix64's finalizer scatters them.
[[nodiscard]] inline std::uint32_t shard_of(VenueId v, SymbolId s,
                                            std::uint32_t shards) noexcept {
  std::uint64_t x = (static_cast<std::uint64_t>(v) << 32) ^ s;
  x ^= x >> 30; x *= 0xBF58476D1CE4E5B9ull;
  x ^= x >> 27; x *= 0x94D049BB133111EBull;
  x ^= x >> 31;
  return static_cast<std::uint32_t>(x % shards);
}

// One inbound instruction, tagged with where it belongs.
struct ShardedMsg {
  VenueId  venue  = 0;
  SymbolId symbol = 0;
  enum class Kind : std::uint8_t { New, Cancel, Replace } kind = Kind::New;
  NewOrder     nw;
  CancelOrder  cx;
  ReplaceOrder rp;
};

// Batch replay across shards.
//
// Messages are partitioned by shard, then each shard's slice is applied in its
// original relative order on its own thread. Because books are disjoint across
// shards, and a book is only ever touched by its owning shard, the per-book
// event stream is identical to what serial execution would produce. That is
// the property the determinism test asserts, and it is the reason this is safe
// without a single lock on the write path.
class ShardedReplay {
 public:
  ShardedReplay(std::uint32_t shards, Price floor_px, Price ceil_px);
  ~ShardedReplay();

  void run(const std::vector<ShardedMsg>& msgs);

  [[nodiscard]] const EventLog* log_for(VenueId v, SymbolId s) const;
  [[nodiscard]] Bbo bbo(VenueId v, SymbolId s) const;
  [[nodiscard]] std::uint64_t applied() const noexcept;
  [[nodiscard]] std::uint64_t trades() const noexcept;
  [[nodiscard]] std::uint32_t shards() const noexcept { return n_shards_; }

 private:
  struct Shard;
  std::uint32_t n_shards_;
  Price floor_, ceil_;
  std::vector<std::unique_ptr<Shard>> shards_;
};

}  // namespace pricetime
