# Risk architecture

The rule this design enforces: **no order reaches the exchange without passing
`RiskEngine::check`, and there is exactly one thread that can call it.**

## Where risk sits

```
market event → book → features → model → strategy ─┐
                                                    ▼
                                    RiskEngine::check(intent, book, position, now)
                                                    │
                                    Ok ─────────────┼──→ PaperExchange::submit
                                    any other reason └──→ rejected, counted by reason
```

The strategy cannot reach the exchange object itself. The engine owns both,
and only `Engine::on_event` connects them, through `check`.

## Checks, in order

Cheapest and most fundamental first. The first failing check is the reason recorded.

| # | Check | Rejects when | Why it exists |
|---|---|---|---|
| 1 | Kill switch | latched, unless the order is marked reducing *and* reduces exposure without flipping sign | stop means stop; flattening is the only thing allowed |
| 2 | Book valid | crossed book, sequence gap, no snapshot yet | prices from a wrong book make every later check meaningless |
| 3 | Order size | qty ≤ 0 or > `max_order_qty` | fat-finger on quantity |
| 4 | Position | resulting \|position\| > `max_position` (non-reducing orders) | inventory limit |
| 5 | Notional | resulting \|position\| × mid > `max_notional` | position limit in money, not lots, so it holds when price moves |
| 6 | Price collar | limit more than `price_collar_bps` from mid | fat-finger on price, stale-quote protection |
| 7 | Rate limit | token bucket at `max_orders_per_sec`, refilled from the event clock | runaway loop protection; event clock keeps replays deterministic |

## Kill switch

Trips on:
- **Drawdown:** PnL marked to mid (net of fees) falls more than `max_drawdown` below its high-water mark.
- **Operator:** `redis-cli PUBLISH hft:control kill`. The gateway turns it into a `KillSwitch` event *in the market-data stream*, so the engine handles it in order, on its own thread, with no extra input to poll.

Once tripped:
1. Every new non-reducing order is rejected.
2. The engine sends reducing IOC orders (capped at `max_order_qty`) on each book event while it has a position and nothing in flight, until flat.
3. It stays latched. Only a restart clears it, and restarting is a human decision.

## Concurrency: why there are no races in order placement

All mutable trading state (book, features, position, cash, risk counters,
pending orders) is owned by the engine thread. The other two threads touch it
only through two SPSC rings:

- feed → engine: market events, by value
- engine → writer: output records, by value

There are no shared pointers, no mutexes, and no atomics outside the rings.
The ring's ordering (release on publish, acquire on observe) is tested by a
2-million-item two-thread test that also runs under ThreadSanitizer
(`make tsan`). A data race in order placement would need a second thread that
can see an order, and there isn't one.

## Backpressure without silent data loss

| Where | If it's full | Consequence |
|---|---|---|
| Gateway → engine socket buffer | flag next event `GAP`, resnapshot | engine invalidates book, rejects with `book_invalid` until the snapshot lands |
| Feed → engine ring | feed spins | never drops market data |
| Engine → writer ring | record dropped, counted | logs incomplete; trading unaffected |

## Tested

`engine/tests/test_main.cpp`: each rejection reason in isolation; kill switch latching and the reducing-only rule (including the sign-flip case); drawdown trip; rate limit refill on the event clock; engine never exceeds `max_position` under a model that always says buy; kill → flat → no further orders; a gap blocks trading until the next snapshot.
