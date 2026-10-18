// Package wire validates fixed-width fields in module event frames.
package wire

// ZeroPadding reports whether every padding byte is zero.
func ZeroPadding(value []byte) bool {
	for _, item := range value {
		if item != 0 {
			return false
		}
	}
	return true
}

// NonzeroText reports whether text contains no NUL byte. Empty fields are allowed.
func NonzeroText(value []byte) bool {
	for _, item := range value {
		if item == 0 {
			return false
		}
	}
	return true
}
