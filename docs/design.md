# Design

## Semantics

This section is normative for every engine. The golden scenarios in `tests/golden/` were written from it before the fast engine existed, and the reference engine in `src/reference/` implements it directly.

Prices are integer ticks and quantities are integer lots. An order is `live` from the moment it rests until it is filled or canceled. A terminal order is forgotten, so its ID can be used again by a later NEW.

### Validation

A NEW is checked in this order. The first failure produces a single `REJECT` and changes nothing.

1. `type` is 1, 2 or 3, otherwise `INVALID_TYPE`. This check applies to every message.
2. `side` is 0 or 1, otherwise `INVALID_SIDE`.
3. `ord_type` is 0 or 1 and `tif` is 0, 1 or 2, otherwise `INVALID_TYPE`.
4. `qty` is not zero, otherwise `INVALID_QTY`.
5. For LIMIT, `price` lies in [`price_min`, `price_max`], otherwise `PRICE_OUT_OF_BAND`.
6. `order_id` is not live, otherwise `DUPLICATE_ID`.
7. For GTC LIMIT, the pool has a free slot, otherwise `POOL_FULL`.

### Order types

- **Limit, GTC.** Matches against the opposite side while its price crosses. Any remainder rests at the tail of its level.
- **IOC.** Matches the same way, then cancels any remainder with `IOC_REMAINDER`, even if nothing filled.
- **FOK.** Fills completely or not at all. The check scans the opposite side from the best price and stops once the quantity is reached. If it fails, the order gets `CANCELED` with `FOK_UNFILLABLE` and the book does not change.
- **Market.** Sweeps from the best price until filled or the side is empty, and never rests. A remainder is canceled with `IOC_REMAINDER`, or `NO_LIQUIDITY` if nothing filled (D-07). `price` is ignored, and `tif` is range-checked but has no other effect.

A trade always executes at the resting order's price.

### Cancel and modify

- **Cancel** removes a live order in O(1) and emits `CANCELED` (`USER`) and `LEVEL_UPDATE`. An unknown or terminal ID gets `CANCEL_REJECT` (`UNKNOWN_ID`).
- **Modify** sets new leaves and a new price on a live order, and always stays GTC. The checks run in this order:
  1. Unknown or terminal ID: `MODIFY_REJECT` (`UNKNOWN_ID`).
  2. New qty 0: treated as a cancel, whatever the price field says.
  3. Price outside the band: `MODIFY_REJECT` (`PRICE_OUT_OF_BAND`), with no state change.
  4. Same price and new qty ≤ leaves: reduce in place and keep queue position. Equal qty is a no-op that still answers `MODIFIED`, with no `LEVEL_UPDATE` because no aggregate changed.
  5. Same price and new qty > leaves: move to the tail of the level, flag priority lost.
  6. Price change: remove from the old level, flag priority lost, match as an aggressor if it now crosses, and rest any remainder at the tail of the new level.

Queue priority rewards the order that committed first. Reducing size gives nothing up to the orders behind it, so it keeps its place. A price change or a size increase is a new commitment, so it goes to the back.

### Output records

Every field not listed is zero. `seq` starts at 1 and increments by one per record.

| Record | `side` | `reason` | `flags` | `qty` | `order_id` | `contra_id` | `price` | `leaves` |
|---|---|---|---|---|---|---|---|---|
| ACK | order | | | order qty | order | | limit price, 0 for market | order qty |
| REJECT | raw `side` byte | reject reason | | | order | | | |
| FILL, resting | resting | | | fill qty | resting | aggressor | execution | resting leaves after |
| FILL, aggressor | aggressor | | aggressor | fill qty | aggressor | resting | execution | aggressor leaves after |
| TRADE | aggressor | | | fill qty | aggressor | resting | execution | aggressor leaves after |
| CANCELED | order | cancel reason | | qty canceled | order | | order price, 0 for market | |
| MODIFIED | order | | priority lost | new leaves | order | | new price | new leaves |
| CANCEL_REJECT, MODIFY_REJECT | | reject reason | | | order | | | |
| LEVEL_UPDATE | level | | removed | new aggregate | | | level | |

`LEVEL_UPDATE.qty` is a u32 while aggregates are u64, so it saturates at 2³²−1. The `removed` flag, never the quantity, says that a level is gone, and `depth_at` returns the exact value.

### Emission order

- Valid NEW: `ACK`; then for each match in priority order `FILL`(resting), `FILL`(aggressor), `TRADE`, `LEVEL_UPDATE`(contra level); then `LEVEL_UPDATE`(own level) if the remainder rests, or `CANCELED` with a reason if it does not.
- Invalid NEW: `REJECT`.
- CANCEL: `CANCELED`, `LEVEL_UPDATE`.
- MODIFY: `MODIFIED`, the `LEVEL_UPDATE` of the old level, then any matches as above, then the `LEVEL_UPDATE` of the new level if a remainder rests.

There is one `LEVEL_UPDATE` per match, even when several matches land on one level.

## Structures

The hot path is `MatchingEngine<LevelStore, Sink>` in `include/llme/engine.hpp`. Both parameters are templates, so there is no virtual call or type erasure between the matching loop and its book or output.

| Structure | File | Cost | Notes |
|---|---|---|---|
| Order pool | `order_pool.hpp` | O(1) allocate and release | Fixed array of 48-byte orders with an index free list. The list is LIFO, so a released slot, still in cache, is reused first. |
| Level FIFO | `order_pool.hpp` | O(1) push and unlink | Intrusive doubly linked list of pool indices, with cached `qty` and `count`. |
| ID map | `id_map.hpp` | O(1) expected | Linear probing with Fibonacci hashing and capacity of at least 2 × `max_orders`. Backward-shift deletion leaves no tombstones, so probe lengths do not degrade over a long run. |
| Price ladder (D2) | `price_ladder.hpp` | O(1) level access; next level by bitmap scan | One `Level` per price per side, plus a bitmap and a summary bitmap. The best price is cached, and the bitmap is searched only when the best level empties. |

All of it is sized and touched in the constructor. At the default config, 2²⁰ levels per side and 2²⁰ orders, the book takes under 256 MiB (NFR-12, checked by a test). When capacity runs out the engine rejects with `POOL_FULL` and never grows.

The output goes to a `Sink` called once per record. The pipeline's sink pushes into the out-ring. A bench sink can discard, so that matching can be timed alone.

## Rings

`SpscRing<T, N>` in `include/llme/spsc_ring.hpp` is a bounded lock-free queue with one producer and one consumer.

- **Indices.** `head` and `tail` are free-running counters, masked on use, so full and empty are distinguishable without giving up a slot.
- **Memory orders.** The producer writes the slot, then stores `tail` with release. The consumer loads `tail` with acquire, so it cannot see the index before the slot's contents. Returning slots is the same pairing on `head`. Each index has a single writer, so sequential consistency would add cost and no guarantee.
- **False sharing.** `head`, `tail` and each side's cached copy of the other's index sit on separate 64-byte lines, and the slots start on their own line. The cached copy means a side touches the shared line only when its cached view says full or empty, so under load most operations stay in the local cache.
- **Proof.** `RingStress.*` moves numbered 32-byte records between two threads: 10M under TSan in CI and 1B in Release, with any loss, duplicate, reorder or torn copy failing the check.

## Replay

The core is single-threaded and sees one totally ordered stream of `InMsg`, so its output is a pure function of the input bytes. Ring timing can change when a record is produced, but never what it contains or where it sits in the stream. Nothing in the output depends on time, addresses, thread scheduling or a hash map's iteration order: pool indices stay inside the engine, and `seq` counts output records.

`OutputHasher` reduces a run to one XXH3-64 digest. AC-W-05 holds that digest fixed across repeated runs, pipeline against in-process, Debug against Release, GCC against Clang, and every engine design.

In production this property is what makes a matching engine recoverable. A standby replaying the same sequenced input reaches the same state, and comparing digests shows that it did. A bug report is a log file that reproduces exactly, which is how `tests/fixtures/regressions/` works here.

## Correctness layers

1. **Golden scenarios.** Hand-written from this document, before either engine ran them.
2. **Reference engine.** Standard containers and no performance work, so it can be checked by reading it.
3. **Differential testing.** `llme-diff` compares every record and the final book against the reference. The generator's shadow book is a reference too, so a defect in a fast engine cannot change its own input.
4. **Invariants.** `check_invariants` in `engine.hpp`, with tests that corrupt the book in each way it must catch.
5. **Fuzzing.** `fuzz_engine_diff` explores order sequences by coverage, on a book small enough to hit empty sides, ID reuse and a full pool all the time.
6. **Sanitizers and the allocation hook.** ASan, UBSan and TSan run the whole suite. The hook proves that the hot path never allocates.

## Real flow

Real order flow reaches the engine as an ordinary `.mlog`. `ReplayBuilder` in `src/workload/replay.cpp` turns source events into requests, and `tools/py/llme_tools/book_l3_to_mlog.py` mirrors it line for line.

| Source event | Engine message |
|---|---|
| Add | NEW GTC limit |
| Partial cancel | MODIFY to the new leaves, which keeps priority |
| Delete | CANCEL |
| Replace | CANCEL, then NEW with the new ID on the same side, which loses priority as ITCH U does |
| Execution | NEW IOC limit on the opposite side at the resting price, with the resting ID in the low 63 bits of its own ID |
| Non-printable execution | MODIFY down: the shares leave the book, but nothing trades |

An execution is emulated, so it hits whatever is at the head of that price in the engine. When the real order was not at the head, because earlier orders are missing from the replay, it hits a different one. `exec_contra_match_rate` measures how often that happens. ITCH replay is a realistic workload, not a venue simulation.

A replay sizes its engine from the sidecar next to the log (DQ-07), and `llme-run` and `llme-diff` read that sidecar automatically.
