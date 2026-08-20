"use client";

import { useCallback, useEffect, useRef, useState } from "react";

type Level = readonly [number, number];

interface Snapshot {
  readonly bids: readonly Level[];
  readonly asks: readonly Level[];
  readonly tape: readonly Level[];
  readonly bid: number;
  readonly ask: number;
  readonly msgs: number;
  readonly trades: number;
  readonly resting: number;
  readonly compared: number;
  readonly diverged: number;
  readonly last: string;
}

interface EngineModule {
  readonly ccall: (
    n: string, r: string | null, a: readonly string[], v: readonly unknown[],
  ) => unknown;
}
type ModuleFactory = () => Promise<EngineModule>;
declare global { interface Window { PricetimeModule?: ModuleFactory } }

const EMPTY: Snapshot = {
  bids: [], asks: [], tape: [], bid: 0, ask: 0,
  msgs: 0, trades: 0, resting: 0, compared: 0, diverged: 0, last: "",
};

/* Big figure / pips. FX and rates desks dim the leading digits everyone
   already knows and let the moving ones carry the eye. Applying it here is
   the single most "this was built by someone who has looked at a real book"
   detail available for two lines of code. */
function Price({ v, tone }: { v: number; tone: "bid" | "ask" }): React.JSX.Element {
  if (!v) return <span className="text-instInk/25">&#183;</span>;
  const s = (v / 100).toFixed(2);
  const head = s.slice(0, -3);
  const tail = s.slice(-3);
  return (
    <span className={tone === "bid" ? "text-bidLit" : "text-askLit"}>
      <span className="opacity-45">{head}</span>
      <span className="font-medium">{tail}</span>
    </span>
  );
}

export function EnginePanel(): React.JSX.Element {
  const [snap, setSnap] = useState<Snapshot>(EMPTY);
  const [ready, setReady] = useState(false);
  const [running, setRunning] = useState(true);
  const [error, setError] = useState<string | null>(null);
  const mod = useRef<EngineModule | null>(null);
  const shell = useRef<HTMLDivElement | null>(null);
  // Nothing is proven to someone who cannot see the panel, so stop the loop
  // when it scrolls away. On a page arguing about wasted cycles, burning them
  // offscreen would be the wrong position to hold.
  const [onscreen, setOnscreen] = useState(true);
  // Which side, and how deep, the pointer is resting. A book is only useful if
  // you can ask what taking it would cost.
  const [sweep, setSweep] = useState<{ side: "bid" | "ask"; row: number } | null>(null);

  useEffect(() => {
    const el = shell.current;
    if (!el) return;
    const io = new IntersectionObserver(
      ([e]) => setOnscreen(e.isIntersecting),
      { rootMargin: "128px" },
    );
    io.observe(el);
    return () => io.disconnect();
  }, [ready]);

  useEffect(() => {
    let cancelled = false;
    const el = document.createElement("script");
    el.src = "/pricetime.js";
    el.async = true;
    el.onload = (): void => {
      const f = window.PricetimeModule;
      if (!f) { setError("engine did not load"); return; }
      void f().then((m) => {
        if (cancelled) return;
        mod.current = m;
        m.ccall("pt_reset", null, ["number"], [0xdec0de]);
        setReady(true);
      });
    };
    el.onerror = (): void => setError("engine did not load");
    document.body.appendChild(el);
    return () => { cancelled = true; el.remove(); };
  }, []);

  const tick = useCallback((): void => {
    const m = mod.current;
    if (!m) return;
    m.ccall("pt_step", null, ["number"], [40]);
    const raw = m.ccall("pt_snapshot", "string", [], []);
    if (typeof raw === "string") setSnap(JSON.parse(raw) as Snapshot);
  }, []);

  useEffect(() => {
    if (!ready || !running || !onscreen) return;
    const id = window.setInterval(tick, 90);
    return () => window.clearInterval(id);
  }, [ready, running, onscreen, tick]);

  // Depth bars carry cumulative size, not size at the level, and both sides
  // divide by the same deepest visible total. Cumulative depth is monotonic,
  // so the divisor only moves when total visible depth moves. Scaling to the
  // largest single level instead makes every bar jump whenever one level
  // trades, which reads as noise rather than as depth.
  const cum = (ls: readonly Level[]): number[] => {
    let run = 0;
    return ls.map((l) => (run += l[1]));
  };
  const bidCum = cum(snap.bids);
  const askCum = cum(snap.asks);
  const scale = Math.max(1, bidCum[bidCum.length - 1] ?? 0, askCum[askCum.length - 1] ?? 0);
  const rows = Array.from({ length: 8 }, (_, i) => i);
  const spread = snap.bid > 0 && snap.ask > 0 ? snap.ask - snap.bid : null;
  const mid = snap.bid > 0 && snap.ask > 0 ? (snap.bid + snap.ask) / 2 : 0;

  // What a marketable order resting at the hovered level would actually pay:
  // it eats every level in front of it, so the fill is the size-weighted mean
  // of those levels and not the price you clicked. The gap between the two is
  // the cost of the depth you crossed.
  const quote = ((): null | { qty: number; avg: number; slip: number; notional: number } => {
    if (!sweep || mid === 0) return null;
    const side = sweep.side === "bid" ? snap.bids : snap.asks;
    const take = side.slice(0, sweep.row + 1);
    if (take.length === 0) return null;
    const qty = take.reduce((a, l) => a + l[1], 0);
    if (qty === 0) return null;
    const notional = take.reduce((a, l) => a + l[0] * l[1], 0);
    const avg = notional / qty;
    return { qty, avg, slip: ((avg - mid) / mid) * 100, notional: notional / 100 };
  })();

  if (error !== null) {
    return (
      <div className="bg-instrument p-8 font-mono text-sm text-askLit">
        {error}. It runs entirely in this tab as WebAssembly, so a content
        blocker is the usual cause.
      </div>
    );
  }

  return (
    <div ref={shell} className="bg-instrument text-instInk">
      {/* THE PROOF. This is the page's whole argument, so it sits above the
          book rather than below it: two independent implementations, one
          input, and a count of events that came out identical. */}
      <div className="flex flex-wrap items-baseline justify-between gap-x-8 gap-y-3 border-b border-instRule px-6 py-5 sm:px-8">
        <div>
          <div className="clause !text-instInk/45">events compared, live</div>
          <div className="mt-1 flex items-baseline gap-3">
            <span
              key={snap.compared}
              className="tick font-mono text-3xl tabular-nums tracking-tight text-signal sm:text-4xl"
              style={{ color: snap.diverged > 0 ? "var(--ask-lit)" : "var(--signal)" }}
            >
              {snap.compared.toLocaleString()}
            </span>
            <span className="font-mono text-xs text-instInk/50">
              {snap.diverged > 0 ? `${snap.diverged} DIVERGENT` : "0 divergent"}
            </span>
          </div>
        </div>
        <p className="max-w-xs text-xs leading-relaxed text-instInk/45">
          Two implementations run on the same input in this tab: the fast one,
          and a deliberately slow one written to be obviously correct. Every
          event is compared.
        </p>
        <button
          type="button"
          onClick={() => setRunning((r) => !r)}
          disabled={!ready}
          className="font-mono text-[11px] uppercase tracking-widest text-instInk/60 underline underline-offset-4 hover:text-instInk disabled:opacity-30"
        >
          {running ? "pause" : "resume"}
        </button>
      </div>

      {/* The book. Depth reads outward from the spread, which is where a
          trader's eye actually sits, rather than top-down like a table. */}
      <div className="grid grid-cols-1 sm:grid-cols-2" onMouseLeave={() => setSweep(null)}>
        <div className="order-2 px-6 py-5 sm:order-1 sm:border-r sm:border-instRule sm:px-8">
          <div className="clause !text-instInk/35 mb-3 text-right">bids</div>
          {rows.map((i) => {
            const l = snap.bids[i];
            return (
              <div
                key={`b${i}`}
                onMouseEnter={() => l && setSweep({ side: "bid", row: i })}
                className="flex h-6 items-center justify-end gap-3 font-mono text-[13px] tabular-nums"
                style={
                  sweep?.side === "bid" && i <= sweep.row
                    ? {
                        background: "rgba(69,196,137,0.08)",
                        boxShadow:
                          i === sweep.row ? "inset 0 -1px 0 rgba(69,196,137,0.5)" : undefined,
                      }
                    : undefined
                }
              >
                <div className="relative h-4 flex-1">
                  {l && (
                    <div
                      className="absolute right-0 top-0 h-full transition-[width] duration-150 ease-out"
                      style={{
                        width: `${Math.max(2, ((bidCum[i] ?? 0) / scale) * 100)}%`,
                        background: "rgba(69,196,137,0.16)",
                        borderRight: "1px solid rgba(69,196,137,0.55)",
                      }}
                    />
                  )}
                </div>
                <span className="w-12 text-right text-instInk/40">{l ? l[1] : ""}</span>
                <span className="w-20 text-right"><Price v={l ? l[0] : 0} tone="bid" /></span>
              </div>
            );
          })}
        </div>
        <div className="order-1 border-b border-instRule px-6 py-5 sm:order-2 sm:border-b-0 sm:px-8">
          <div className="clause !text-instInk/35 mb-3">asks</div>
          <div className="flex flex-col-reverse sm:flex-col">
          {rows.map((i) => {
            const l = snap.asks[i];
            return (
              <div
                key={`a${i}`}
                onMouseEnter={() => l && setSweep({ side: "ask", row: i })}
                className="flex h-6 items-center gap-3 font-mono text-[13px] tabular-nums"
                style={
                  sweep?.side === "ask" && i <= sweep.row
                    ? {
                        background: "rgba(255,111,97,0.08)",
                        boxShadow:
                          i === sweep.row ? "inset 0 -1px 0 rgba(255,111,97,0.5)" : undefined,
                      }
                    : undefined
                }
              >
                <span className="w-20"><Price v={l ? l[0] : 0} tone="ask" /></span>
                <span className="w-12 text-instInk/40">{l ? l[1] : ""}</span>
                <div className="relative h-4 flex-1">
                  {l && (
                    <div
                      className="absolute left-0 top-0 h-full transition-[width] duration-150 ease-out"
                      style={{
                        width: `${Math.max(2, ((askCum[i] ?? 0) / scale) * 100)}%`,
                        background: "rgba(255,111,97,0.16)",
                        borderLeft: "1px solid rgba(255,111,97,0.55)",
                      }}
                    />
                  )}
                </div>
              </div>
            );
          })}
          </div>
        </div>
      </div>

      <div className="grid grid-cols-2 border-t border-instRule sm:grid-cols-4">
        {(quote
          ? ([
              ["taking", `${quote.qty.toLocaleString()} lots`],
              ["average fill", (quote.avg / 100).toFixed(4)],
              ["against mid", `${quote.slip >= 0 ? "+" : ""}${quote.slip.toFixed(3)}%`],
              ["notional", `$${Math.round(quote.notional).toLocaleString()}`],
            ] as const)
          : ([
              ["spread", spread === null ? "n/a" : `${spread} ticks`],
              ["messages", snap.msgs.toLocaleString()],
              ["trades", snap.trades.toLocaleString()],
              ["resting", snap.resting.toLocaleString()],
            ] as const)
        ).map(([k, v]) => (
          <div key={k} className="border-r border-instRule px-6 py-4 last:border-r-0 sm:px-8">
            <div className="clause !text-instInk/35">{k}</div>
            <div
              className={`mt-1 font-mono text-sm tabular-nums ${
                quote ? (sweep?.side === "bid" ? "text-bidLit" : "text-askLit") : ""
              }`}
            >
              {v}
            </div>
          </div>
        ))}
      </div>

      {/* The engine's actual output, unedited. */}
      <div className="border-t border-instRule px-6 py-4 sm:px-8">
        <div className="clause !text-instInk/35">last event emitted</div>
        <div className="mt-1 overflow-x-auto whitespace-pre font-mono text-[11px] text-instInk/70">
          {snap.last || (ready ? "…" : "loading WebAssembly")}
        </div>
      </div>
    </div>
  );
}
