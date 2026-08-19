#include "pricetime/sharded.hpp"

#include <map>
#include <utility>

namespace pricetime {

struct ShardedReplay::Shard {
  // Books owned exclusively by this shard. Nothing else may touch them.
  std::map<std::pair<VenueId, SymbolId>, std::unique_ptr<Book>> books;
  std::map<std::pair<VenueId, SymbolId>, EventLog>              logs;
  std::map<std::pair<VenueId, SymbolId>, BboSlot>               bbos;
  std::vector<const ShardedMsg*> work;
  ShardStats stats;
};

ShardedReplay::ShardedReplay(std::uint32_t shards, Price floor_px,
                             Price ceil_px)
    : n_shards_(shards == 0 ? 1u : shards), floor_(floor_px), ceil_(ceil_px) {
  shards_.reserve(n_shards_);
  for (std::uint32_t i = 0; i < n_shards_; ++i)
    shards_.push_back(std::make_unique<Shard>());
}

ShardedReplay::~ShardedReplay() = default;

void ShardedReplay::run(const std::vector<ShardedMsg>& msgs) {
  // Partition. This is serial and deliberately so: routing is cheap, and doing
  // it up front means each worker touches only its own memory afterwards.
  for (auto& sh : shards_) sh->work.clear();
  for (const ShardedMsg& m : msgs) {
    const auto idx = shard_of(m.venue, m.symbol, n_shards_);
    shards_[idx]->work.push_back(&m);
  }

  auto worker = [this](Shard& sh) {
    for (const ShardedMsg* mp : sh.work) {
      const ShardedMsg& m = *mp;
      const auto key = std::make_pair(m.venue, m.symbol);

      auto bit = sh.books.find(key);
      if (bit == sh.books.end()) {
        // Modest per-book pool, NOT the single-symbol default.
        //
        // Book pre-allocates its order pool so the hot path never allocates,
        // and for one busy symbol 65,536 nodes is right. Multiplied across a
        // real symbol universe it is not: 4 venues x 256 symbols is 1,024
        // books, and at ~48 bytes a node that is over 3 GB of pool reserved to
        // process a few thousand messages. It OOM-killed CI.
        //
        // Most symbols are quiet, so each book starts small and alloc_node()
        // grows the ones that turn out to be busy. The cost is a handful of
        // reallocations on the few hot names during warmup; the alternative is
        // reserving for the worst case on every name simultaneously.
        constexpr std::size_t kPerBookOrders = 1u << 10;
        bit = sh.books
                  .emplace(key, std::make_unique<Book>(
                                    floor_, ceil_, SelfTradePolicy::None,
                                    kPerBookOrders))
                  .first;
      }
      Book& book = *bit->second;
      EventLog& log = sh.logs[key];

      const std::size_t before = log.size();
      switch (m.kind) {
        case ShardedMsg::Kind::New:     book.submit(m.nw, log);  break;
        case ShardedMsg::Kind::Cancel:  book.cancel(m.cx, log);  break;
        case ShardedMsg::Kind::Replace: book.replace(m.rp, log); break;
      }
      for (std::size_t i = before; i < log.size(); ++i)
        if (log[i].kind == Event::Kind::Trade) ++sh.stats.trades;
      ++sh.stats.applied;

      // Publish the new top of book for cross-shard readers.
      Bbo b;
      b.bid_px = book.best_bid();
      b.ask_px = book.best_ask();
      b.bid_sz = book.bid_size();
      b.ask_sz = book.ask_size();
      sh.bbos[key].publish(b);
    }
  };

  if (n_shards_ == 1) {
    worker(*shards_[0]);  // no thread at all: the serial reference path
    return;
  }

  std::vector<std::thread> pool;
  pool.reserve(n_shards_);
  for (auto& sh : shards_) pool.emplace_back([&worker, &sh] { worker(*sh); });
  for (auto& t : pool) t.join();
}

const EventLog* ShardedReplay::log_for(VenueId v, SymbolId s) const {
  const auto idx = shard_of(v, s, n_shards_);
  const auto it = shards_[idx]->logs.find(std::make_pair(v, s));
  return it == shards_[idx]->logs.end() ? nullptr : &it->second;
}

Bbo ShardedReplay::bbo(VenueId v, SymbolId s) const {
  const auto idx = shard_of(v, s, n_shards_);
  const auto it = shards_[idx]->bbos.find(std::make_pair(v, s));
  return it == shards_[idx]->bbos.end() ? Bbo{} : it->second.read();
}

std::uint64_t ShardedReplay::applied() const noexcept {
  std::uint64_t n = 0;
  for (const auto& sh : shards_) n += sh->stats.applied;
  return n;
}

std::uint64_t ShardedReplay::trades() const noexcept {
  std::uint64_t n = 0;
  for (const auto& sh : shards_) n += sh->stats.trades;
  return n;
}

}  // namespace pricetime
