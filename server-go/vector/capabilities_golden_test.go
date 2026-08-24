package vector

import (
	"encoding/hex"
	"testing"
)

// The same 48 bytes src/tests/test_vector_route.c pins, so that a layout change
// on either side fails on the side that changed.
//
// Two implementations of one wire cannot be kept in step by careful reading --
// which is how the C decoder's offsets were derived in the first place, and
// exactly the kind of care that has been wrong before. This is the shared fact
// they are both checked against.
const capabilitiesGoldenFrame = "44423343010030008877665544332211030000000500000001000000" +
	"0100000000040000400000008000000000000000"

func capabilitiesGoldenValue() Capabilities {
	return Capabilities{
		Generation: 0x1122334455667788, Operations: OperationSearch | OperationApply,
		Metrics: MetricCosine | MetricDot, Filters: FilterExact,
		MaxDimension: 1024, MaxBatch: 64, MaxTopK: 128, Ready: true,
	}
}

func TestCapabilitiesGoldenFrame(t *testing.T) {
	encoded, err := EncodeCapabilities(capabilitiesGoldenValue())
	if err != nil {
		t.Fatal(err)
	}
	if got := hex.EncodeToString(encoded); got != capabilitiesGoldenFrame {
		t.Fatalf("capabilities layout changed:\n got %s\nwant %s", got, capabilitiesGoldenFrame)
	}

	raw, err := hex.DecodeString(capabilitiesGoldenFrame)
	if err != nil {
		t.Fatal(err)
	}
	decoded, err := DecodeCapabilities(raw)
	if err != nil {
		t.Fatal(err)
	}
	if decoded != capabilitiesGoldenValue() {
		t.Fatalf("golden frame does not decode to the golden value: %+v", decoded)
	}
}
