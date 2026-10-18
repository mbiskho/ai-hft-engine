// Package event encodes the 64-byte MarketEvent defined in shared/event_format.md.
package event

import (
	"encoding/binary"
	"errors"
)

const Size = 64

type Type uint8

const (
	BookLevel     Type = 1
	Trade         Type = 2
	SnapshotBegin Type = 3
	SnapshotEnd   Type = 4
	KillSwitch    Type = 5
	Heartbeat     Type = 6
)

const (
	SideBid uint8 = 0
	SideAsk uint8 = 1
	AggBuy  uint8 = 0
	AggSell uint8 = 1
)

const FlagGap uint32 = 1 << 0

type Event struct {
	Type         Type
	Side         uint8
	SymbolID     uint16
	Flags        uint32
	ExchangeTsNs uint64
	RecvTsNs     uint64
	Price        int64 // ticks
	Qty          int64 // lots
	Seq          uint64
}

// Encode writes e into b, which must be at least Size bytes. No allocation.
func (e *Event) Encode(b []byte) {
	_ = b[Size-1]
	b[0] = byte(e.Type)
	b[1] = e.Side
	binary.LittleEndian.PutUint16(b[2:], e.SymbolID)
	binary.LittleEndian.PutUint32(b[4:], e.Flags)
	binary.LittleEndian.PutUint64(b[8:], e.ExchangeTsNs)
	binary.LittleEndian.PutUint64(b[16:], e.RecvTsNs)
	binary.LittleEndian.PutUint64(b[24:], uint64(e.Price))
	binary.LittleEndian.PutUint64(b[32:], uint64(e.Qty))
	binary.LittleEndian.PutUint64(b[40:], e.Seq)
	clear(b[48:Size])
}

func Decode(b []byte) (Event, error) {
	if len(b) < Size {
		return Event{}, errors.New("event: short buffer")
	}
	return Event{
		Type:         Type(b[0]),
		Side:         b[1],
		SymbolID:     binary.LittleEndian.Uint16(b[2:]),
		Flags:        binary.LittleEndian.Uint32(b[4:]),
		ExchangeTsNs: binary.LittleEndian.Uint64(b[8:]),
		RecvTsNs:     binary.LittleEndian.Uint64(b[16:]),
		Price:        int64(binary.LittleEndian.Uint64(b[24:])),
		Qty:          int64(binary.LittleEndian.Uint64(b[32:])),
		Seq:          binary.LittleEndian.Uint64(b[40:]),
	}, nil
}
