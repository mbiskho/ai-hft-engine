package questdb

import (
	"testing"

	"github.com/mbiskho/ai-hft-engine/gateway/internal/event"
)

func TestAppendLine(t *testing.T) {
	got := string(AppendLine(nil, "BTCUSDT", event.Event{Type: event.BookLevel, Side: event.SideAsk,
		Price: 6543210, Qty: 1200, Seq: 77, ExchangeTsNs: 1700000000000000000}))
	want := "book,symbol=BTCUSDT,side=ask price=6543210i,qty=1200i,seq=77i 1700000000000000000\n"
	if got != want {
		t.Fatalf("got  %q\nwant %q", got, want)
	}
	if s := AppendLine(nil, "X", event.Event{Type: event.Heartbeat}); len(s) != 0 {
		t.Fatal("heartbeats must not be written")
	}
}
