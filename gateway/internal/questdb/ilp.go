// Package questdb writes ticks to QuestDB over the InfluxDB Line Protocol
// (TCP 9009) for research queries and dashboards. This is the analytics path,
// not the trading path: if QuestDB is slow or down, lines are dropped and
// counted, and the engine never notices.
package questdb

import (
	"bufio"
	"log/slog"
	"net"
	"strconv"
	"sync/atomic"
	"time"

	"github.com/mbiskho/ai-hft-engine/gateway/internal/event"
)

type Writer struct {
	addr    string
	symbol  string
	ch      chan event.Event
	log     *slog.Logger
	Dropped atomic.Uint64
}

func New(addr, symbol string, log *slog.Logger) *Writer {
	return &Writer{addr: addr, symbol: symbol, ch: make(chan event.Event, 1<<16), log: log}
}

func (w *Writer) Offer(e event.Event) {
	select {
	case w.ch <- e:
	default:
		w.Dropped.Add(1)
	}
}

// AppendLine formats one ILP line into b. Exported for tests.
func AppendLine(b []byte, symbol string, e event.Event) []byte {
	switch e.Type {
	case event.Trade:
		b = append(b, "trades,symbol="...)
		b = append(b, symbol...)
		if e.Side == event.AggBuy {
			b = append(b, ",aggressor=buy"...)
		} else {
			b = append(b, ",aggressor=sell"...)
		}
	case event.BookLevel:
		b = append(b, "book,symbol="...)
		b = append(b, symbol...)
		if e.Side == event.SideBid {
			b = append(b, ",side=bid"...)
		} else {
			b = append(b, ",side=ask"...)
		}
	default:
		return b
	}
	b = append(b, " price="...)
	b = strconv.AppendInt(b, e.Price, 10)
	b = append(b, "i,qty="...)
	b = strconv.AppendInt(b, e.Qty, 10)
	b = append(b, "i,seq="...)
	b = strconv.AppendUint(b, e.Seq, 10)
	b = append(b, "i "...)
	b = strconv.AppendUint(b, e.ExchangeTsNs, 10)
	return append(b, '\n')
}

func (w *Writer) Run(done <-chan struct{}) {
	var line []byte
	for {
		conn, err := net.DialTimeout("tcp", w.addr, 3*time.Second)
		if err != nil {
			w.log.Warn("questdb unavailable; dropping analytics lines", "err", err)
			select {
			case <-time.After(5 * time.Second):
				continue
			case <-done:
				return
			}
		}
		bw := bufio.NewWriterSize(conn, 1<<16)
		flush := time.NewTicker(250 * time.Millisecond)
	loop:
		for {
			select {
			case e := <-w.ch:
				line = AppendLine(line[:0], w.symbol, e)
				if _, err := bw.Write(line); err != nil {
					break loop
				}
			case <-flush.C:
				if err := bw.Flush(); err != nil {
					break loop
				}
			case <-done:
				_ = bw.Flush()
				flush.Stop()
				conn.Close()
				return
			}
		}
		flush.Stop()
		conn.Close()
	}
}
