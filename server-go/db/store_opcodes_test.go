package db

import "testing"

// The seven store opcodes, pinned to their numbers.
//
// WHY THIS EXISTS. The const block declaring them had no `iota`, so
// `opStoreExec uint32 = 1` followed by six bare names made all seven equal 1:
// every operation aimee asked of the store went onto the wire as EXEC.
//
// It survived because nothing served the other end. With no SQL stage on the
// postgres module, every call failed at the transport before its opcode was
// read, so the defect had no way to show. A wire constant is only checked by
// the far side reading it -- and until there is a far side, "the numbers are
// obviously right" is the only check there is.
//
// Numbers, not relationships. Asserting they are merely DISTINCT would pass on
// any renumbering, and renumbering a live wire silently reinterprets every
// frame the other module has already learned to decode.
func TestStoreOpcodesAreTheContractNumbers(t *testing.T) {
	for _, c := range []struct {
		name string
		got  uint32
		want uint32
	}{
		{"exec", opStoreExec, 1},
		{"query", opStoreQuery, 2},
		{"begin", opStoreBegin, 3},
		{"commit", opStoreCommit, 4},
		{"rollback", opStoreRollback, 5},
		{"migrate", opStoreMigrate, 6},
		{"current_version", opStoreCurrentVersion, 7},
	} {
		if c.got != c.want {
			t.Errorf("opStore%s = %d, the contract says %d", c.name, c.got, c.want)
		}
	}
}

// The same defect one type set over: the value kinds are also a bare const
// block, and they carry the same hazard. wireNull starts at iota so they are
// correct today; this pins them so a hand-edit cannot quietly flatten them.
func TestWireValueKindsAreTheContractNumbers(t *testing.T) {
	for _, c := range []struct {
		name string
		got  uint8
		want uint8
	}{
		{"null", wireNull, 0},
		{"text", wireText, 1},
		{"int", wireInt, 2},
		{"float", wireFloat, 3},
		{"bool", wireBool, 4},
		{"text_array", wireTextArray, 5},
		{"bytes", wireBytes, 6},
	} {
		if c.got != c.want {
			t.Errorf("wire%s = %d, the contract says %d", c.name, c.got, c.want)
		}
	}
}
