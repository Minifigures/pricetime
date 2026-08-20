#!/usr/bin/env python3
"""Normalize live order data from three exchanges into pricetime's feed protocol.

    ./scripts/feed_crypto.py [seconds] | ./build/pricetime_nbbo

Why crypto and not equities. A genuine cross-venue NBBO needs two or more
venues quoting the SAME instrument at the SAME time. The free US equity
order-by-order archives do not overlap: IEX DEEP+ starts 2024-10-01 and the
free Nasdaq ITCH samples are 2019 and 2025-26. Crypto exchanges all quote
BTC/USD concurrently and publish it without authentication, so this is the
honest way to get a real NBBO rather than a synthetic one.

Venue roles, and the distinction matters:

  BITSTAMP  order-by-order (L3). Every individual order add, change and delete
            with a real exchange order ID, so pricetime rebuilds the actual
            book and derives its BBO from reconstructed depth. This is the one
            that exercises the matching engine.

  COINBASE  top-of-book only, from the ticker channel.
  KRAKEN    top-of-book only, from the ticker channel.

The two quote venues are NOT reconstructed books and the display says so.
Claiming depth that was never sent would be the easy lie here.

All three quote BTC against actual USD. Binance is deliberately excluded: its
liquid pair is BTC/USDT, which is a different instrument, and mixing it into a
USD NBBO would be wrong in a way that is invisible on the screen.

No API keys. No signup. Nothing is redistributed: this streams live and prints
to stdout.
"""
import asyncio, json, sys, time, urllib.request

try:
    import websockets
except ImportError:
    sys.exit("needs `pip install websockets` (python3 -m pip install --user websockets)")

# Prices are integer ticks of $0.01. Sizes are integer units of 1e-4 BTC.
def px(v) -> int:
    return int(round(float(v) * 100))

def sz(v) -> int:
    return int(round(float(v) * 10_000))

def now_ms() -> int:
    return int(time.time() * 1000)

out_lock = asyncio.Lock()

async def emit(line: str) -> None:
    async with out_lock:
        sys.stdout.write(line + "\n")
        sys.stdout.flush()


BITSTAMP_SNAPSHOT = "https://www.bitstamp.net/api/v2/order_book/btcusd/?group=2"


def fetch_bitstamp_snapshot():
    """Full order-by-order book: [price, amount, order_id] per level."""
    with urllib.request.urlopen(BITSTAMP_SNAPSHOT, timeout=20) as r:
        return json.loads(r.read().decode())


async def bitstamp(stop: float) -> None:
    """True L3: individual orders with exchange-assigned IDs.

    A live subscription alone is NOT enough, and getting this wrong produces a
    convincing-looking lie. Subscribing mid-session only shows orders created
    after you connect, so the reconstructed book contains a fraction of the
    real depth. Measured: that partial book showed a $2.21 spread while
    Coinbase showed $0.01, and the consolidator then reported thousands of
    crossed markets that were entirely an artifact of the missing depth.

    The fix is the standard snapshot-plus-deltas sequence, in this order:
      1. subscribe and BUFFER deltas, do not apply them yet
      2. fetch the REST snapshot, which carries its own microtimestamp
      3. emit the snapshot as adds
      4. replay buffered deltas, discarding any older than the snapshot
    Doing the snapshot first and subscribing second would drop every change in
    the gap between the two.
    """
    url = "wss://ws.bitstamp.net"
    while time.time() < stop:
        try:
            async with websockets.connect(url, ping_interval=20,
                                          max_size=8 * 1024 * 1024) as ws:
                for ch in ("live_orders_btcusd", "live_trades_btcusd"):
                    await ws.send(json.dumps(
                        {"event": "bts:subscribe", "data": {"channel": ch}}))

                buffered, seeded, snap_ts = [], False, 0
                loop = asyncio.get_running_loop()
                snap_task = loop.run_in_executor(None, fetch_bitstamp_snapshot)

                while time.time() < stop:
                    msg = json.loads(await asyncio.wait_for(ws.recv(), timeout=20))
                    ev, ch, d = msg.get("event"), msg.get("channel", ""), msg.get("data") or {}
                    if ev in ("bts:subscription_succeeded", "bts:request_reconnect"):
                        continue

                    if not seeded:
                        buffered.append((ev, ch, d))
                        if snap_task.done():
                            snap = snap_task.result()
                            snap_ts = int(snap.get("microtimestamp", 0))
                            n = 0
                            for side, key in (("B", "bids"), ("S", "asks")):
                                for row in snap.get(key, []):
                                    if len(row) < 3:
                                        continue
                                    await emit(f"A 0 {int(row[2])} {side} "
                                               f"{px(row[0])} {sz(row[1])}")
                                    n += 1
                            print(f"# bitstamp seeded {n} resting orders from snapshot",
                                  file=sys.stderr)
                            seeded = True
                            replay, buffered = buffered, []
                            for bev, bch, bd in replay:
                                if int(bd.get("microtimestamp", 0)) > snap_ts:
                                    await handle_bitstamp(bev, bch, bd)
                        continue

                    await handle_bitstamp(ev, ch, d)
        except Exception as e:
            print(f"# bitstamp reconnect: {e}", file=sys.stderr)
            await asyncio.sleep(2)


async def handle_bitstamp(ev, ch, d) -> None:
    if "live_trades" in ch and ev == "trade":
        side = "B" if d.get("type", 0) == 0 else "S"
        await emit(f"T 0 {side} {px(d['price'])} {sz(d['amount'])} {now_ms()}")
    elif "live_orders" in ch:
        side = "B" if d.get("order_type", 0) == 0 else "S"
        oid = int(d.get("id", 0))
        if ev == "order_created":
            await emit(f"A 0 {oid} {side} {px(d['price'])} {sz(d['amount'])}")
        elif ev == "order_deleted":
            await emit(f"X 0 {oid}")
        elif ev == "order_changed":
            # Cancel-then-add. Bitstamp does not publish whether the change
            # kept queue priority, and assuming it did would invent a fact.
            await emit(f"X 0 {oid}")
            await emit(f"A 0 {oid} {side} {px(d['price'])} {sz(d['amount'])}")


async def coinbase(stop: float) -> None:
    url = "wss://ws-feed.exchange.coinbase.com"
    while time.time() < stop:
        try:
            async with websockets.connect(url, ping_interval=20) as ws:
                await ws.send(json.dumps({"type": "subscribe",
                                          "product_ids": ["BTC-USD"],
                                          "channels": ["ticker"]}))
                while time.time() < stop:
                    m = json.loads(await asyncio.wait_for(ws.recv(), timeout=20))
                    if m.get("type") != "ticker":
                        continue
                    b, a = m.get("best_bid"), m.get("best_ask")
                    if not b or not a:
                        continue
                    bs = sz(m.get("best_bid_size", 0)); as_ = sz(m.get("best_ask_size", 0))
                    await emit(f"Q 1 {px(b)} {bs} {px(a)} {as_} {now_ms()}")
                    if m.get("last_size") and m.get("price") and sz(m["last_size"]) > 0:
                        side = "B" if m.get("side") == "buy" else "S"
                        await emit(f"T 1 {side} {px(m['price'])} {sz(m['last_size'])} {now_ms()}")
        except Exception as e:
            print(f"# coinbase reconnect: {e}", file=sys.stderr)
            await asyncio.sleep(2)


async def kraken(stop: float) -> None:
    url = "wss://ws.kraken.com/v2"
    while time.time() < stop:
        try:
            async with websockets.connect(url, ping_interval=20) as ws:
                # The ticker channel only publishes when the ticker changes,
                # which was 8 updates in 30 seconds and left the quote 20
                # seconds stale. A stale quote in an NBBO does not look stale,
                # it looks like an arbitrage. The book channel updates on every
                # depth change, which is what a consolidated quote needs.
                await ws.send(json.dumps({"method": "subscribe",
                                          "params": {"channel": "book",
                                                     "symbol": ["BTC/USD"],
                                                     "depth": 10}}))
                best_b = best_a = None
                while time.time() < stop:
                    m = json.loads(await asyncio.wait_for(ws.recv(), timeout=20))
                    if m.get("channel") != "book":
                        continue
                    for d in m.get("data", []):
                        bids, asks = d.get("bids") or [], d.get("asks") or []
                        if bids: best_b = max(bids, key=lambda x: float(x["price"]))
                        if asks: best_a = min(asks, key=lambda x: float(x["price"]))
                        if best_b is None or best_a is None:
                            continue
                        d = {"bid": best_b["price"], "bid_qty": best_b.get("qty", 0),
                             "ask": best_a["price"], "ask_qty": best_a.get("qty", 0)}
                        await emit(f"Q 2 {px(d['bid'])} {sz(d.get('bid_qty', 0))} "
                                   f"{px(d['ask'])} {sz(d.get('ask_qty', 0))} {now_ms()}")
        except Exception as e:
            print(f"# kraken reconnect: {e}", file=sys.stderr)
            await asyncio.sleep(2)


async def main() -> None:
    secs = float(sys.argv[1]) if len(sys.argv) > 1 else 60.0
    stop = time.time() + secs
    await emit("V 0 BITSTAMP book")
    await emit("V 1 COINBASE quote")
    await emit("V 2 KRAKEN quote")
    await asyncio.gather(bitstamp(stop), coinbase(stop), kraken(stop),
                         return_exceptions=True)

if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
