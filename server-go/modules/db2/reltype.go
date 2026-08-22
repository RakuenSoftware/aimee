package db2

import "strings"

// relTypeNameMax mirrors REL_TYPE_NAME_MAX. It is a buffer size in the C, which
// makes it a silent truncation point rather than a validation error: a longer
// name is cut, not refused, and two names agreeing in their first 63 characters
// normalize to the same relation.
const relTypeNameMax = 63

// normalizeRelType folds a relation name to the spelling the ontology stores.
//
// Every ontology statement binds the normalized form, so this is what decides
// whether two spellings are the same relation. The rules, in the order they
// apply to each character:
//
//   - letters and digits are kept, lowercased
//   - an uppercase letter following a lowercase letter or a digit starts a new
//     word, so "worksFor" becomes "works_for"
//   - anything else becomes an underscore, and a run of them collapses to one
//   - leading underscores are suppressed and trailing ones stripped
//
// A name that normalizes to nothing -- punctuation only -- is not a relation,
// and the callers refuse it rather than querying for the empty string.
func normalizeRelType(in string) string {
	var out strings.Builder
	out.Grow(len(in))

	// Starts true so a leading run of punctuation is suppressed rather than
	// producing an underscore with nothing before it.
	previousUnderscore := true
	previousLowerOrDigit := false

	for _, b := range []byte(in) {
		if out.Len() >= relTypeNameMax {
			break
		}
		switch {
		case isASCIILetter(b) || isASCIIDigit(b):
			if isASCIIUpper(b) && previousLowerOrDigit && !previousUnderscore &&
				out.Len() < relTypeNameMax {
				out.WriteByte('_')
			}
			if out.Len() < relTypeNameMax {
				out.WriteByte(toASCIILower(b))
			}
			previousUnderscore = false
			previousLowerOrDigit = isASCIILower(b) || isASCIIDigit(b)
		case !previousUnderscore:
			out.WriteByte('_')
			previousUnderscore = true
			previousLowerOrDigit = false
		}
	}
	// Bytes rather than runes throughout, which is what the C does: it walks
	// with isalnum on unsigned char, so every byte of a multi-byte character is
	// non-alphanumeric and collapses to a single underscore. Decoding UTF-8 here
	// would normalize differently and split the ontology in two.
	return strings.TrimRight(out.String(), "_")
}

func isASCIILetter(b byte) bool { return isASCIILower(b) || isASCIIUpper(b) }
func isASCIILower(b byte) bool  { return b >= 'a' && b <= 'z' }
func isASCIIUpper(b byte) bool  { return b >= 'A' && b <= 'Z' }
func isASCIIDigit(b byte) bool  { return b >= '0' && b <= '9' }

func toASCIILower(b byte) byte {
	if isASCIIUpper(b) {
		return b + ('a' - 'A')
	}
	return b
}
