import { EnginePanel } from "@/components/engine-panel";

const REPO = "https://github.com/Minifigures/pricetime";

/* Numbering here is not decoration. These are the three failures the design
   is a response to, in the order they happened, and the years are the point. */
const FAILURES: ReadonlyArray<readonly [string, string, string]> = [
  ["2012", "Knight Capital", "A deployment reached seven of eight servers. The firm lost $460 million in 45 minutes and did not survive as an independent company."],
  ["2012", "Nasdaq, Facebook's IPO", "The opening auction entered a loop it could not exit, because a validation step could never converge under load. 38,000 orders were excluded."],
  ["2026", "NYSE, fined $9M", "Primary and backup ran at once, and 2,800 opening auctions were silently treated as already done. No policy required anyone to check whether they had."],
];

const EVIDENCE: ReadonlyArray<readonly [string, string, string, string]> = [
  ["500,000", "operations, both engines", "Written twice, on purpose",
   "Once slow and obviously correct, once fast and not obviously anything. A fuzz test drives roughly 500,000 random operations through both and demands byte-identical output after every single one. It is running above."],
  ["1,499", "of 1,499 executions matched", "Graded against the exchange's own records",
   "The engine replays 50,163,616 real orders from one trading day at a live US exchange, reconstructs what must have happened, and checks itself. For Apple: 1,499 of 1,499 executions matched exactly."],
  ["16,807", "crash points, all recovered", "Killed at every possible instant",
   "Every input is journalled before it is applied, so the inputs are the state. The process was killed at all 16,807 byte offsets in the journal; 16,406 of those cuts landed mid-record. All recovered to byte-identical state."],
  ["4x", "wrong, and left in the README", "Wrong guesses left in",
   "A prediction about which workload would be slowest turned out to be the fastest by four times. It stayed in the README. A refuted prediction is worth as much as a confirmed one."],
];

export default function Home(): React.JSX.Element {
  return (
    <main>
      <header className="mx-auto max-w-5xl px-6 pt-8 sm:px-10">
        <div className="rule-b flex items-baseline justify-between pb-3">
          <span className="font-mono text-[11px] uppercase tracking-[0.2em] text-ink2">
            pricetime
          </span>
          <span className="font-mono text-[11px] tracking-wider text-ink3">
            C&#43;&#43;20 · MIT
          </span>
        </div>
      </header>

      <section className="mx-auto max-w-5xl px-6 pb-10 pt-12 sm:px-10 sm:pt-16">
        <h1 className="max-w-4xl font-display text-[2.75rem] font-light leading-[1.04] tracking-[-0.02em] sm:text-[4.25rem]">
          Every trade in the world passes through a matching engine.
          <span className="block text-ink3">Almost every first one is quietly wrong.</span>
        </h1>
        <p className="mt-7 max-w-2xl text-[1.0625rem] leading-relaxed text-ink2">
          It decides who trades with whom, at what price, in what order. Simple
          enough to describe in a sentence and unforgiving enough that the
          mistakes are measured in hundreds of millions of dollars. This one is
          built to be <em className="not-italic text-ink">proven</em> correct
          rather than claimed correct, and the proof is running below, in this
          tab, compiled from the same C&#43;&#43; the benchmarks measure.
        </p>
      </section>

      {/* The instrument. Full bleed, and the only dark object on the page. */}
      <section className="mx-auto max-w-5xl px-6 sm:px-10">
        <EnginePanel />
      </section>

      <section className="mx-auto max-w-5xl px-6 py-14 sm:px-10 sm:py-20">
        <p className="clause">Why it is hard</p>
        <div className="mt-8 grid gap-px bg-rule sm:grid-cols-3">
          {FAILURES.map(([year, who, what]) => (
            <article key={who} className="bg-paper pr-6 pt-6 sm:pt-8">
              <div className="font-mono text-[11px] tracking-widest text-ink3">{year}</div>
              <h3 className="mt-2 font-display text-xl font-normal leading-snug">{who}</h3>
              <p className="mt-3 text-[0.9375rem] leading-relaxed text-ink2">{what}</p>
            </article>
          ))}
        </div>
        <p className="mt-10 max-w-2xl text-[0.9375rem] leading-relaxed text-ink2">
          Two of those three were not matching bugs. They were{" "}
          <em className="not-italic text-ink">recovery</em> failures: state
          existed in one place and there was no way to rebuild it somewhere else
          and prove the rebuild was right.
        </p>
      </section>

      <section className="rule-t mx-auto max-w-5xl px-6 py-14 sm:px-10 sm:py-20">
        <p className="clause">What is actually claimed, and how it is shown</p>
        <div className="mt-10">
          {EVIDENCE.map(([figure, unit, title, body]) => (
            <div
              key={title}
              className="rule-t grid gap-3 py-7 first:border-t-0 first:pt-0 sm:grid-cols-[11rem_1fr] sm:gap-10"
            >
              <div>
                <div className="tnum font-mono text-[1.375rem] leading-none text-ink">{figure}</div>
                <div className="clause mt-2 leading-snug">{unit}</div>
              </div>
              <div>
                <h3 className="font-display text-2xl font-normal leading-tight">{title}</h3>
                <p className="mt-2 max-w-2xl text-[0.9375rem] leading-relaxed text-ink2">{body}</p>
              </div>
            </div>
          ))}
        </div>
      </section>

      <section className="rule-t mx-auto max-w-5xl px-6 py-14 sm:px-10 sm:py-20">
        <p className="clause">The measurement that changed the design</p>
        <h2 className="mt-6 max-w-3xl font-display text-3xl font-light leading-tight sm:text-[2.5rem]">
          Real data ran thirty times slower than synthetic tests. The algorithm
          was not the problem.
        </h2>
        <div className="mt-10 grid gap-8 sm:grid-cols-2">
          <p className="text-[0.9375rem] leading-relaxed text-ink2">
            The price table had grown to 27.7&nbsp;MB and this CPU&apos;s fast
            cache holds 20. Restructuring so the hot data stays resident took one
            symbol from <span className="tnum font-mono text-ink">1,986&nbsp;ns</span>{" "}
            per order to <span className="tnum font-mono text-ink">50</span>, and
            another from 4,634 to 62, with the output unchanged.
          </p>
          <p className="text-[0.9375rem] leading-relaxed text-ink2">
            So the structure is not simply fast. It is fast on the condition that
            its working set fits in cache, and that condition is a design
            constraint rather than a footnote. Every latency figure published
            here says which machine produced it and what it excludes.
          </p>
        </div>
      </section>

      <section className="rule-t mx-auto max-w-5xl px-6 py-14 sm:px-10 sm:py-20">
        <p className="clause">Run it</p>
        <p className="mt-6 max-w-2xl text-[0.9375rem] leading-relaxed text-ink2">
          No package manager and no framework. Two commands you almost certainly
          already have. Every number on this page is reproducible in about a
          minute.
        </p>
        <pre className="mt-8 overflow-x-auto border border-rule bg-paper2 p-6 font-mono text-[12.5px] leading-[1.9]">
{`git clone ${REPO}
cd pricetime

make test      # 70 tests, ~550,000 fuzzed operations
make bench     # latency percentiles, four flow regimes
make recover   # journal a run, crash it, recover, prove it
make replay    # the book, in your terminal`}
        </pre>
      </section>

      <footer className="rule-t mx-auto max-w-5xl px-6 py-12 sm:px-10">
        <div className="flex flex-wrap items-baseline justify-between gap-4">
          <a href={REPO} className="font-mono text-sm underline underline-offset-4 hover:text-ink2">
            Source on GitHub
          </a>
          <p className="max-w-lg text-xs leading-relaxed text-ink3">
            Market data provided for free by IEX. By accessing or using IEX
            Historical Data you agree to the IEX Historical Data Terms of Use.
          </p>
        </div>
      </footer>
    </main>
  );
}
