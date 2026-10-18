# MarketEvent wire format

The one contract shared by the Go gateway (writer), the C++ engine (reader)
and the Python research code (reader/writer). Fixed 64 bytes, little-endian,
no padding surprises: the C++ struct has a `static_assert(sizeof == 64)` and
Python uses an explicit `numpy.dtype`.

| Offset | Size | Field          | Type   | Notes |
|-------:|-----:|----------------|--------|-------|
| 0      | 1    | type           | u8     | 1 book level, 2 trade, 3 snapshot begin, 4 snapshot end, 5 kill switch, 6 heartbeat |
| 1      | 1    | side           | u8     | book: 0 bid, 1 ask · trade: aggressor 0 buy, 1 sell |
| 2      | 2    | symbol_id      | u16    | index into config/symbols |
| 4      | 4    | flags          | u32    | bit 0: gap detected before this event |
| 8      | 8    | exchange_ts_ns | u64    | exchange event time |
| 16     | 8    | recv_ts_ns     | u64    | gateway receive time (CLOCK_REALTIME) |
| 24     | 8    | price          | i64    | integer ticks (price / tick_size) |
| 32     | 8    | qty            | i64    | integer lots (qty / lot_size); 0 on a book level = remove level |
| 40     | 8    | seq            | u64    | exchange update id |
| 48     | 16   | reserved       | -      | zero |

Prices and quantities are integers end to end. The gateway parses the
exchange's decimal strings directly into ticks and lots without going through
a float, so `65432.10` never becomes `65432.099999`.

One cache line per event: the SPSC ring moves whole lines, and an event never
straddles two.
