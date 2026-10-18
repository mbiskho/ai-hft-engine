package binance

import "testing"

func diff(U, u uint64) DepthDiff { return DepthDiff{FirstID: U, FinalID: u} }

func TestBuffersUntilSnapshot(t *testing.T) {
	var s Syncer
	if a, _ := s.Next(diff(1, 5)); a != Buffer {
		t.Fatalf("got %v", a)
	}
}

func TestDropsStaleThenBridgesThenContinues(t *testing.T) {
	var s Syncer
	s.Snapshot(100)
	steps := []struct {
		d    DepthDiff
		want Action
	}{
		{diff(90, 95), Drop},    // entirely before the snapshot
		{diff(96, 100), Drop},   // ends exactly at the snapshot
		{diff(98, 104), Apply},  // bridges 101
		{diff(105, 110), Apply}, // contiguous
		{diff(111, 111), Apply},
	}
	for i, st := range steps {
		if a, err := s.Next(st.d); a != st.want {
			t.Fatalf("step %d: got %v (%v), want %v", i, a, err, st.want)
		}
	}
}

func TestFirstDiffMustBridge(t *testing.T) {
	var s Syncer
	s.Snapshot(100)
	if a, err := s.Next(diff(103, 110)); a != Resync || err == nil {
		t.Fatalf("got %v, %v; want resync (101-102 missing)", a, err)
	}
}

func TestGapForcesResyncAndBuffersAfter(t *testing.T) {
	var s Syncer
	s.Snapshot(100)
	_, _ = s.Next(diff(101, 105))
	if a, _ := s.Next(diff(107, 109)); a != Resync {
		t.Fatalf("got %v, want resync on missing 106", a)
	}
	if s.Gaps() != 1 {
		t.Fatalf("gaps = %d", s.Gaps())
	}
	if a, _ := s.Next(diff(110, 112)); a != Buffer {
		t.Fatalf("after a gap, diffs wait for the new snapshot; got %v", a)
	}
	s.Snapshot(111)
	if a, _ := s.Next(diff(110, 112)); a != Apply {
		t.Fatalf("got %v", a)
	}
}
