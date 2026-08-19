# pricetime

A multi-venue limit order book and matching engine in C++20, built to be fast
and *proven* correct rather than asserted correct.

The name is the rule the whole thing turns on: **price-time priority**. Better
prices execute first; at the same price, whoever arrived first executes first.

```
git clone https://github.com/Minifigures/pricetime && cd pricetime
make test      # 38 tests, including 100k-op differential fuzz
make bench     # latency percentiles across four flow regimes
make replay    # live terminal order book
./scripts/fetch_iex.sh && ./build/pricetime_iex data/iex/20241223_DPLS.pcap.gz AAPL
```

**Zero dependencies.** No CMake, no GTest, no Google Benchmark, no package
manager. `g++` and `make`, because every number on this page should be
checkable without installing anything first.

---

## Read this before the benchmarks

There are four different things people mean by "latency" in this domain, and
conflating them is the classic mistake:

| | measures | published example |
|---|---|---|
| **(a)** | customer round trip from a colocated cabinet | Kraken ~200 µs |
| **(b)** | exchange gateway to gateway | LSE 126 µs avg, 99% within 210 µs |
| **(c)** | matching engine internal processing only | **LMAX production: <50 µs** |
| **(d)** | in-memory data structure, no I/O at all | **LMAX Disruptor: 52 ns** |

LMAX's Disruptor benchmarks at 52 ns. LMAX's actual exchange, *running that
same Disruptor*, publishes **under 50 microseconds** of internal matching
latency and peaks at 800,000 orders/sec. Three orders of magnitude apart,
because the real one also does network I/O, pre-trade risk, persistence and
replication.

**Every number in this README is category (d).** This engine does no I/O, no
risk checks, no persistence, no replication, and no sequencing across
processes. It is the data structure and the matching logic, measured in
isolation on a laptop-class CPU under WSL2.

No production venue publishes a sub-microsecond matching figure. If you read a
nanosecond number here and compare it to an exchange's published latency, you
are comparing two different things, and I would rather say that myself than
have you notice it.

---

## The problem

A matching engine is simple enough to describe in a paragraph and unforgiving
enough that almost every first implementation is subtly wrong in a way that
costs someone money.

The description: orders that cross the spread trade immediately; orders that do
not, rest in a queue ordered by price, then arrival. That is it.

The ways it goes wrong are the interesting part, and they are not hypothetical:

- **Nasdaq, Facebook IPO, 18 May 2012.** The IPO cross validated that no order
  used in the calculation had been cancelled mid-calculation. Under load the
  calculation took 20 ms instead of 1-2 ms, so a cancel always arrived during
  it, and **by design each recalculation absorbed only one additional
  cancellation** — so it could never converge. Infinite loop. The failover went
  to an engine with the validation removed, frozen at 11:11am, and 38,000+
  orders were excluded in violation of Nasdaq's own price/time priority rule.
  $10M penalty (SEC Rel. 34-69655).
- **NYSE, 24 January 2023, fined $9M on 6 March 2026.** Primary and backup
  trading systems ran simultaneously; the primary concluded that 2,800+ opening
  auctions had already happened, so they never ran. There was **no policy
  requiring anyone to check that the auctions had actually occurred.** A
  split-brain, punished as an observability gap (Reg SCI 1001(a)(2)(vii)).
- **Knight Capital, 1 August 2012.** Dormant code left callable, its activation
  flag repurposed, deployed to 7 of 8 servers. **$460M in 45 minutes**
  (SEC Rel. 34-70694).

The recurring theme is not that matching is algorithmically hard. It is that
the invariants are subtle, the failure modes are silent, and the recovery path
is usually less tested than the thing that failed.

---

## Architecture

```
   NewOrder ────┐
   CancelOrder ─┼──▶ Book (per venue, per symbol) ──▶ EventLog
   ReplaceOrder ┘      flat ladder   O(1) level              Accepted
                       intrusive lists   no alloc            Rejected
                       occupancy bitmap  64 lvls/scan        Trade
                              │                              Rested (+queue pos)
                              │                              Canceled
                              ▼                              Replaced
                       ┌─────────────┐
                       │ Consolidator│──▶ get_exchange_bbo(venue, symbol)
                       │  N venues   │──▶ get_nbbo(symbol)
                       └─────────────┘──▶ locked / crossed / trade-through
                              ║
                              ║ every result cross-checked against
                              ▼
                       ReferenceBook  (std::map, obviously correct, slow)
```

| Path | File |
|---|---|
| Types, integer ticks | `include/pricetime/types.hpp` |
| Inbound protocol | `include/pricetime/order.hpp` |
| Outbound events | `include/pricetime/events.hpp` |
| **Specification** | `src/reference_book.cpp` |
| **Fast path** | `src/book.cpp` |
| **Cross-venue** | `src/consolidator.cpp` |
| Real market data | `src/iex.cpp` |
| Proof | `tests/test_differential.cpp`, `tests/test_consolidator.cpp` |
| Measurement | `bench/bench_main.cpp` |

---

## Design choices, and why

### Prices are integers, always

A `Price` is a count of ticks, never a `double`. Binary floating point cannot
represent most decimal prices exactly, so `0.1 + 0.2 != 0.3`. In a matching
engine that is not a rounding curiosity, it is a fill printed at a price that is
not on the ladder and a break somebody unwinds by hand the next morning.

### Trades print at the resting price

An aggressor bidding 105 into a book offered at 100 trades at **100** and keeps
the improvement. The resting order was there first and set the terms.

This is not a preference. Coinbase's own matching-engine documentation states
the same rule: matches occur *"at the price of the order that was on the book,
not at the price of the taker order."* Printing at the aggressor's limit is the
classic off-by-one-side bug and is invisible unless you look for it, because the
trade still happens and the quantities still balance.

### Time priority comes from a sequence number, not a clock

Timestamps tie, and under NTP correction they can go backwards. A monotonic
sequence stamped at acceptance is the only thing establishing priority.
Timestamps are carried for observability and never consulted for ordering.

### Cancel/replace priority is asymmetric

Reducing size at the same price keeps queue position. Any price change, or any
size *increase*, goes to the back.

This matches CME's published rule verbatim: *"an order loses order priority and
is re-queued when changed in any of the following ways: Increase the quantity,
Change the price, Change the account number."* Both cases are tested.
(**Gap:** we do not model accounts, so the third condition is unimplemented.)

### The engine publishes queue position

`Rested` events carry `queue_ahead`: the total size already queued in front of
you at your price, at the moment you rested.

Most toy engines omit this, and it makes them unusable. Moallemi and Yuan
(Columbia GSB, rev. June 2017) find that price-time priority *"creates a
technological arms race... to establish early (and hence advantageous)
positions in the resulting FIFO queue,"* that adverse selection costs increase
with queue position, and that **for some large-tick stocks queue value is of the
same order of magnitude as the bid-ask spread.** A market maker who cannot see
how much size is ahead cannot compute a fill probability, and every strategy
degenerates into pennying the touch.

### A market order is a limit order priced through the book

One matching path, not two. A market buy is a limit buy at the band ceiling.
A market order with a Day time-in-force is **rejected**, not silently converted:
there is no price for it to rest at, and inventing one is worse than refusing.

### Self-trade prevention, and why it exists

Three policies: `None`, `CancelResting`, `CancelAggressor`.

This is not decoration. **The CFTC fined Coinbase $6.5M in March 2021** because
two of its own automated programs, Hedger and Replicator, *"matched orders with
one another,"* inflating apparent volume — and that tainted data fed the CME
Bitcoin Real Time Index, CoinMarketCap and the NYSE Bitcoin Index. Not fraud;
two well-intentioned internal algorithms crossing each other.

**Gap:** MIAX Rule 2614(f) and Coinbase both implement four modes (cancel
newest, cancel oldest, decrement and cancel, cancel both). We implement three,
and no decrement mode.

---

## Correctness

The fast book is not obviously correct, because it is fast. So it is not asked
to be. It is asked to be **identical to something that is**.

`ReferenceBook` uses `std::map`, a vector per level, copies freely, optimizes
nothing. `tests/test_differential.cpp` drives randomized order flow through both
and asserts, **after every single operation**, that the emitted event streams are
identical (sequence numbers, trade prices, contra ids, reject reasons) *and* that
the resulting book state is identical (best bid, best ask, resting count, eight
levels of depth per side).

**Coverage: 100,000 randomized operations** across three self-trade policies and
26 seeded campaigns, plus 23 hand-written behavioural tests and 11 consolidation
tests. The NBBO is separately fuzzed across four venues against an independent
naive walk of every level of every venue.

Two details borrowed from better engineers:

- **The generator is derived from the enums, not hand-written.** Adding an
  `OrderType` widens the fuzz automatically, and `all_combinations_exercised`
  asserts every legal combination was actually produced.
- **The RNG is a hand-rolled xorshift64\*, not `std::mt19937`.** mt19937 is
  reproducible but the standard library's *distributions* are not specified, so
  a corpus built on them silently changes when you upgrade your toolchain.

This is not theoretical. Rewriting best-price tracking (a delicate change to the
hot path) and adding queue position to both engines were both caught-or-cleared
by this suite within seconds.

```
make asan     # same suite under AddressSanitizer + UBSan, leak detection on
```

---

## Measured performance

```
12th Gen Intel Core i5-12600KF (20 MB L3) · g++ 13.3 · -O3 -march=native
WSL2, no core pinning, no isolation, no huge pages
1,000,000 ops per regime after 100,000 warmup · category (d), see top of README
```

| regime | p50 | p90 | p99 | p99.9 | ops/sec |
|---|---|---|---|---|---|
| tight (deep narrow book) | 66 ns | 227 ns | 469 ns | **816 ns** | 7.5 M |
| wide (sparse levels) | 64 ns | 166 ns | 381 ns | 684 ns | 8.7 M |
| sweep (large marketable) | 40 ns | 75 ns | 377 ns | 705 ns | 13.2 M |
| cancel storm (90% cancels) | 37 ns | 42 ns | 61 ns | 194 ns | 18.3 M |

Four deliberately different regimes, **worst case published rather than best**.
Timer overhead is 14 ns and is *included*, not subtracted, so the true figures
are lower than printed.

**A hypothesis I got wrong, kept because it is informative.** I predicted the
cancel-heavy regime would be the adversarial case, since it leans hardest on the
unoptimized `std::unordered_map` order index. It is the *fastest* regime by
4x. The worst case is the deep narrow book, where more orders per level mean
cancels walk longer lists. The engine's weak spot was the opposite of my guess.

### The cache cliff

Replaying real IEX data instead of synthetic flow, p50 went from 66 ns to
**1,986 ns**. The cause is not algorithmic. Clamping the ladder's price band
while replaying the same real AAPL order flow:

| band | ladder | p50 | p99.9 | messages dropped |
|---|---|---|---|---|
| 4,097 ticks | 0.2 MB | **17 ns** | 27 ns | 54% |
| 32,769 | 1.5 MB | 21 ns | 41 ns | 54% |
| **262,145** | **12.0 MB** | **47 ns** | 313 ns | **130 of 405,038 (0.03%)** |
| 605,001 (full day) | 27.7 MB | 1,986 ns | 13,287 ns | 0% |

Compare the last two rows: **dropping 0.03% of messages moves p50 by 42x**. That
is not a curve, it is a cliff, and it sits exactly where the ladder stops fitting
in this CPU's 20 MB L3 cache. A 12 MB ladder is resident; a 27.7 MB one is not.

So the flat ladder is not "O(1) and fast." It is O(1) with a constant that
depends entirely on whether the working set is cache-resident. Real venues
enforce price bands and reject orders outside them; this is the performance
reason why, quantified.

**This is a known unfixed gap.** The right answer is a two-tier book: a hot
ladder sized to L3 plus a cold map for outliers. It is not implemented.

---

## Real market data

`make iex` replays the IEX DEEP+ historical feed: every displayed order added,
modified, deleted and executed on the Investors Exchange, each with a real
exchange-assigned Order ID.

Decoder path: gzip → classic pcap **or** pcapng → Ethernet → IPv4 → UDP →
IEX-TP (protocol `0x8005`) → length-prefixed messages. Both container formats
appear in the archive. Everything is little-endian, which is worth stating
because Nasdaq ITCH — the feed everyone reaches for first — is big-endian with a
2-byte length prefix, and mixing them up yields prices in the trillions.

Verified against a full trading day, **2024-12-23**:

```
  packets         : 42,492,551
  order messages  : 50,163,616 decoded, 793,630 skipped (auction/system)
  decode rate     : 1.82 M msg/sec
  symbols seen    : 8,159
```

### Validating against the venue's own executions

DEEP+ publishes the *effect* of an execution but never the aggressing order. So
a feed replay cannot exercise a matching path at all on its own: the venue's
book is never crossed, so nothing ever matches.

Each Order Executed message is therefore turned back into the aggressor that
must have caused it — an IOC on the opposite side, at the reported price, for
the reported size, from a different participant. That drives the real match path
*and* yields a correctness check against reality.

**AAPL, 2024-12-23: 1,499 executions replayed, 1,499 matched the exact order ID
IEX reported. 100.0%.** Zero size mismatches, zero unknown orders.

### Why IEX and not something better

Nasdaq's ITCH is better data. Its terms permit only *"one unaltered permanent
copy... for personal and non-commercial use."*

**LOBSTER is a trap.** Its sample files are still freely downloadable, but its
terms were rewritten on **2026-08-14** and clause 5.1(b) now prohibits
publishing *"results, tables, figures, statistics, screenshots, or findings
derived from the Data"* without a paid licence, with 5.1(g) barring use to
**benchmark**. It was my default choice until I read them.

IEX is the only free US-equities source whose terms explicitly permit
redistribution. See `data/README.md` for every source evaluated and rejected.

> Data provided for free by IEX. By accessing or using IEX Historical Data, you
> agree to the [IEX Historical Data Terms of Use](https://iextrading.com/iex-historical-data-terms/).

---

## Multi-venue consolidation

```cpp
Bbo  get_exchange_bbo(VenueId, SymbolId) const;   // top of book at one venue
Nbbo get_nbbo(SymbolId) const;                    // best bid/offer across venues
bool check_trade_through(...) const;              // print worse than elsewhere?
```

A single book answers "what is the best price *here*". A trader needs "what is
the best price *anywhere*", because executing at a worse price than another
venue was displaying is what regulators call a trade-through.

- **Locked** (bid == ask across venues) and **crossed** (bid > ask) are reported,
  not normalised away. Rule 610(e) prohibits displaying quotes that cause them.
- **Size at the NBBO aggregates** across venues tied at the touch, because a
  taker sweeping the touch can lift all of it.
- **Venue attribution on a tie** goes to the lower venue id. Arbitrary, but
  deterministic, which is what replay requires.

This is topical rather than historical. The SEC published a proposal on
**2026-06-17** to **rescind the trade-through rule entirely** (Rel. 34-105655);
the comment period closed on **2026-08-17, two days ago**. The Commission's own
language: *"participants are locked in a technology and latency arms race for
speed, and the Commission believes that Rule 611 has contributed to this."*
There are now 17 operating equity exchanges, up from 8 in 2005, and the
trade-through rate the rule polices is under 2.5%.

**Honest limitation:** only one real venue feed is wired up. The consolidator is
exercised by differential fuzz across four *synthetic* venues, not four real
ones. Aligning two real US venues requires order-by-order feeds for the same
date, and the free Nasdaq samples do not overlap the IEX DEEP+ archive.

---

## What this is not

- **Single-threaded.** No sharding across cores. A real venue shards by symbol;
  this would be one shard.
- **No wire protocol.** No FIX, ITCH, OUCH or SBE for order entry. Orders arrive
  as structs.
- **No persistence, sequencing, or failover.** The event stream is the natural
  journal and the engine is deterministic, so replay-based recovery is the
  obvious next step. Not implemented. Given that two of the three failures cited
  at the top of this README were *recovery* failures rather than matching bugs,
  this is the most important thing missing.
- **One allocation model.** Price-time FIFO. CME alone runs ten (FIFO, Pro-Rata,
  Allocation, Configurable, Threshold Pro-Rata, and variants with Lead Market
  Maker steps), exposed per-instrument in FIX tag 1142.
- **The order index is `std::unordered_map`.** Measured and found *not* to be
  the bottleneck, so it stays documented rather than optimized.
- **Benchmarked on WSL2**, no core pinning, no isolated cores, no huge pages.
  The tail would improve on tuned hardware.

---

## Build

| Command | What |
|---|---|
| `make` | Build tests, benchmark, replay, IEX replay |
| `make test` | Correctness and differential suites |
| `make bench` | Latency and throughput across four regimes |
| `make asan` | Rebuild under AddressSanitizer + UBSan and run tests |
| `make replay` | Live terminal order book |
| `./scripts/fetch_iex.sh` | Download a real trading day (~1.7 GB) |

Requires g++ 13+ and make. Warning set is `-Wall -Wextra -Wpedantic -Wshadow
-Wconversion -Wsign-conversion -Wold-style-cast -Wcast-align`, and the tree is
clean under all of them. CI builds on g++ and clang and runs the suite under
sanitizers.

## References

- SEC Rel. 34-69655 (Nasdaq / Facebook IPO), 34-70694 (Knight Capital), 34-105655 (Rule 611 rescission proposal)
- CFTC Release 8369-21 (Coinbase, self-matching)
- Moallemi & Yuan, *A Model for Queue Position Valuation in a Limit Order Book*, Columbia GSB, rev. 2017
- Thompson et al., *Disruptor*, LMAX, 2011; Fowler, *The LMAX Architecture*, 2011
- CME Group, *Supported Matching Algorithms*; MIAX Pearl Equities Rule 2614(f)
- IEX DEEP+ Specification v1.04

## License

MIT. See [LICENSE](LICENSE).
