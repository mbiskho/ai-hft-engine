// Package sink delivers encoded events to the C++ engine over a Unix socket.
//
// The exchange read loop must never block on the engine. Events go into a
// bounded channel; a writer goroutine drains it. If the channel is full, the
// engine is behind. We don't silently drop market data: the sink records the
// overflow and calls onOverflow, which makes the feed resnapshot, and the
// next event carries FlagGap so the engine invalidates its book.
package sink

import (
	"errors"
	"log/slog"
	"net"
	"os"
	"sync/atomic"

	"github.com/mbiskho/ai-hft-engine/gateway/internal/event"
)

type Unix struct {
	path       string
	ch         chan event.Event
	log        *slog.Logger
	onOverflow func()
	overflow   atomic.Bool
	Dropped    atomic.Uint64
	Sent       atomic.Uint64
}

func NewUnix(path string, buffer int, log *slog.Logger, onOverflow func()) *Unix {
	return &Unix{path: path, ch: make(chan event.Event, buffer), log: log, onOverflow: onOverflow}
}

// Offer enqueues without blocking.
func (u *Unix) Offer(e event.Event) {
	if u.overflow.Swap(false) {
		e.Flags |= event.FlagGap
	}
	select {
	case u.ch <- e:
	default:
		u.Dropped.Add(1)
		if !u.overflow.Swap(true) && u.onOverflow != nil {
			u.onOverflow()
		}
	}
}

// Serve accepts one engine connection at a time and streams events to it.
func (u *Unix) Serve(done <-chan struct{}) error {
	_ = os.Remove(u.path)
	ln, err := net.Listen("unix", u.path)
	if err != nil {
		return err
	}
	go func() { <-done; ln.Close() }()
	buf := make([]byte, event.Size*256)
	for {
		conn, err := ln.Accept()
		if err != nil {
			if errors.Is(err, net.ErrClosed) {
				return nil
			}
			return err
		}
		u.log.Info("engine connected")
		u.stream(conn, buf, done)
		u.log.Warn("engine disconnected")
	}
}

func (u *Unix) stream(conn net.Conn, buf []byte, done <-chan struct{}) {
	defer conn.Close()
	for {
		// Block for one event, then batch whatever else is already queued
		// into a single write: one syscall per burst, not per event.
		var e event.Event
		select {
		case e = <-u.ch:
		case <-done:
			return
		}
		n := 0
		e.Encode(buf[n:])
		n += event.Size
	batch:
		for n+event.Size <= len(buf) {
			select {
			case e = <-u.ch:
				e.Encode(buf[n:])
				n += event.Size
			default:
				break batch
			}
		}
		if _, err := conn.Write(buf[:n]); err != nil {
			return
		}
		u.Sent.Add(uint64(n / event.Size))
	}
}
