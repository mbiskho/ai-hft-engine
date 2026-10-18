// Command gateway: exchange market data in, normalized 64-byte events out.
//
//	gateway -symbol BTCUSDT -tick 0.01 -lot 0.00001 -socket /tmp/hft-gateway.sock \
//	        -record data/btcusdt-$(date +%F).bin -questdb localhost:9009 -redis redis://localhost:6379/0
package main

import (
	"context"
	"flag"
	"log/slog"
	"os"
	"os/signal"
	"syscall"
	"time"

	"github.com/mbiskho/ai-hft-engine/gateway/internal/binance"
	"github.com/mbiskho/ai-hft-engine/gateway/internal/control"
	"github.com/mbiskho/ai-hft-engine/gateway/internal/event"
	"github.com/mbiskho/ai-hft-engine/gateway/internal/questdb"
	"github.com/mbiskho/ai-hft-engine/gateway/internal/recorder"
	"github.com/mbiskho/ai-hft-engine/gateway/internal/sink"
)

func main() {
	symbol := flag.String("symbol", "BTCUSDT", "exchange symbol")
	symbolID := flag.Uint("symbol-id", 0, "id in config/symbols.conf")
	tick := flag.String("tick", "0.01", "tick size")
	lot := flag.String("lot", "0.00001", "lot size")
	socket := flag.String("socket", "/tmp/hft-gateway.sock", "Unix socket the engine connects to (empty = off)")
	record := flag.String("record", "", "append raw events to this file (empty = off)")
	qdb := flag.String("questdb", "", "QuestDB ILP host:port (empty = off)")
	redisURL := flag.String("redis", "", "Redis URL for the kill-switch channel (empty = off)")
	wsBase := flag.String("ws", "wss://stream.binance.com:9443", "websocket base")
	restBase := flag.String("rest", "https://api.binance.com", "REST base")
	flag.Parse()

	log := slog.New(slog.NewJSONHandler(os.Stdout, nil))
	ctx, stop := signal.NotifyContext(context.Background(), syscall.SIGINT, syscall.SIGTERM)
	defer stop()

	feed := &binance.Feed{Symbol: *symbol, SymbolID: uint16(*symbolID), Tick: *tick, Lot: *lot,
		WSBase: *wsBase, RESTBase: *restBase, Log: log}

	var outputs []func(event.Event)

	if *socket != "" {
		// On overflow the engine has missed events and its book is wrong. The
		// sink flags the next event as a gap (engine stops trading) and the
		// feed resnapshots (engine rebuilds and resumes).
		u := sink.NewUnix(*socket, 1<<16, log, func() {
			log.Error("engine too slow: sink overflow; resnapshotting")
			feed.RequestResync()
		})
		go func() {
			if err := u.Serve(ctx.Done()); err != nil {
				log.Error("socket", "err", err)
				stop()
			}
		}()
		outputs = append(outputs, u.Offer)
		go report(ctx, log, u)
	}
	if *record != "" {
		r, err := recorder.Open(*record)
		if err != nil {
			log.Error("record", "err", err)
			os.Exit(1)
		}
		defer r.Close()
		outputs = append(outputs, func(e event.Event) { _ = r.Write(e) })
	}
	if *qdb != "" {
		w := questdb.New(*qdb, *symbol, log)
		go w.Run(ctx.Done())
		outputs = append(outputs, w.Offer)
	}

	emit := func(e event.Event) {
		for _, out := range outputs {
			out(e)
		}
	}

	if *redisURL != "" {
		go func() {
			if err := control.Listen(ctx, *redisURL, uint16(*symbolID), log, emit); err != nil {
				log.Error("control channel", "err", err)
			}
		}()
	}

	log.Info("gateway starting", "symbol", *symbol, "socket", *socket, "record", *record, "questdb", *qdb)
	if err := feed.Run(ctx, emit); err != nil {
		log.Error("feed", "err", err)
		os.Exit(1)
	}
}

func report(ctx context.Context, log *slog.Logger, u *sink.Unix) {
	t := time.NewTicker(10 * time.Second)
	defer t.Stop()
	for {
		select {
		case <-ctx.Done():
			return
		case <-t.C:
			log.Info("sink", "sent", u.Sent.Load(), "dropped", u.Dropped.Load())
		}
	}
}
