package postgres

import (
	"bytes"
	"encoding/binary"
	"strings"
	"testing"
)

func transactionSeed(seed uint64) *bytes.Reader {
	var raw [8]byte
	binary.LittleEndian.PutUint64(raw[:], seed)
	return bytes.NewReader(raw[:])
}
func TestTransactionRegistryRandomSeedNoWrapAndEntropyRefusal(t *testing.T) {
	registry := newTxRegistryWithEntropy(transactionSeed(^uint64(0) - 1))
	registry.ttl = 0
	last, err := registry.add(nil, nil)
	if err != nil || last != ^uint64(0) {
		t.Fatal(last, err)
	}
	if next, err := registry.add(nil, nil); err == nil || next != 0 {
		t.Fatal("transaction handle wrapped", next, err)
	}
	registry = newTxRegistryWithEntropy(strings.NewReader("unready"))
	if handle, err := registry.add(nil, nil); err == nil || handle != 0 || len(registry.open) != 0 {
		t.Fatal("entropy failure allocated transaction", handle, err)
	}
	first := newTxRegistryWithEntropy(transactionSeed(100))
	second := newTxRegistryWithEntropy(transactionSeed(900))
	a, err := first.add(nil, nil)
	if err != nil {
		t.Fatal(err)
	}
	b, err := second.add(nil, nil)
	if err != nil {
		t.Fatal(err)
	}
	if a != 101 || b != 901 {
		t.Fatal("registry seed ignored", a, b)
	}
	if _, ok := second.remove(a); ok {
		t.Fatal("old registry handle consumed new lease")
	}
	if _, ok := second.get(b); !ok {
		t.Fatal("new lease lost")
	}
	if first.maxOpen != 64 || first.ttl != txIdleTTL {
		t.Fatal("registry cap or TTL changed")
	}
}
