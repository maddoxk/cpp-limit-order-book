# Design

This document walks through how the code is organised and why. Read it top to bottom once,
then keep `include/lob/order_book.hpp` open next to it.

## 1. Vocabulary

- **Tick**: smallest price increment. Prices are `int32_t` tick counts, so equal prices compare
  exactly and can be used as keys. No floating point anywhere in the book.
- **Level**: all resting orders at one price on one side. **Side**: bids (buys) or asks (sells).
- **Best price**: highest bid / lowest ask. **Crossing**: a buy at price >= best ask (or a sell at <= best bid).
- **Maker / taker**: the resting order is the maker, the incoming order the taker.
- **Price-time priority**: better price first; at equal price, earlier arrival first.

## 2. Matching rules (what the code guarantees)

| Operation | Behaviour |
|---|---|
| `add_limit` | Match while the best opposite level crosses the limit, oldest order first; rest the remainder at the back of its level's queue. |
| `add_market` | Match at any price; any remainder is discarded (never rests). |
| `cancel` | Remove a resting order; unknown/filled/cancelled ids return `UnknownId`. |
| `modify` | Same price and smaller qty: reduce in place, **keeps priority**. Same price and same qty: no-op. Larger qty or new price: cancel + re-enter with the same id, **loses priority**, and may match immediately. Qty 0 is rejected. |
| `execute` | Feed-reported fill of 1..resting qty against a resting order; reduces in place (keeps priority), records a trade with `taker_id == 0`. |

Trades print at the **maker's** price, so an aggressive buy at 105 against an ask at 100 trades at 100.
Rejected operations never change the book. Why does a size increase lose priority? Otherwise
someone could join the queue with 1 share and later grow to 10,000 while ahead of everyone; a decrease
cannot hurt the people queued behind, so it is safe to keep priority.

## 3. Order book data structures

```
bids_.levels (sorted by key ascending, best = back)      orders_ (pool, std::vector<Order>)
 [ 98 | 99 | 100 ]                                        idx  id  price qty prev next
                    \                                      0   11   100   5  nil  3
                     level 100: head=0 -> 3 -> 7 <-tail    3   14   100   2   0   7
                                                           7   19   100   9   3  nil
index_: id -> pool idx   (unordered_map)
```

### Orders: a pooled intrusive FIFO

All orders live in one `std::vector<Order>`. Each order stores `prev`/`next` as **32-bit indices** into
that same vector, forming a doubly linked list per price level (the "intrusive" part: the links live
inside the order, not in separately allocated list nodes). Why indices and not pointers?

- the vector can grow (reallocate) without invalidating any links;
- 4-byte links keep `Order` small (32 bytes, two per cache line);
- no per-order `new`/`delete`: a freed slot goes onto a free list (reusing its `next` field) and is reused.

A doubly linked list gives O(1) removal from the middle (cancel), O(1) append (new order) and O(1)
pop-from-front (matching).

### Levels: a sorted flat vector, best at the back

Each side keeps `std::vector<Level>` sorted by a key (bids: price; asks: negated price) so **the best
level is always `levels.back()`**. Negating the ask price lets both sides share one comparison.

Compared with `std::map<Price, Queue>`:

| | flat sorted vector (used here) | `std::map` |
|---|---|---|
| Access best level | `back()`: O(1), contiguous memory | `begin()`/`rbegin()`: O(1) but pointer chasing |
| Match (consume best levels) | pop from the back: O(1) | erase node: O(log n) + free |
| Find a level (cancel, add) | binary search over contiguous memory: O(log n), cache friendly | tree walk: O(log n), a cache miss per node |
| Insert/erase a *new* level | O(distance from best) element shifts (memmove) | O(log n) |
| Memory | one allocation | one allocation per level |

Real books are dominated by activity near the touch and have few active levels (tens to a few hundred),
so the shift cost for levels near the best price is tiny and the contiguous layout wins on cache
behaviour. The honest downside: a pathological workload creating/erasing levels far from the best price
in a book with thousands of levels pays O(n) per such operation. I did not benchmark `std::map` side by
side, so the "wins" above is reasoning about memory behaviour, not a measured claim. An alternative
for bounded price ranges is a direct-indexed array of levels (price - base); it avoids the search
altogether but needs a price band and wastes memory when sparse.

Orders never store a pointer or index to their level (it would be invalidated whenever the level vector shifts); they find it with a binary search on their price (`remove_order`,
`reduce_in_place`). That costs O(log levels) per cancel but keeps the structures independent.

### Order id index

`std::unordered_map<OrderId, uint32_t>` maps an id to its pool slot, giving O(1) average cancel/modify.
It is the simplest correct choice and the main remaining cache-unfriendly part (node-based buckets).
A flat open-addressing table would likely be faster; that is listed as a limitation rather than done.

### Trades

Every mutating call clears and refills an internal `std::vector<Trade>`; callers read it through
`last_trades()`. This avoids allocating or calling back into user code on the hot path.

## 4. Binary feed

Inspired by Nasdaq ITCH (fixed-size, type-prefixed, little-endian here), not compatible with it.

```
Add     'A'  type(1) side(1) order_type(1) id(8) price(4) qty(4)   19 bytes
Cancel  'X'  type(1) id(8)                                          9 bytes
Modify  'M'  type(1) id(8) price(4) qty(4)                         17 bytes
Execute 'E'  type(1) id(8) qty(4)                                  13 bytes
File:   "LOBF" + u32 version, then messages back to back
```

Because each type has a fixed size, a decoder needs no length prefix: read the type byte, look up the size,
check enough bytes remain. `decode` returns `Ok`, `NeedMore` (truncated), `BadType` or `BadField` and
never reads past the span it is given. Integers are assembled byte by byte, so there is no
alignment, padding or endianness assumption (and no undefined behaviour from casting byte buffers to structs).

**Generator** (`FeedGenerator`): seeded `std::mt19937_64`. Mid price takes a +/-1 tick step with probability
0.1 per message. Passive limit orders sit `1 + Exp(4)` ticks on the passive side of the mid; 5% are
aggressive. Sizes are `1 + Exp(30)`. Cancels, modifies and executions pick a random order that is really resting,
which the generator knows because it applies every message to a private shadow `OrderBook`. A fresh book
gets 500 warm-up passive orders first. It is a toy model: real order flow has bursts, clustering and
much heavier tails.

**Replay** (`replay_bytes`): decode, `apply` to the book, accumulate `ReplayStats`. The stats include a
FNV-1a hash over every trade in order; two runs with identical trade sequences get identical hashes,
which is how the tests check determinism and that the threaded pipeline is equivalent to the single-thread one.

## 5. The SPSC ring buffer

`SpscQueue<T>` holds a power-of-two array and two counters. `tail_` (producer) counts pushes, `head_`
(consumer) counts pops; slot = counter & mask; full when `tail - head == capacity`; empty when `head == tail`.
Counters only grow (64-bit), so there is no ambiguity between "full" and "empty".

Why this is correct without locks - each counter has exactly one writer:

1. Producer writes the element, then `tail_.store(release)`.
2. Consumer `tail_.load(acquire)`; if it sees the new tail it is guaranteed to see the element write.
3. Consumer reads the element, then `head_.store(release)`.
4. Producer `head_.load(acquire)` before reusing a slot, so it never overwrites an element still being read.

Each thread reads its *own* counter with `relaxed` ordering. Two performance touches: `head_` and `tail_`
are on different cache lines (avoids false sharing), and each side caches the last value it saw of the
other's counter and only reloads the shared atomic when the cache says full/empty.
End of stream is signalled with a separate `std::atomic<bool>` set after the last push; the consumer
drains the queue once more after seeing it, because pushes can land between its failed pop and its read of the flag.
The `tsan` CI job runs the two-thread tests so the ordering is machine-checked for the tested interleavings
(TSan finds races that occur in the run, it does not prove their absence).

## 6. Testing strategy

1. **Example tests** pin down each rule with a tiny scenario (priority, sweeps, modify rules, validation).
2. **Property test**: a small `ReferenceBook` (tests/reference_book.hpp) using `std::map<Price, std::deque<Order>>` and linear scans is
   obviously correct and shares no code with the real book. The same seeded random stream of
   limit/market/cancel/modify/execute/invalid operations (prices in a narrow band to force crossings, and a
   wide band to exercise level insert/erase) is applied to both and every result, trade list, top of book,
   and (periodically) full depth must match, and the book must never end up crossed.
3. **Codec tests**: round trips, exact bytes, truncation at every prefix length, corrupt bytes.
4. **Pipeline test**: single-thread and two-thread replay must give identical `ReplayStats`, including with
   tiny queues (capacity 2) that force constant full/empty handling.
5. **Sanitizers**: ASan+UBSan on the whole suite, TSan on the whole suite.

## 7. Things I would do next

- Replace `unordered_map` with an open-addressing table, and measure the difference.
- Benchmark `std::map` vs the flat vector under different level counts instead of reasoning about it.
- Batch messages through the SPSC queue (the two-thread pipeline is currently slower than one thread).
- Self-trade prevention, IOC/FOK, multiple symbols.
