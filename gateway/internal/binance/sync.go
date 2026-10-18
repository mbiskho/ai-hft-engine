// Package binance keeps a local order book in sync with Binance's diff depth
// stream, following the exchange's documented procedure:
//
//  1. Open the <symbol>@depth@100ms stream and buffer events.
//  2. Fetch a REST snapshot; note its lastUpdateId.
//  3. Drop buffered events with u <= lastUpdateId.
//  4. The first applied event must satisfy U <= lastUpdateId+1 <= u.
//  5. Every later event must have U == previous u + 1. Anything else is a gap:
//     discard the book and start again from step 2.
//
// Syncer is the pure state machine for steps 3-5; the network code feeds it.
package binance

import "fmt"

// DepthDiff is one diff event (field names follow the Binance payload).
type DepthDiff struct {
	FirstID uint64 // U
	FinalID uint64 // u
	EventMs int64  // E
	Bids    [][2]string
	Asks    [][2]string
}

type Action int

const (
	Apply  Action = iota // apply this diff to the book
	Drop                 // stale (already in the snapshot); ignore
	Resync               // gap detected; fetch a new snapshot
	Buffer               // no snapshot yet; keep it until one arrives
)

func (a Action) String() string {
	return [...]string{"apply", "drop", "resync", "buffer"}[a]
}

type Syncer struct {
	haveSnapshot bool
	snapshotID   uint64
	lastFinal    uint64
	first        bool
	gaps         int
}

// Snapshot resets the sequence at a REST snapshot's lastUpdateId.
func (s *Syncer) Snapshot(lastUpdateID uint64) {
	s.haveSnapshot = true
	s.snapshotID = lastUpdateID
	s.lastFinal = lastUpdateID
	s.first = true
}

// Next decides what to do with a diff.
func (s *Syncer) Next(d DepthDiff) (Action, error) {
	if !s.haveSnapshot {
		return Buffer, nil
	}
	if d.FinalID <= s.snapshotID {
		return Drop, nil
	}
	if s.first {
		if d.FirstID <= s.snapshotID+1 && s.snapshotID+1 <= d.FinalID {
			s.first = false
			s.lastFinal = d.FinalID
			return Apply, nil
		}
		return s.gap(fmt.Errorf("first diff [%d,%d] does not bridge snapshot %d", d.FirstID, d.FinalID, s.snapshotID))
	}
	if d.FirstID != s.lastFinal+1 {
		return s.gap(fmt.Errorf("expected U=%d, got [%d,%d]", s.lastFinal+1, d.FirstID, d.FinalID))
	}
	s.lastFinal = d.FinalID
	return Apply, nil
}

func (s *Syncer) gap(err error) (Action, error) {
	s.haveSnapshot = false
	s.gaps++
	return Resync, err
}

func (s *Syncer) Gaps() int { return s.gaps }
