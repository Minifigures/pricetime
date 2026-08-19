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
  readonly vol: number;
  readonly resting: number;
  readonly ns: number;
}

interface EngineModule {
  readonly ccall: (
    name: string,
    returnType: string | null,
    argTypes: readonly string[],
    args: readonly unknown[],
  ) => unknown;
}

type ModuleFactory = () => Promise<EngineModule>;

declare global {
  interface Window {
    PricetimeModule?: ModuleFactory;
  }
}

const EMPTY: Snapshot = {
  bids: [], asks: [], tape: [], bid: 0, ask: 0,
  msgs: 0, trades: 0, vol: 0, resting: 0, ns: 0,
};

const px = (p: number): string => (p / 100).toFixed(2);

export function OrderBook(): React.JSX.Element {
  const [snap, setSnap] = useState<Snapshot>(EMPTY);
  const [ready, setReady] = useState(false);
  const [running, setRunning] = useState(true);
  const [error, setError] = useState<string | null>(null);
  const mod = useRef<EngineModule | null>(null);

  useEffect(() => {
    let cancelled = false;
    const script = document.createElement("script");
    script.src = "/pricetime.js";
    script.async = true;
    script.onload = (): void => {
      const factory = window.PricetimeModule;
      if (!factory) {
        setError("engine failed to load");
        return;
      }
      void factory().then((instance) => {
        if (cancelled) return;
        mod.current = instance;
        instance.ccall("pt_reset", null, ["number"], [0xdec0de]);
        setReady(true);
      });
    };
    script.onerror = (): void => setError("engine failed to load");
    document.body.appendChild(script);
    return () => {
      cancelled = true;
      script.remove();
    };
  }, []);

  const tick = useCallback((): void => {
    const m = mod.current;
    if (!m) return;
    m.ccall("pt_step", null, ["number"], [40]);
    const raw = m.ccall("pt_snapshot", "string", [], []);
    if (typeof raw === "string") setSnap(JSON.parse(raw) as Snapshot);
  }, []);

  useEffect(() => {
    if (!ready || !running) return;
    const id = window.setInterval(tick, 80);
    return () => window.clearInterval(id);
  }, [ready, running, tick]);

  const scale = Math.max(
    1,
    ...snap.bids.map((l) => l[1]),
    ...snap.asks.map((l) => l[1]),
  );
  const rows = Array.from({ length: 10 }, (_, i) => 9 - i);
  const spread = snap.bid > 0 && snap.ask > 0 ? snap.ask - snap.bid : null;

  if (error !== null) {
    return (
      <div className="rounded-lg border border-line bg-panel p-6 text-sm text-ask">
        {error}. The engine runs entirely client-side as WebAssembly; a
        content blocker may be preventing it from loading.
      </div>
    );
  }

  return (
    <div className="rounded-lg border border-line bg-panel">
      <div className="flex flex-wrap items-center justify-between gap-3 border-b border-line px-5 py-3">
        <div className="flex items-center gap-2 text-sm">
          <span
            className={`inline-block h-2 w-2 rounded-full ${ready ? "bg-bid" : "bg-neutral-600"}`}
            aria-hidden
          />
          <span className="text-neutral-400">
            {ready ? "engine running in your browser" : "loading WebAssembly…"}
          </span>
        </div>
        <button
          type="button"
          onClick={() => setRunning((r) => !r)}
          disabled={!ready}
          className="rounded border border-line px-3 py-1 text-xs text-neutral-300 hover:border-neutral-500 disabled:opacity-40"
        >
          {running ? "pause" : "resume"}
        </button>
      </div>

      <div className="grid grid-cols-1 gap-px bg-line sm:grid-cols-2">
        <div className="bg-panel p-4">
          <div className="mb-2 text-right text-xs uppercase tracking-wider text-neutral-500">
            bids
          </div>
          {rows.map((i) => {
            const lvl = snap.bids[i];
            return (
              <div key={`b${i}`} className="flex h-5 items-center justify-end gap-2 font-mono text-xs">
                <div className="flex-1 overflow-hidden">
                  <div
                    className="ml-auto h-3 rounded-sm bg-bid/25"
                    style={{ width: lvl ? `${Math.max(2, (lvl[1] / scale) * 100)}%` : "0%" }}
                  />
                </div>
                <span className="w-14 text-right text-neutral-500">{lvl ? lvl[1] : ""}</span>
                <span className="w-16 text-right text-bid">{lvl ? px(lvl[0]) : ""}</span>
              </div>
            );
          })}
        </div>

        <div className="bg-panel p-4">
          <div className="mb-2 text-xs uppercase tracking-wider text-neutral-500">asks</div>
          {rows.map((i) => {
            const lvl = snap.asks[i];
            return (
              <div key={`a${i}`} className="flex h-5 items-center gap-2 font-mono text-xs">
                <span className="w-16 text-ask">{lvl ? px(lvl[0]) : ""}</span>
                <span className="w-14 text-neutral-500">{lvl ? lvl[1] : ""}</span>
                <div className="flex-1 overflow-hidden">
                  <div
                    className="h-3 rounded-sm bg-ask/25"
                    style={{ width: lvl ? `${Math.max(2, (lvl[1] / scale) * 100)}%` : "0%" }}
                  />
                </div>
              </div>
            );
          })}
        </div>
      </div>

      <div className="grid grid-cols-2 gap-px border-t border-line bg-line sm:grid-cols-4">
        {[
          ["spread", spread === null ? "—" : `${spread} ticks`],
          ["messages", snap.msgs.toLocaleString()],
          ["trades", snap.trades.toLocaleString()],
          ["resting orders", snap.resting.toLocaleString()],
        ].map(([label, value]) => (
          <div key={label} className="bg-panel px-4 py-3">
            <div className="text-[10px] uppercase tracking-wider text-neutral-500">{label}</div>
            <div className="font-mono text-sm text-neutral-200">{value}</div>
          </div>
        ))}
      </div>

      <div className="border-t border-line px-5 py-3">
        <div className="mb-2 text-[10px] uppercase tracking-wider text-neutral-500">tape</div>
        <div className="flex flex-wrap gap-x-4 gap-y-1 font-mono text-xs text-neutral-400">
          {snap.tape.length === 0 ? (
            <span className="text-neutral-600">no prints yet</span>
          ) : (
            snap.tape.map((t, i) => (
              <span key={`${t[0]}-${t[1]}-${i}`}>
                <span className="text-neutral-200">{t[1]}</span> @ {px(t[0])}
              </span>
            ))
          )}
        </div>
      </div>
    </div>
  );
}
