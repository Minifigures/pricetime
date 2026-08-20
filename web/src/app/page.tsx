import { OrderBook } from "@/components/order-book";

const REPO = "https://github.com/Minifigures/pricetime";

const HEADLINE: ReadonlyArray<readonly [string, string, string]> = [
  ["50,163,616", "real orders replayed", "one full trading day from a live US exchange"],
  ["100.0%", "agreement with the venue", "1,499 of 1,499 executions matched exactly"],
  ["40x", "faster after one fix", "1,986 ns to 50 ns per order"],
  ["50 ns", "per order, real data", "12.8 million orders per second"],
  ["16,807", "crash points verified", "killed at every byte, all recovered exactly"],
  ["3", "live exchanges", "consolidated into one best price, in real time"],
  ["62", "automated tests", "~400,000 fuzzed operations"],
  ["0", "dependencies", "g++ and make, nothing else"],
];

const FACTS: ReadonlyArray<readonly [string, string]> = [
  [
    "It is checked against a second implementation, constantly.",
    "The engine is written twice: once slow and obviously correct, once fast and not obviously anything. A fuzz test drives ~200,000 random operations through both and demands byte-identical output after every single operation.",
  ],
  [
    "It runs on real market data.",
    "It decodes an exchange's actual historical feed, 42 million network packets down to individual order IDs, and rebuilds the book order by order.",
  ],
  [
    "It grades itself against the exchange's own records.",
    "The feed says which orders were executed. The engine reconstructs what must have happened and checks whether it reaches the same conclusion. For Apple: 1,499 of 1,499.",
  ],
  [
    "It consolidates three live exchanges into one best price.",
    "Bitstamp, Coinbase and Kraken all quote BTC/USD at the same moment, so the engine rebuilds Bitstamp's book order by order and consolidates it against the other two into a real cross-venue best bid and offer. Building it surfaced three bugs, every one caught by checking against the exchanges' own published prices rather than trusting my own output.",
  ],
  [
    "Price-time priority is a choice, and it is not the only one.",
    "Which order fills first, once several sit at the same price, is a separate decision from price priority. CME runs ten different answers and exposes the choice per instrument. This implements three: FIFO, pro-rata, and CME's configurable split between them, which runs in production at 40 percent FIFO on grain and oilseed contracts. Under pro-rata a large order that arrived a moment ago outranks a small one that has rested all day, and that single change is why pro-rata markets show cancellation rates above 96 percent.",
  ],
  [
    "It survives being killed, and proves it.",
    "Every input is journalled before it is applied, and because the engine is deterministic those inputs are the state. The process was killed at every one of the 16,807 byte offsets in the journal; 16,406 of those cuts landed mid-record, and every one recovered to byte-identical state.",
  ],
  [
    "A hypothesis it got wrong is published, not deleted.",
    "The prediction about which workload would be slowest turned out to be the fastest by 4x. A refuted prediction is worth as much as a confirmed one.",
  ],
];

export default function Home(): React.JSX.Element {
  return (
    <main className="mx-auto max-w-4xl px-6 py-16 sm:py-24">
      <header className="mb-14">
        <h1 className="font-mono text-4xl font-semibold tracking-tight text-neutral-100 sm:text-5xl">
          pricetime
        </h1>
        <p className="mt-4 max-w-2xl text-lg leading-relaxed text-neutral-400">
          A stock exchange matching engine in C&#43;&#43;20. It decides who trades
          with whom, at what price, in what order &mdash; and it is running
          below, compiled to WebAssembly, inside your browser.
        </p>
        <p className="mt-3 max-w-2xl text-sm leading-relaxed text-neutral-500">
          The name is the rule the whole thing turns on:{" "}
          <span className="text-neutral-300">price&ndash;time priority</span>.
          Better prices execute first; at the same price, whoever arrived first
          executes first.
        </p>
      </header>

      <section className="mb-16">
        <OrderBook />
        <p className="mt-3 text-xs leading-relaxed text-neutral-600">
          Not a recording and not a mockup. This is the same{" "}
          <span className="font-mono text-neutral-500">Book</span> class the
          native benchmarks measure and the differential fuzz validates,
          compiled to a 42&nbsp;KB WebAssembly module. Latency shown in the
          repository is measured natively; browser clocks are deliberately
          coarsened and cannot resolve nanoseconds.
        </p>
      </section>

      <section className="mb-16 grid grid-cols-2 gap-px overflow-hidden rounded-lg border border-line bg-line sm:grid-cols-4">
        {HEADLINE.map(([n, label, sub]) => (
          <div key={label} className="bg-panel p-5">
            <div className="font-mono text-2xl text-neutral-100">{n}</div>
            <div className="mt-1 text-xs uppercase tracking-wider text-neutral-400">
              {label}
            </div>
            <div className="mt-2 text-xs leading-relaxed text-neutral-600">{sub}</div>
          </div>
        ))}
      </section>

      <section className="mb-16">
        <h2 className="mb-3 text-xl font-semibold text-neutral-200">
          Why this is hard
        </h2>
        <p className="mb-6 max-w-2xl leading-relaxed text-neutral-400">
          A matching engine is simple to describe and brutally unforgiving to
          build. When one breaks, it breaks expensively.
        </p>
        <ul className="space-y-3 border-l border-line pl-5 text-sm leading-relaxed text-neutral-400">
          <li>
            <span className="text-neutral-200">2012.</span> A deployment reached
            seven of eight servers. The firm lost{" "}
            <span className="text-neutral-200">$460 million in 45 minutes</span>{" "}
            and ceased to exist independently.
          </li>
          <li>
            <span className="text-neutral-200">2012.</span> A major exchange&apos;s
            opening auction entered an infinite loop, because a validation step
            could never converge under load. 38,000 orders were excluded.
          </li>
          <li>
            <span className="text-neutral-200">2026.</span> An exchange ran its
            primary and backup systems simultaneously; 2,800&#43; opening
            auctions silently never happened. There was no policy requiring
            anyone to check that they had.
          </li>
        </ul>
      </section>

      <section className="mb-16">
        <h2 className="mb-6 text-xl font-semibold text-neutral-200">
          How it is proven, not claimed
        </h2>
        <div className="space-y-6">
          {FACTS.map(([title, body]) => (
            <div key={title}>
              <h3 className="text-sm font-medium text-neutral-200">{title}</h3>
              <p className="mt-1 max-w-2xl text-sm leading-relaxed text-neutral-500">
                {body}
              </p>
            </div>
          ))}
        </div>
      </section>

      <section className="mb-16 rounded-lg border border-line bg-panel p-6">
        <h2 className="mb-2 text-sm font-semibold uppercase tracking-wider text-neutral-300">
          Two of those three were recovery failures
        </h2>
        <p className="max-w-2xl text-sm leading-relaxed text-neutral-400">
          Nasdaq&apos;s failover engine held state frozen nineteen minutes
          stale. NYSE could not answer whether 2,800 auctions had run, because
          no authoritative record existed to ask. Both are the same shape:
          state lived in one place, and there was no way to rebuild it
          elsewhere and prove the rebuild was right.
        </p>
        <p className="mt-3 max-w-2xl text-sm leading-relaxed text-neutral-400">
          pricetime journals every input before applying it. Because the engine
          is deterministic, the inputs <span className="text-neutral-100">are</span>{" "}
          the state. Testing it meant killing the process at{" "}
          <span className="text-neutral-100">every one of the 16,807 byte
          offsets</span> in the journal and demanding byte-identical recovery
          each time. 16,406 of those cuts landed mid-record.
        </p>
        <pre className="mt-4 overflow-x-auto rounded border border-line bg-ink p-4 font-mono text-xs leading-relaxed text-neutral-400">
{`2. CRASH      process killed mid-append
              journal was 214931 bytes, 3616 bytes lost
3. RECOVERY   read 4914 records from a cold start
4. VERDICT    event streams byte-identical : yes
              book state identical         : yes
              inputs lost to the crash     : 86 of 5000`}
        </pre>
        <p className="mt-3 max-w-2xl text-xs leading-relaxed text-neutral-600">
          That last line is the honest part. Inputs after the cut are genuinely
          gone, and an operator is told exactly how far the world rewound
          rather than that everything is fine.
        </p>
      </section>

      <section className="mb-16 rounded-lg border border-line bg-panel p-6">
        <h2 className="mb-2 text-sm font-semibold uppercase tracking-wider text-neutral-300">
          A market with no order-protection rule stays inverted
        </h2>
        <p className="max-w-2xl text-sm leading-relaxed text-neutral-400">
          Consolidating three live exchanges produced a book that is inverted
          essentially all the time. That turns out to be the{" "}
          <span className="text-neutral-100">correct</span> answer, not a bug.
          Checked against each venue&apos;s own API rather than my output:
        </p>
        <pre className="mt-3 overflow-x-auto rounded border border-line bg-ink p-4 font-mono text-xs leading-relaxed text-neutral-400">
{`bitstamp  bid 69449.38  ask 69449.39   own spread $0.01
coinbase  bid 69452.18  ask 69452.19   own spread $0.01
kraken    bid 69458.90  ask 69459.00   own spread $0.10`}
        </pre>
        <p className="mt-3 max-w-2xl text-sm leading-relaxed text-neutral-400">
          Every venue quotes a penny-wide market internally, yet they sit{" "}
          <span className="text-neutral-100">$3 to $10 apart</span>. Nothing
          closes the gap: taker fees of 0.26 to 0.6 percent are $180 to $400 on
          a $69,000 trade, so the dislocation is one to two orders of magnitude
          too small to arbitrage. And crypto has no rule forcing venues into
          line, which is the natural experiment US stock markets cannot run.
        </p>
        <p className="mt-3 max-w-2xl text-xs leading-relaxed text-neutral-600">
          The engine therefore has to accept a crossed consolidated book as
          valid. An implementation that assumes the best bid is always below
          the best ask fails within seconds of real data.
        </p>
      </section>

      <section className="mb-16 rounded-lg border border-line bg-panel p-6">
        <h2 className="mb-2 text-sm font-semibold uppercase tracking-wider text-neutral-300">
          The finding it surfaced in real data
        </h2>
        <p className="max-w-2xl text-sm leading-relaxed text-neutral-400">
          Running surveillance over one real trading day, the engine found that{" "}
          <span className="text-neutral-100">
            106,476 orders &mdash; 49.1% of every order that joined the queue
          </span>{" "}
          &mdash; were cancelled within one thousandth of a second of arriving,
          against a cancel-to-trade ratio of 124:1.
        </p>
        <p className="mt-3 max-w-2xl text-xs leading-relaxed text-neutral-600">
          That is a measurement, not an accusation. High cancellation is normal
          market-making behaviour, and the report says so in its own output.
          Every finding names the evidence that produced it, and none asserts
          intent.
        </p>
      </section>

      <section className="mb-16">
        <h2 className="mb-3 text-xl font-semibold text-neutral-200">Run it yourself</h2>
        <p className="mb-4 max-w-2xl text-sm leading-relaxed text-neutral-500">
          Zero dependencies. No package manager, no frameworks. Every number in
          the repository is reproducible in under a minute.
        </p>
        <pre className="overflow-x-auto rounded-lg border border-line bg-panel p-5 font-mono text-xs leading-relaxed text-neutral-300">
{`git clone ${REPO}
cd pricetime

make test     # 45 tests, including the differential fuzz
make bench    # latency percentiles, four flow regimes
make replay   # live order book in your terminal
make recover  # journal a run, crash it, recover, prove it

./scripts/feed_crypto.py | ./build/pricetime_nbbo
              # live best price across three real exchanges`}
        </pre>
      </section>

      <footer className="border-t border-line pt-6 text-xs leading-relaxed text-neutral-600">
        <a
          href={REPO}
          className="text-neutral-300 underline underline-offset-4 hover:text-neutral-100"
        >
          Source on GitHub
        </a>
        <span className="mx-2">·</span>
        MIT licensed
        <p className="mt-4">
          Market data provided for free by IEX. By accessing or using IEX
          Historical Data you agree to the IEX Historical Data Terms of Use.
        </p>
      </footer>
    </main>
  );
}
