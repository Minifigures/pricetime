# pricetime

A multi-venue limit order book and matching engine in C++20, built to be fast
and *proven* correct rather than asserted correct.

The name is the rule the whole thing turns on: **price-time priority**. Better
prices execute first; at the same price, whoever arrived first executes first.

**[Run it in your browser](https://pricetime-mu.vercel.app)** (no install). The
page compiles this same C++ to WebAssembly and runs *both* implementations in
the tab on the same input, comparing them event for event. The divergence
count stays at zero. Hovering a price level quotes what sweeping to it would
actually fill at.

```
git clone https://github.com/Minifigures/pricetime && cd pricetime
make test      # 68 tests, including ~500k-op differential fuzz
make bench     # latency percentiles across four flow regimes
make shardbench # throughput vs shard count
make tsan      # ThreadSanitizer over the concurrent paths
make replay    # live terminal order book
make recover   # journal, crash, recover, prove it
./scripts/feed_crypto.py 60 | ./build/pricetime_nbbo   # live 3-venue NBBO
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
  cancellation**, so it could never converge. Infinite loop. The failover went
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
one another,"* inflating apparent volume, and that tainted data fed the CME
Bitcoin Real Time Index, CoinMarketCap and the NYSE Bitcoin Index. Not fraud;
two well-intentioned internal algorithms crossing each other.

**Gap:** MIAX Rule 2614(f) and Coinbase both implement four modes (cancel
newest, cancel oldest, decrement and cancel, cancel both). We implement three,
and no decrement mode.

---

## Allocation is a choice, not a given

```cpp
Book b(floor, ceil, SelfTradePolicy::None, pool, hot_ticks, Allocation::ProRata);
```

Price priority selects the level. What happens *within* a level, once several
orders sit at the same price, is a separate decision, and treating FIFO as the
only answer is a parochialism of equity markets. CME exposes the algorithm
per-instrument in FIX tag 1142 and runs ten of them; Eurex runs three; ICE uses
a time-weighted pro-rata on short-term-rate products.

**FIFO**: the oldest order at the price fills first.

**Pro-rata**: each resting order receives a share proportional to its size,
rounded **down**, and the rounding remainder is then distributed FIFO. Pro-rata
is never the last step of an algorithm precisely because of that rounding;
something has to place the leftovers.

Timestamps are deliberately not consulted in the proportional step. A large
order that arrived a moment ago outranks a small one that has rested all day,
and that asymmetry is the whole point:

```
resting: 10 (oldest)  and  90 (newest),  aggressor buys 50
  FIFO      ->  10 fills 10, 90 fills 40      (arrival wins)
  PRO-RATA  ->  10 fills  5, 90 fills 45      (size wins)
```

**Why it matters beyond mechanics.** Field & Large (CFS WP 2008/40) found that
pro-rata one-tick futures markets sit at the minimum spread essentially always,
with depth around **100x mean trade size** and cancellation rates **above 96
percent**, because rationing by size makes traders submit orders far larger
than they intend to fill. The allocation rule changes what participants *do*,
not just who gets filled.

**Split**: CME's Configurable algorithm. A fixed percentage of each fill is
placed FIFO and the remainder pro-rata. CME calls the parameter *Price Time
Percentage*, an integer where 100 is pure price-time and 0 is pure pro-rata, and
runs it in production at **40 percent FIFO on grain and oilseed contracts**. It
exists because neither pure rule is satisfactory: pure FIFO lets one large
resting order block everyone behind it, and pure pro-rata rewards submitting
size you never intend to fill.

The parameterisation is verified rather than asserted: `split_at_100_percent_is_exactly_fifo`
and `split_at_0_percent_is_exactly_prorata` require the degenerate cases to
produce byte-identical fills to the standalone policies. If they diverge, these
are three unrelated code paths wearing one name.

**Time-weighted**: one kernel that covers five published exchange algorithms.
Sorting a level by time, with `Q_j` the cumulative volume from order *j* onward
and `V = Q_1`:

```
f_j(k) = (Q_j^k  -  Q_{j+1}^k) / V^k
```

| k | what it is |
|---|---|
| 1 | pure pro-rata (CME `C`, Eurex Pro-Rata) |
| 2 | Eurex Time-Pro-Rata, **and** ICE Euribor / SARON / SONIA / SOFR |
| 4 | ICE Short Sterling and Euroswiss |
| large | price-time FIFO |

The Eurex identity is not obvious and is the nicest part: Eurex publishes
Time-Pro-Rata as a recursion, `a_i = min(q_i, A_i(1 - (1 - q_i/Q_i)^2))`.
Expanding by induction gives `A(Q_i^2 - Q_{i+1}^2)/V^2`, which is this kernel at k=2.
Two exchanges documenting the same algorithm in two notations.

Measured on Eurex's own published Example 7-7 (20, 20, 50 in time order,
aggressor 25), oldest order's fill:

```
k=1   7   pro-rata, size wins
k=2  11   Eurex TPR / ICE Euribor
k=4  17   ICE Short Sterling, much closer to FIFO
k=32 20   FIFO exactly
```

**CME's FIFO Exception** is implemented too: when an aggressor takes a whole
price level, FIFO applies in place of the configured algorithm. The outcome is
identical either way since everything fills, so this is purely about not doing
proportional arithmetic that cannot change the answer.

All four policies run the full differential fuzz, across the Split percentage
range and the time-weight exponents, because allocation is the most intricate
part of matching and unit tests passing is not sufficient reason to trust it.
That is not hypothetical: the time-weighted kernel initially computed correct
allocations and then discarded them, because the emit loop sat inside the
pro-rata branch. It silently ran as FIFO and **every existing test still
passed**. The differential campaign is what found it.

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
26 seeded campaigns, plus 23 hand-written behavioural tests, 11 consolidation
tests, 5 sharding tests, 6 journal and recovery tests, and 12 allocation tests.
The differential campaigns run under FIFO, pro-rata, split across five FIFO
percentages, and the time-weighted kernel across five exponents. The NBBO is separately fuzzed across four venues against an independent
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
because Nasdaq ITCH (the feed everyone reaches for first) is big-endian with a
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
must have caused it: an IOC on the opposite side, at the reported price, for
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

### Running it on three real exchanges

```
./scripts/feed_crypto.py 60 | ./build/pricetime_nbbo
```

Crypto rather than equities, for a specific reason: a genuine cross-venue NBBO
needs two or more venues quoting **the same instrument at the same time**, and
the free US order-by-order archives do not overlap. IEX DEEP+ starts
2024-10-01; the free Nasdaq ITCH samples are 2019 and 2025-26. Crypto exchanges
all quote BTC/USD concurrently, without authentication.

| venue | role |
|---|---|
| **Bitstamp** | order-by-order. Individual orders with exchange IDs, so the book is rebuilt and its BBO derived from reconstructed depth. This is the one that exercises the matching engine. |
| **Coinbase** | top of book only |
| **Kraken** | top of book only |

The display labels which venues are reconstructed and which are quotes. Claiming
depth that was never sent is the easy lie here.

**Nothing captured is committed.** Bitstamp is the only one of these venues whose
terms affirmatively permit redistribution (*"Bitstamp allows the incorporation and
redistribution of our exchange data"*). Coinbase, Kraken, Gemini and OKX all
prohibit it explicitly. So the feed streams live and prints to stdout, and no
market data from any venue is checked into this repository. Binance is deliberately excluded:
its liquid pair is BTC/**USDT**, a different instrument, and folding it into a
USD consolidated quote would be wrong in a way that is invisible on screen.

### Two bugs this found, both caught by checking against the venues' own tickers

**A live L3 subscription is not enough on its own.** Subscribing mid-session
only shows orders created *after* connecting, so the reconstructed book holds a
fraction of real depth. Measured: that partial book showed a **$2.21 spread
while Coinbase showed $0.01**, and the consolidator then reported thousands of
crossed markets that were pure artifact. Fixed with snapshot-plus-deltas in the
correct order: subscribe and buffer first, then fetch the snapshot, seed from
it, then replay buffered deltas newer than the snapshot's timestamp. Doing the
snapshot first would silently drop everything in the gap.

**A stale quote does not look stale, it looks like an arbitrage.** Kraken's
ticker channel published 8 times in 30 seconds, leaving its quote 20 seconds old
and $18 from the others. The consolidator dutifully found free money that did
not exist. Moved to the book channel (1,031 updates in the same window) and
added quote ageing on top, which is exactly why real consolidated feeds carry
per-participant timestamps.

### The finding: a market with no order-protection rule stays inverted

The consolidated book is inverted essentially all of the time, and that is the
**correct** state rather than a bug. The evidence, cross-checked against each
venue's own REST API rather than taken from my own output:

```
bitstamp  bid 69449.38  ask 69449.39   own spread $0.01
coinbase  bid 69452.18  ask 69452.19   own spread $0.01
kraken    bid 69458.90  ask 69459.00   own spread $0.10
```

Every venue quotes a penny-wide market internally, and they sit **$3 to $10
apart from each other**. Nothing closes that gap, because taker fees of 0.26 to
0.6 percent are **$180 to $400** on a $69,000 trade. The dislocation is one to
two orders of magnitude too small to arbitrage. And crypto has no Reg NMS
order-protection rule, so nothing forces the venues into line either.

This is the natural experiment US equities cannot run: what a consolidated quote
looks like *without* a trade-through rule. Worth noting while the SEC has a live
proposal (Rel. 34-105655, 2026-06-17) to rescind exactly that rule.

**The engine must therefore tolerate a crossed NBBO as valid.** An implementation
that asserts `best_bid < best_ask` fires immediately on real data.

### Two bugs the venues' own documentation does not warn you about

**Coinbase's `ticker` channel fires 1:1 with trades.** Measured: 170 ticker
messages against 170 matches, 170/170 trade-id overlap. Between prints its
`best_bid` and `best_ask` are stale, and in a quiet market they stay stale.
`level2_batch` is unauthenticated and delivers full depth batched at 50ms, so
that is what this uses.

**Kraken's book channel is a fixed-depth window and must be trimmed after every
delta.** Without the trim, the local book grows past the window with levels the
venue no longer considers inside, and the touch silently freezes. Kraken
publishes a CRC32 over the top ten levels precisely so this is detectable:
replaying one capture, no trim gives 683 checksum mismatches against 192
matches; trimming gives 875 matches and zero mismatches.

---

## Sharding across cores

No book is ever written by more than one thread. A book belongs to one shard, a
shard belongs to one thread, and a message routes to its shard by hashing
(venue, symbol) through splitmix64, because dense small ids would otherwise cluster
badly on a power-of-two shard count. Inside a shard the engine is the same
single-threaded code the differential fuzz already validated, so threading
introduces no new matching semantics to get wrong.

This is the single-writer principle, and the reason for it is measured rather
than assumed. LMAX published: 500M increments take 300 ms on one thread,
10,000 ms with a lock, and **224,000 ms with two threads contending that lock**.
Three orders of magnitude, to protect an increment.

Cross-shard reads (an NBBO must read every venue's touch, and those live on
different threads) go through a **seqlock**, not a mutex: readers never block
writers, which matters because the writer is the hot path.

### Determinism is the acceptance criterion, not throughput

The same input replayed on 2, 4 and 8 shards must produce **byte-identical
per-book event streams** to serial execution. Six seeds, 20,000 messages each.
If that fails, the sharding is wrong and no throughput number redeems it.

### ThreadSanitizer found a bug my own test could not

The first seqlock stored its payload as a plain struct and relied on the
version counter to prevent torn reads. I wrote a test that hammered it with
**400,000 concurrent reads against a live writer and saw zero tears**, because
x86 is forgiving.

TSan reported it as a data race anyway, and TSan was right: concurrent
non-atomic access is undefined behaviour under the C++ memory model whatever
protocol sits on top, and the compiler is entitled to assume races never
happen. The payload is now relaxed atomics ordered by two fences, the same
loads and stores on x86, but actually defined. TSan is clean and `make tsan`
plus a CI job keep it that way.

Passing your own test is not the same as being correct.

### Scaling, including where it stops

```
12 hardware threads (6 performance + 4 efficiency cores) · 4M messages
WSL2: no core pinning, no core isolation, no huge pages
```

| shards | uniform, 256 symbols | 60% of flow on one symbol |
|---|---|---|
| 1 | 2.09 M msg/s | 2.55 M msg/s |
| 2 | 4.20 (2.02x) | 3.99 (1.57x) |
| 4 | 8.38 (4.02x) | 7.95 (3.12x) |
| 6 | 11.33 (5.43x) | 8.58 (3.36x) |
| 8 | 14.76 (7.07x) | **13.99 (5.48x)** |
| 12 | **18.80 (9.01x)** | 10.45 (4.09x) |

An earlier version of this table reported 4.12x uniform rather than 9.01x. That
version was wrong, and how it was wrong is worth keeping: each book was
pre-allocating an order pool sized for a single busy instrument, so 1,024 books
reserved over 3 GB before processing a message. The benchmark was measuring
allocator pressure, not the engine. Peak RSS on the CI-sized run went from
~3.2 GB to 424 MB once books started small and grew on demand, and the real
scaling appeared underneath.

The hot-symbol column is the interesting one, and it is measured precisely
because uniform distribution flatters any sharding scheme. **It peaks at 8 shards and regresses at 12**,
from 5.48x down to 4.09x. If most traffic is one instrument, that instrument's
shard does most of the work however many shards exist, and adding more only
adds scheduling overhead. SPY and QQQ carry orders of magnitude more messages than a typical
name, so this is the real-world case, not the pathological one.

---

## Market surveillance

`make iex` also runs a surveillance pass over the engine's own event stream.

It is **deliberately off the hot path**: the engine emits events, surveillance
reads them afterwards. Nothing in it can slow a match or change a matching
decision. Surveillance that can influence execution is not surveillance.

Detected: trade-throughs (Rule 611), locked and crossed markets (Rule 610(e)),
self-match attempts (CEA 4c(a); cf. the $6.5M CFTC penalty against Coinbase for
its own programs crossing each other), quote stuffing by cancel-to-trade ratio,
and fleeting orders.

Real output, AAPL on 2024-12-23:

```
  observed        : 216,846 rested, 186,652 cancelled, 1,502 traded
  cancel-to-trade : 124.3:1

  [FLEETING_ORDER] 106,476 orders (49.1% of all resting orders) were
                   cancelled within 1000 microseconds of arriving
  [QUOTE_STUFFING] cancel-to-trade 124.3:1, above the 100:1 threshold
```

**Nearly half of every displayed order on Apple that day existed for under a
millisecond.** That is a measurement, not an accusation: high cancel rates are
normal modern market-making behaviour, and the report says so. Every finding
names the evidence that produced it, and none asserts intent.

### The optional language-model stage, and its boundary

`scripts/explain.sh` turns those findings into prose for a compliance reviewer.
It is a **separate process** and the boundary is the point: a matching engine
must be deterministic and auditable, and a language model is neither. It never
touches a price, a fill, or a matching decision. It is handed numbers the engine
already computed, told it may not invent any, and told not to assert intent.

With no API key set, the stage prints why it skipped and exits zero. The
engine's deterministic report is the deliverable; the narrative is a
convenience over it.

---

## Crash recovery

```
make recover
```

This README opens with three real failures, and **two of them were not matching
bugs at all, they were recovery failures.** Nasdaq's Facebook IPO cross failed
over to an engine whose state was frozen nineteen minutes earlier, excluding
38,000 orders. NYSE ran primary and backup simultaneously and could not answer
whether 2,800 opening auctions had happened, because no authoritative record
existed to ask.

Both are the same shape: state existed in one place, and there was no way to
rebuild it somewhere else and prove the rebuild was right.

**The journal records inputs, not outputs.** That works only because the engine
is deterministic: the same inputs in the same order produce the same book and
the same event stream. So the inputs are the smaller, more durable thing to
persist and the outputs are derivable. LMAX phrase it exactly: *"the current
state of the Business Logic Processor is entirely derivable by processing the
input events."*

Records are length-prefixed and CRC-32 checksummed. Recovery stops at the first
record that fails to verify rather than salvaging past it, because a torn tail
is the *expected* shape of a process dying mid-append, not an exceptional case.
The on-disk format writes every field explicitly rather than memcpy'ing a
struct, so a compiler changing its layout cannot silently change the format.

### Verified by killing it at every possible instant

`tests/test_journal.cpp` truncates the journal at **every byte offset in turn**
and demands that what recovers is a prefix of what was written, and that
replaying it produces byte-identical events and book state to running exactly
those operations live.

**16,807 cut points. 16,406 of them landed mid-record. All correct.**

Corruption inside a record is caught by the checksum and stops recovery there.
A foreign file is rejected rather than misread.

```
1. RUNNING    journalled 5000 inputs while matching
              book: bid 99.79 / ask 100.05, 410 resting, 2558 trades
2. CRASH      process killed mid-append
              journal was 214931 bytes, 3616 bytes lost (cut at byte 211315)
3. RECOVERY   read 4914 records from a cold start
              tail: torn record at tail
4. VERDICT    compared against an independent live run of the same 4914 inputs
              event streams byte-identical : yes
              book state identical         : yes
              inputs lost to the crash     : 86 of 5000
```

That last line is the honest part. Inputs after the cut are genuinely gone, and
an operator learns exactly how far the world rewound rather than being told
everything is fine.

**Not replication, consensus, or failover.** One journal on one disk. Getting
from here to a replicated state machine is the next problem and a much larger
one.

---

## What this is not

- **No wire protocol.** No FIX, ITCH, OUCH or SBE for order entry. Orders arrive
  as structs.
- **Sharding is batch, not streaming.** Messages are partitioned up front and
  each shard's slice is replayed on its own thread. A live venue needs an
  ingress sequencer establishing total order before fan-out; that is not here.
- **No replication, no consensus, no failover.** Recovery from a local journal
  exists (above); surviving the loss of the *machine* does not. A real venue
  replicates the input stream to standbys and needs a sequencer establishing
  total order before fan-out. That is the next problem and it is much larger.
- **No ingress sequencing.** Sharded replay is batch: messages are partitioned
  up front. A live venue must establish total order at ingress, which is the
  single-writer bottleneck every exchange architecture is organised around.
- **Four allocation models, not ten.** FIFO, pro-rata, CME's configurable
  split, and the time-weighted kernel. Still missing: top-order priority (with
  its TOP Min / TOP Max / TOP % parameters), Lead Market Maker allocations,
  pro-rata minimums, and CME's leveling step. Also unimplemented: the LIFFE
  *rank* family, which is a genuinely different profile from the power kernel
  and should not be conflated with it.
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
| `make tsan` | Rebuild under ThreadSanitizer and run the concurrent tests |
| `make shardbench` | Throughput against shard count |
| `make replay` | Live terminal order book |
| `make recover` | Journal a run, crash it, recover, verify against a live replay |
| `make nbbo` | Build the live cross-venue consolidator |
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
- Field & Large, *Pro-Rata Matching in One-Tick Markets*, CFS Working Paper 2008/40
- Haynes & Onur, *Precedence rules in matching algorithms*, J. Commodity Markets 19, 2020
- CME Group, *Supported Matching Algorithms*; MIAX Pearl Equities Rule 2614(f)
- IEX DEEP+ Specification v1.04

## License

MIT. See [LICENSE](LICENSE).
