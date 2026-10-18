package binance

import (
	"context"
	"encoding/json"
	"fmt"
	"log/slog"
	"net/http"
	"strings"
	"sync/atomic"
	"time"

	"github.com/gorilla/websocket"

	"github.com/mbiskho/ai-hft-engine/gateway/internal/decimal"
	"github.com/mbiskho/ai-hft-engine/gateway/internal/event"
)

// Feed streams one symbol's depth and trades as normalized events.
type Feed struct {
	Symbol   string // e.g. BTCUSDT
	SymbolID uint16
	Tick     string // e.g. "0.01"
	Lot      string // e.g. "0.00001"
	WSBase   string // wss://stream.binance.com:9443
	RESTBase string // https://api.binance.com
	Log      *slog.Logger

	syncer Syncer
	resync atomic.Bool
}

// RequestResync makes the running session discard its book and fetch a new
// snapshot. Called when a downstream consumer fell behind and lost events.
func (f *Feed) RequestResync() { f.resync.Store(true) }

type wsEnvelope struct {
	Stream string          `json:"stream"`
	Data   json.RawMessage `json:"data"`
}

type wsDepth struct {
	E    int64       `json:"E"`
	U    uint64      `json:"U"`
	Last uint64      `json:"u"`
	Bids [][2]string `json:"b"`
	Asks [][2]string `json:"a"`
}

type wsTrade struct {
	T     int64  `json:"T"`
	Price string `json:"p"`
	Qty   string `json:"q"`
	Maker bool   `json:"m"` // buyer is maker -> aggressor sold
	ID    uint64 `json:"t"`
}

type restDepth struct {
	LastUpdateID uint64      `json:"lastUpdateId"`
	Bids         [][2]string `json:"bids"`
	Asks         [][2]string `json:"asks"`
}

// Run connects and emits events until ctx ends, reconnecting on failure.
// emit must not block for long: it runs on the socket read loop.
func (f *Feed) Run(ctx context.Context, emit func(event.Event)) error {
	backoff := 500 * time.Millisecond
	for ctx.Err() == nil {
		err := f.session(ctx, emit)
		if ctx.Err() != nil {
			return nil
		}
		f.Log.Warn("feed session ended; reconnecting", "err", err, "in", backoff)
		select {
		case <-time.After(backoff):
		case <-ctx.Done():
			return nil
		}
		backoff = min(backoff*2, 10*time.Second)
	}
	return nil
}

func (f *Feed) session(ctx context.Context, emit func(event.Event)) error {
	s := strings.ToLower(f.Symbol)
	url := fmt.Sprintf("%s/stream?streams=%s@depth@100ms/%s@trade", f.WSBase, s, s)
	conn, _, err := websocket.DefaultDialer.DialContext(ctx, url, nil)
	if err != nil {
		return err
	}
	defer conn.Close()
	go func() { <-ctx.Done(); conn.Close() }()

	f.syncer = Syncer{}
	var buffered []DepthDiff
	snapshotDone := make(chan restDepth, 1)
	snapshotErr := make(chan error, 1)
	requestSnapshot := func() {
		go func() {
			snap, err := f.snapshot(ctx)
			if err != nil {
				snapshotErr <- err
				return
			}
			snapshotDone <- snap
		}()
	}
	requestSnapshot() // step 2, while step 1 buffers
	gapPending := false

	for {
		select {
		case snap := <-snapshotDone:
			f.syncer.Snapshot(snap.LastUpdateID)
			if err := f.emitSnapshot(snap, gapPending, emit); err != nil {
				return err
			}
			gapPending = false
			for _, d := range buffered {
				if a, err := f.apply(d, emit); a == Resync {
					return fmt.Errorf("gap while draining buffer: %w", err)
				}
			}
			buffered = buffered[:0]
		case err := <-snapshotErr:
			return fmt.Errorf("snapshot: %w", err)
		default:
		}
		if f.resync.Swap(false) {
			f.syncer = Syncer{} // diffs buffer until the new snapshot lands
			gapPending = true
			requestSnapshot()
		}

		_ = conn.SetReadDeadline(time.Now().Add(30 * time.Second))
		_, msg, err := conn.ReadMessage()
		if err != nil {
			return err
		}
		recv := uint64(time.Now().UnixNano())
		var env wsEnvelope
		if err := json.Unmarshal(msg, &env); err != nil {
			continue
		}
		switch {
		case strings.HasSuffix(env.Stream, "@trade"):
			var tr wsTrade
			if json.Unmarshal(env.Data, &tr) != nil {
				continue
			}
			px, err1 := decimal.ToUnits(tr.Price, f.Tick)
			q, err2 := decimal.ToUnits(tr.Qty, f.Lot)
			if err1 != nil || err2 != nil {
				continue
			}
			side := event.AggBuy
			if tr.Maker {
				side = event.AggSell
			}
			emit(event.Event{Type: event.Trade, Side: side, SymbolID: f.SymbolID, ExchangeTsNs: uint64(tr.T) * 1e6,
				RecvTsNs: recv, Price: px, Qty: q, Seq: tr.ID})
		case strings.Contains(env.Stream, "@depth"):
			var d wsDepth
			if json.Unmarshal(env.Data, &d) != nil {
				continue
			}
			diff := DepthDiff{FirstID: d.U, FinalID: d.Last, EventMs: d.E, Bids: d.Bids, Asks: d.Asks}
			a, err := f.apply(diff, emit)
			switch a {
			case Buffer:
				buffered = append(buffered, diff)
			case Resync:
				f.Log.Warn("depth gap; resnapshotting", "err", err, "gaps", f.syncer.Gaps())
				gapPending = true // the next snapshot's first event carries FlagGap
				buffered = append(buffered[:0], diff)
				requestSnapshot()
			}
		}
	}
}

func (f *Feed) apply(d DepthDiff, emit func(event.Event)) (Action, error) {
	a, err := f.syncer.Next(d)
	if a != Apply {
		return a, err
	}
	ts := uint64(d.EventMs) * 1e6
	recv := uint64(time.Now().UnixNano())
	for _, side := range []struct {
		levels [][2]string
		s      uint8
	}{{d.Bids, event.SideBid}, {d.Asks, event.SideAsk}} {
		for _, lv := range side.levels {
			px, err1 := decimal.ToUnits(lv[0], f.Tick)
			q, err2 := decimal.ToUnits(lv[1], f.Lot)
			if err1 != nil || err2 != nil {
				f.Log.Error("bad level from exchange", "price", lv[0], "qty", lv[1])
				continue
			}
			emit(event.Event{Type: event.BookLevel, Side: side.s, SymbolID: f.SymbolID, ExchangeTsNs: ts,
				RecvTsNs: recv, Price: px, Qty: q, Seq: d.FinalID})
		}
	}
	return Apply, nil
}

func (f *Feed) emitSnapshot(s restDepth, gap bool, emit func(event.Event)) error {
	now := uint64(time.Now().UnixNano())
	var flags uint32
	if gap {
		flags = event.FlagGap
	}
	emit(event.Event{Type: event.SnapshotBegin, SymbolID: f.SymbolID, Flags: flags, ExchangeTsNs: now, RecvTsNs: now, Seq: s.LastUpdateID})
	for _, side := range []struct {
		levels [][2]string
		s      uint8
	}{{s.Bids, event.SideBid}, {s.Asks, event.SideAsk}} {
		for _, lv := range side.levels {
			px, err1 := decimal.ToUnits(lv[0], f.Tick)
			q, err2 := decimal.ToUnits(lv[1], f.Lot)
			if err1 != nil || err2 != nil {
				return fmt.Errorf("snapshot level %v: %v %v", lv, err1, err2)
			}
			emit(event.Event{Type: event.BookLevel, Side: side.s, SymbolID: f.SymbolID, ExchangeTsNs: now, RecvTsNs: now,
				Price: px, Qty: q, Seq: s.LastUpdateID})
		}
	}
	emit(event.Event{Type: event.SnapshotEnd, SymbolID: f.SymbolID, ExchangeTsNs: now, RecvTsNs: now, Seq: s.LastUpdateID})
	return nil
}

func (f *Feed) snapshot(ctx context.Context) (restDepth, error) {
	req, _ := http.NewRequestWithContext(ctx, http.MethodGet,
		fmt.Sprintf("%s/api/v3/depth?symbol=%s&limit=1000", f.RESTBase, f.Symbol), nil)
	resp, err := http.DefaultClient.Do(req)
	if err != nil {
		return restDepth{}, err
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		return restDepth{}, fmt.Errorf("status %d", resp.StatusCode)
	}
	var d restDepth
	return d, json.NewDecoder(resp.Body).Decode(&d)
}
