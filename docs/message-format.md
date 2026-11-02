# Message formats

All records are little-endian, fixed layout and trivially copyable. `include/llme/messages.hpp` holds the definitions, with `static_assert`s on size, alignment and every field offset. All reserved and padding bytes are zero (DQ-09).

## Files

A `.mlog` (input) or `.olog` (output) file is one 64-byte `MlogHeader` followed by `record_count` fixed-size records. Files are written with `MlogWriter` and `OlogWriter`, which patch `record_count` on close, so a file from a killed writer fails validation instead of replaying short.

## MlogHeader, 64 bytes

| Offset | Field | Type | Meaning |
|---|---|---|---|
| 0 | `magic` | char[8] | `LLMEMLOG` (input) or `LLMEOLOG` (output) |
| 8 | `version` | u16 | Format version, 1 |
| 10 | `record_size` | u16 | 32 for `.mlog`, 48 for `.olog` |
| 12 | `symbol_id` | u32 | Engine symbol index; the mapping is in the `.meta.json` sidecar |
| 16 | `tick_size_e9` | i64 | Tick size × 10⁹ in price currency; informational |
| 24 | `record_count` | u64 | Must equal (file size − 64) / `record_size` |
| 32 | `seed` | u64 | Generator seed, 0 for replayed data |
| 40 | `source` | u8 | 0 synthetic, 1 book_l3, 2 LOBSTER, 3 ITCH fallback decoder |
| 41 | `reserved` | u8[23] | Zero; readers reject anything else |

`MlogReader::open` rejects a file with the wrong magic, version, record size or record count, or with non-zero reserved bytes. It reports these as `MlogError` values and does not throw.

## InMsg, 32 bytes

| Offset | Field | Type | Meaning |
|---|---|---|---|
| 0 | `type` | u8 | 1 NEW, 2 CANCEL, 3 MODIFY; other values get REJECT `INVALID_TYPE` |
| 1 | `side` | u8 | 0 buy, 1 sell; ignored for CANCEL and MODIFY |
| 2 | `ord_type` | u8 | 0 LIMIT, 1 MARKET; NEW only |
| 3 | `tif` | u8 | 0 GTC, 1 IOC, 2 FOK; MARKET implies IOC behaviour |
| 4 | `qty` | u32 | Lots. NEW: order qty. MODIFY: new leaves, where 0 means cancel |
| 8 | `order_id` | u64 | Unique among live orders; top bit reserved for replay-synthesised aggressors |
| 16 | `price` | i64 | Ticks; ignored for MARKET and CANCEL |
| 24 | `trader_id` | u32 | Owner for self-trade prevention, 0 = anonymous |
| 28 | `reserved` | u32 | Zero |

## OutMsg, 48 bytes

| Offset | Field | Type | Meaning |
|---|---|---|---|
| 0 | `type` | u8 | 1 ACK, 2 REJECT, 3 FILL, 4 CANCELED, 5 MODIFIED, 6 CANCEL_REJECT, 7 MODIFY_REJECT, 8 TRADE, 9 LEVEL_UPDATE |
| 1 | `side` | u8 | Order side (FILL), aggressor side (TRADE), level side (LEVEL_UPDATE) |
| 2 | `reason` | u8 | Reject: 1 INVALID_QTY, 2 PRICE_OUT_OF_BAND, 3 DUPLICATE_ID, 4 POOL_FULL, 5 UNKNOWN_ID, 6 INVALID_TYPE, 7 INVALID_SIDE. Cancel: 1 USER, 2 IOC_REMAINDER, 3 FOK_UNFILLABLE, 4 NO_LIQUIDITY, 5 SELF_TRADE |
| 3 | `flags` | u8 | bit0 aggressor, bit1 priority lost (MODIFIED), bit2 level removed (LEVEL_UPDATE) |
| 4 | `qty` | u32 | Fill qty (FILL, TRADE), aggregate (LEVEL_UPDATE), new leaves (MODIFIED) |
| 8 | `seq` | u64 | Output sequence, starts at 1, gap-free |
| 16 | `order_id` | u64 | Order the report refers to; 0 for LEVEL_UPDATE |
| 24 | `contra_id` | u64 | Resting ID (FILL of aggressor, TRADE); aggressor ID (FILL of resting); 0 otherwise |
| 32 | `price` | i64 | Execution price or level price, in ticks |
| 40 | `leaves` | u32 | Open qty of `order_id` after this event |
| 44 | `reserved` | u32 | Zero |

## Output hash

The deterministic-replay digest is streaming XXH3-64, seed 0, over the raw bytes of every `OutMsg` in `seq` order (`OutputHasher`). It is printed as `0x` plus 16 hex digits.
