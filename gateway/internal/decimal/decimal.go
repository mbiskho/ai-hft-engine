// Package decimal converts exchange decimal strings to integer ticks and lots
// without ever going through float64. "65432.10" with tick "0.01" is exactly
// 6543210. A float parse can come out as 6543209.999…, which truncates to the
// wrong price level.
package decimal

import (
	"errors"
	"fmt"
	"strings"
)

var (
	ErrSyntax      = errors.New("decimal: invalid number")
	ErrNotMultiple = errors.New("decimal: value is not a multiple of the increment")
	ErrRange       = errors.New("decimal: value out of int64 range")
)

// parse returns the digits as an integer and the number of fractional digits.
func parse(s string) (mant int64, scale int, err error) {
	s = strings.TrimSpace(s)
	if s == "" {
		return 0, 0, ErrSyntax
	}
	neg := false
	if s[0] == '-' || s[0] == '+' {
		neg = s[0] == '-'
		s = s[1:]
	}
	intPart, frac, _ := strings.Cut(s, ".")
	frac = strings.TrimRight(frac, "0") // trailing zeros carry no value; Binance pads to 8
	if intPart == "" && frac == "" {
		return 0, 0, ErrSyntax
	}
	for _, c := range intPart + frac {
		if c < '0' || c > '9' {
			return 0, 0, ErrSyntax
		}
		if mant > (1<<63-1-9)/10 {
			return 0, 0, ErrRange
		}
		mant = mant*10 + int64(c-'0')
	}
	if neg {
		mant = -mant
	}
	return mant, len(frac), nil
}

func pow10(n int) int64 {
	p := int64(1)
	for i := 0; i < n; i++ {
		p *= 10
	}
	return p
}

// ToUnits returns value / increment as an exact integer, e.g.
// ToUnits("65432.10000000", "0.01") == 6543210.
func ToUnits(value, increment string) (int64, error) {
	vm, vs, err := parse(value)
	if err != nil {
		return 0, fmt.Errorf("%w: %q", err, value)
	}
	im, is, err := parse(increment)
	if err != nil || im <= 0 {
		return 0, fmt.Errorf("%w: increment %q", ErrSyntax, increment)
	}
	// Bring both to the same scale, then divide exactly.
	scale := max(vs, is)
	if scale > 18 {
		return 0, ErrRange
	}
	vm *= pow10(scale - vs)
	im *= pow10(scale - is)
	if vm%im != 0 {
		return 0, fmt.Errorf("%w: %s / %s", ErrNotMultiple, value, increment)
	}
	return vm / im, nil
}
