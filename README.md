# pricetime

A limit order book and matching engine in C++20, built to be fast and proven
correct by construction rather than by assertion.

The name is the rule the whole thing turns on: **price-time priority**. Better
prices execute first, and at the same price the order that arrived first
executes first. Everything below is in service of getting that exactly right,
several million times a second.

```
git clone https://github.com/Minifigures/pricetime && cd pricetime
make test     # 27 tests, including 100k-operation differential fuzz
make bench    # latency percentiles and throughput
make replay   # live terminal order book
```

No dependencies. No package manager, no CMake, no GTest, no Google Benchmark.
`g++` and `make` are the whole toolchain, because a reviewer should be able to
verify every claim on this page without installing anything first.

---

## The problem

An exchange matching engine is a piece of software with an unusual property:
it is simple enough to describe in a paragraph and unforgiving enough that
almost every first implementation is subtly wrong in a way that costs someone
money.

The description: participants send orders; orders that cross the spread trade
immediately; orders that do not cross rest in a queue; the queue is ordered by
price, then by arrival. That is it.

The ways it goes wrong are less obvious. Trades print at the wrong side's
price and every taker is quietly overcharged. Prices are stored as `double` and
`0.1 + 0.2` puts a fill at a price that does not exist on the ladder. A
cancel/replace keeps its place in the queue when it should have lost it, so one
participant can hold the front of the line and grow into it. A fill-or-kill
partially fills. The p99.9 latency is forty times the median because the
allocator went to get more memory in the middle of a burst.

`pricetime` is an attempt to get all of that right, and to *demonstrate* it
rather than claim it.

---

## Architecture

```
                      ┌──────────────────────────────────────┐
   NewOrder ─────────▶│                                      │
   CancelOrder ──────▶│            Book (fast path)          │──────▶ EventLog
   ReplaceOrder ─────▶│                                      │        Accepted
                      │  flat price ladder  ◀── O(1) index   │        Rejected
                      │  intrusive lists    ◀── no alloc     │        Trade
                      │  occupancy bitmap   ◀── 64 lvls/scan │        Canceled
                      └──────────────────────────────────────┘        Replaced
                                      ║
                                      ║  identical event streams
                                      ║  asserted after every operation
                                      ▼
                      ┌──────────────────────────────────────┐
                      │       ReferenceBook (the spec)       │
                      │   std::map + std::vector, slow and   │
                      │       obviously correct              │
                      └──────────────────────────────────────┘
```

| Path | File | Role |
|---|---|---|
| Types | `include/pricetime/types.hpp` | Integer ticks, sides, order types, TIF, reject reasons |
| Protocol in | `include/pricetime/order.hpp` | `NewOrder`, `CancelOrder`, `ReplaceOrder` |
| Protocol out | `include/pricetime/events.hpp` | The five events the engine can emit |
| Specification | `src/reference_book.cpp` | Slow, legible, correct |
| Fast path | `src/book.cpp` | Ladder, pool, bitmap |
| Proof | `tests/test_differential.cpp` | Fuzz one against the other |
| Measurement | `bench/bench_main.cpp` | Per-operation percentiles |
| Demo | `src/replay_main.cpp` | Live terminal book |

---

## Design choices that matter

### Prices are integers, always

A `Price` is a count of ticks, never a `double`. Converting to a display
currency happens once, at the edge.

This is not fastidiousness. Binary floating point cannot represent most decimal
prices exactly, so `0.1 + 0.2 != 0.3`. In a matching engine that is not a
rounding curiosity, it is a fill printed at a price that is not on the ladder, a
position that does not reconcile, and a break somebody unwinds by hand the next
morning. Integer ticks make price comparison exact and total, which is precisely
what price-time priority requires. Every production venue does this.

### Trades print at the resting price

An aggressor willing to pay 105 into a book offered at 100 trades at **100**, and
keeps the improvement. The resting order was there first and set the terms.

Printing at the aggressor's limit is the classic off-by-one-side bug, and it is
invisible in testing unless you specifically look for it, because the trade
still happens and the quantities still balance. `trade_prints_at_resting_price_not_aggressor_price`
exists to catch exactly that.

### Time priority comes from a sequence number, not a clock

The book never compares wall-clock timestamps to decide who was first.
Timestamps tie, and under NTP correction they can go backwards. A monotonic
sequence number stamped at acceptance is the only thing that establishes
priority. Timestamps are carried for observability and never consulted for
ordering.

### Cancel/replace priority is asymmetric, on purpose

A pure size *reduction* at the same price keeps its queue position. Any price
change, or any size *increase*, goes to the back of the new level.

The asymmetry is the point. A participant reducing size is only giving something
up, so nobody behind them is harmed. If growing were free, you could hold the
front of the queue indefinitely and quietly inflate the size sitting in front of
everyone else. Both cases are tested.

### A market order is a limit order priced through the book

There is one matching path, not two. A market buy is a limit buy at the ceiling
of the price band. This removes an entire class of bug where the two paths drift
apart and only the less-tested one is wrong.

A market order with a Day time-in-force is **rejected**, not silently converted:
there is no price for it to rest at, and inventing one is worse than refusing.

### The price band is bounded, and says so

`Book` allocates a flat ladder over `[floor, ceil]` and rejects prices outside
it. Real order flow clusters within a few ticks of the touch, so a tree's
O(log n) plus a pointer chase per level buys nothing. The ladder makes level
lookup a single array offset.

This is a real constraint, not a hidden one. Venues do the same thing and call
them price bands. `ReferenceBook` has no band, which is why the differential
fuzz generates in-band prices only and out-of-band rejection is tested
separately.

### Nothing allocates on the hot path

Orders live in one pre-sized pool, threaded into intrusive doubly-linked lists
by **index**, not pointer. After warmup the engine performs no allocation at
all, so there is no allocator lock and no tail-latency spike when it decides to
go get more memory. Indices also survive pool growth, which raw pointers would
not.

### Finding the next level is a word scan

When the best level is consumed, the engine must find the next occupied one.
Walking the ladder is O(band). One bit per level scanned 64 at a time with
`std::countr_zero` makes it O(band/64), with no branch in the inner step.

---

## Correctness

The optimized book is not obviously correct, because it is fast. So it is not
asked to be obviously correct. It is asked to be *identical to something that
is*.

`ReferenceBook` is written to be boring: `std::map` keyed by price, a
`std::vector` per level, copies everywhere, no cleverness. `tests/test_differential.cpp`
drives randomized order flow through both and asserts, **after every single
operation**, that:

1. the emitted event streams are identical, including sequence numbers, trade
   prices, contra ids, and reject reasons, and
2. the resulting book state is identical: best bid, best ask, resting count,
   and eight levels of depth on each side.

Not equivalent. Not close. Identical. When they disagree the test prints both
streams side by side with the divergence marked, so the failure is a diff rather
than an index.

Current coverage: **100,000 randomized operations** across three self-trade
policies and 26 seeded campaigns, plus 23 hand-written behavioural tests for the
rules above.

Two details worth calling out, both borrowed from better engineers than me:

- **The generator is derived from the enums, not hand-written.** Adding an
  `OrderType` or `TimeInForce` automatically widens the fuzz. A separate test,
  `all_combinations_exercised`, asserts every legal combination was actually
  produced, so a generator bug narrows coverage loudly instead of silently.
- **The RNG is a hand-rolled xorshift64\*, not `std::mt19937`.** `mt19937` is
  reproducible but the standard library's *distributions* are not specified, so
  a fuzz corpus built on them changes when you upgrade your toolchain. A
  regression test that quietly tests something different after an upgrade is not
  a regression test.

```
make asan    # same suite under AddressSanitizer + UBSan, leak detection on
```

---

## Measured performance

```
12th Gen Intel Core i5-12600KF · g++ 13.3.0 · -O3 -march=native
2,000,000 operations after 200,000 warmup · 55% submit / 45% cancel
prices clustered within 12 ticks of mid · price band 2,001 ticks
```

| engine | p50 | p90 | p99 | p99.9 | mean | ops/sec |
|---|---|---|---|---|---|---|
| **`Book`** | **66 ns** | **135 ns** | **266 ns** | **530 ns** | **82 ns** | **9.83 M** |
| `ReferenceBook` | 80 ns | 391 ns | 2,942 ns | 5,408 ns | 246 ns | 3.77 M |

**The median is only 1.2x better. The tail is 10x better.** That is the whole
result, and it is the one that matters: in a matching engine nobody is harmed by
a median, they are harmed by the one message in a thousand that arrived during a
burst and took five microseconds. Removing allocation from the hot path and
replacing the level walk with a bitmap scan barely moves p50 and collapses
p99.9.

Methodology, because a benchmark you cannot audit is a marketing number:

- Every operation is timed individually. p99.9 is a real percentile over all
  two million samples, not an average of batch averages.
- Paired `steady_clock` reads cost a median of **14 ns** on this machine. That
  overhead is measured, reported by the harness, and **deliberately not
  subtracted**, so the published numbers understate the engine. True p50 is
  nearer 52 ns.
- Order flow is generated into a vector before the measured region, so the RNG
  is not inside the timing.
- The reference engine runs the identical flow. This is a like-for-like
  comparison, not a comparison against a strawman.

Reproduce with `make bench`.

---

## What this is not

Stated plainly, because the gaps are more interesting than the features.

- **Single symbol, single thread.** There is no cross-instrument state and no
  concurrency. A real venue shards by symbol and this would be one shard. The
  sequencer, replication, and failover that make that safe are not here.
- **No wire protocol.** No FIX, no ITCH/OUCH, no SBE. Orders arrive as structs.
  Parsing and normalizing a real feed is a meaningful chunk of work that this
  does not attempt.
- **No persistence or recovery.** The event stream is the natural journal and
  the engine is deterministic, so replay-based recovery is the obvious next
  step. It is not implemented.
- **Benchmarked on WSL2, not bare metal.** No CPU pinning, no isolated cores, no
  huge pages, no `SCHED_FIFO`. The tail would improve on tuned hardware. The
  numbers above are honest for a laptop-class dev box and should not be compared
  against a colocated production engine.
- **The order index is `std::unordered_map`.** It is the least optimized thing
  in the hot path and the obvious next target, since it is the one remaining
  place a cancel can miss cache. Replacing it with open addressing is the next
  commit I would write.
- **Self-trade prevention is per-order-owner only.** Real venues prevent at the
  firm, MPID, and group level with configurable precedence.

---

## Build

| Command | What it does |
|---|---|
| `make` | Build tests, benchmark, and replay into `build/` |
| `make test` | Run the correctness and differential suites |
| `make bench` | Run the latency and throughput harness |
| `make asan` | Rebuild under AddressSanitizer and UBSan, run the suite |
| `make replay` | Live terminal order book |
| `make clean` | Remove `build/` |

Requires g++ 13+ (C++20) and make. Built and tested on Linux; the warning set is
`-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion -Wold-style-cast`
and the tree is clean under all of them.

## License

MIT. See [LICENSE](LICENSE).
