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
