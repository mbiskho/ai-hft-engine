package decimal

import (
	"errors"
	"testing"
)

func TestToUnits(t *testing.T) {
	cases := []struct {
		v, inc string
		want   int64
	}{
		{"65432.10000000", "0.01", 6543210},
		{"65432.1", "0.01", 6543210},
		{"0.00012000", "0.00001", 12},
		{"1", "0.00001", 100000},
		{"0.00000000", "0.00001", 0},
		{"3000.5", "0.5", 6001},
		{"-12.34", "0.01", -1234},
	}
	for _, c := range cases {
		got, err := ToUnits(c.v, c.inc)
		if err != nil || got != c.want {
			t.Errorf("ToUnits(%q, %q) = %d, %v; want %d", c.v, c.inc, got, err, c.want)
		}
	}
}

// The reason this package exists: int64(f * 100) gets every one of these wrong.
func TestNoFloatRoundingTrap(t *testing.T) {
	for v, want := range map[string]int64{"0.29": 29, "1.15": 115, "4.35": 435, "65432.07": 6543207} {
		if got, err := ToUnits(v, "0.01"); err != nil || got != want {
			t.Errorf("ToUnits(%q) = %d, %v; want %d", v, got, err, want)
		}
	}
}

func TestRejects(t *testing.T) {
	if _, err := ToUnits("1.005", "0.01"); !errors.Is(err, ErrNotMultiple) {
		t.Errorf("off-tick price: err = %v", err)
	}
	for _, bad := range []string{"", "abc", "1.2.3", "1e5", "."} {
		if _, err := ToUnits(bad, "0.01"); !errors.Is(err, ErrSyntax) {
			t.Errorf("%q: err = %v, want ErrSyntax", bad, err)
		}
	}
	if _, err := ToUnits("1", "0"); err == nil {
		t.Error("zero increment accepted")
	}
}
