package event

import "testing"

func TestRoundTripAndLayout(t *testing.T) {
	e := Event{Type: BookLevel, Side: SideAsk, SymbolID: 7, Flags: FlagGap, ExchangeTsNs: 1, RecvTsNs: 2,
		Price: 6543210, Qty: -5, Seq: 99}
	var b [Size]byte
	for i := range b {
		b[i] = 0xAA // reserved bytes must be cleared, not left as garbage
	}
	e.Encode(b[:])
	got, err := Decode(b[:])
	if err != nil || got != e {
		t.Fatalf("round trip: %+v, %v", got, err)
	}
	// Spot-check offsets against shared/event_format.md.
	if b[0] != 1 || b[1] != 1 || b[2] != 7 || b[24] != byte(6543210&0xff) || b[40] != 99 {
		t.Fatalf("layout mismatch: % x", b[:48])
	}
	for _, x := range b[48:] {
		if x != 0 {
			t.Fatal("reserved bytes not zeroed")
		}
	}
}

func TestEncodeDoesNotAllocate(t *testing.T) {
	e := Event{Type: Trade, Price: 1, Qty: 1}
	var b [Size]byte
	if n := testing.AllocsPerRun(1000, func() { e.Encode(b[:]) }); n != 0 {
		t.Fatalf("Encode allocates %v times", n)
	}
}
