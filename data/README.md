# data/

Market data is **not committed**. Run `scripts/fetch_iex.sh` to download it.

## IEX DEEP+ (DPLS)

Order-by-order (L3 / MBO) historical data from the Investors Exchange: every
displayed order added, modified, deleted, and executed, each with a real
exchange-assigned Order ID.

> Data provided for free by IEX. By accessing or using IEX Historical Data, you
> agree to the [IEX Historical Data Terms of Use](https://iextrading.com/iex-historical-data-terms/).

IEX's terms permit redistribution provided that attribution accompanies it,
which is why this project uses IEX rather than the alternatives. It is, as far
as this project could establish, the only free US-equities source where that is
true.

## Why not the other sources

Checked and rejected, with reasons, because "which data can I legally use" is
most of the work:

| Source | Why not |
|---|---|
| **LOBSTER samples** | Terms rewritten 2026-08-14. Clause 5.1(b) now prohibits publishing "results, tables, figures, statistics, screenshots, or findings derived from the Data" without a paid licence, and 5.1(g) bars using it to **benchmark**. The files are still downloadable, which makes this a trap rather than an option. |
| **Nasdaq TotalView-ITCH 5.0** | Better data, and freely downloadable from `emi.nasdaq.com` with no signup. But Nasdaq's terms permit only "one unaltered permanent copy ... for personal and non-commercial use". Download-script-only; nothing derived from it is published here. |
| **Binance Data Vision** | Publishes no order book. `bookDepth` is ten percentage buckets sampled every 30 seconds, not a book. `bookTicker` was discontinued in 2024. |
| **Polygon / Alpaca / Finnhub / Tiingo** | No order-book depth on any free tier. Top-of-book quotes only. |
| **Databento** | Real MBO, but redistribution sits behind the $1,750/month tier. |
| **Zenodo Hyperliquid L4** | Genuinely open (CC-BY-4.0) and excellent, but 195 GB total with a 6.3 GB minimum file, served slowly. A good week-two upgrade. |
| **FI-2010** | Normalized feature vectors, not messages. Nothing to replay. |
| **SEC MIDAS** | Public domain but daily aggregates. Useful to calibrate synthetic flow, not to replay. |
