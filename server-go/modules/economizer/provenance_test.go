package economizer

import (
	"sync"
	"testing"
)

func goodBinding() ProvenanceBinding {
	return ProvenanceBinding{
		TenantID: 1, TaskID: 2, CallID: 3,
		SemanticContractID: 4, TransformID: 5, TransformVersion: 6,
	}
}

func TestProvenanceIssueAndConsume(t *testing.T) {
	src := []byte("the reduced payload")
	cap, err := IssueProvenanceLocal(goodBinding(), src)
	if err != nil {
		t.Fatal(err)
	}
	if err := cap.Consume(goodBinding(), src); err != nil {
		t.Errorf("a matching consume should succeed: %v", err)
	}
}

// Every binding field must be set: a zero id is an UNSET id, and treating unset
// as "matches" would let a capability bind to anything.
func TestProvenanceRequiresCompleteBinding(t *testing.T) {
	fields := []func(*ProvenanceBinding){
		func(b *ProvenanceBinding) { b.TenantID = 0 },
		func(b *ProvenanceBinding) { b.TaskID = 0 },
		func(b *ProvenanceBinding) { b.CallID = 0 },
		func(b *ProvenanceBinding) { b.SemanticContractID = 0 },
		func(b *ProvenanceBinding) { b.TransformID = 0 },
		func(b *ProvenanceBinding) { b.TransformVersion = 0 },
	}
	for i, clear := range fields {
		b := goodBinding()
		clear(&b)
		if _, err := IssueProvenanceLocal(b, []byte("x")); err == nil {
			t.Errorf("field %d unset: issue should fail", i)
		}
		// And an incomplete EXPECTED binding must not be accepted at consume.
		cap, _ := IssueProvenanceLocal(goodBinding(), []byte("x"))
		if err := cap.Consume(b, []byte("x")); err == nil {
			t.Errorf("field %d unset: consume should fail", i)
		}
	}
}

// A capability is bound to its exact call: no field may differ.
func TestProvenanceRejectsWrongBinding(t *testing.T) {
	src := []byte("payload")
	mutations := []func(*ProvenanceBinding){
		func(b *ProvenanceBinding) { b.TenantID = 99 },
		func(b *ProvenanceBinding) { b.TaskID = 99 },
		func(b *ProvenanceBinding) { b.CallID = 99 },
		func(b *ProvenanceBinding) { b.SemanticContractID = 99 },
		func(b *ProvenanceBinding) { b.TransformID = 99 },
		func(b *ProvenanceBinding) { b.TransformVersion = 99 },
	}
	for i, mutate := range mutations {
		cap, _ := IssueProvenanceLocal(goodBinding(), src)
		wrong := goodBinding()
		mutate(&wrong)
		if err := cap.Consume(wrong, src); err == nil {
			t.Errorf("mutation %d: a different call must not consume this capability", i)
		}
	}
}

// The capability covers specific BYTES: swapping the payload must fail even when
// the length matches, and a length change must fail too.
func TestProvenanceRejectsDifferentBytes(t *testing.T) {
	cap, _ := IssueProvenanceLocal(goodBinding(), []byte("original"))
	if err := cap.Consume(goodBinding(), []byte("modified")); err == nil {
		t.Error("same-length different bytes must be rejected")
	}
	cap2, _ := IssueProvenanceLocal(goodBinding(), []byte("original"))
	if err := cap2.Consume(goodBinding(), []byte("original-plus")); err == nil {
		t.Error("a length change must be rejected")
	}
}

// Single use: a replay must fail.
func TestProvenanceIsSingleUse(t *testing.T) {
	src := []byte("payload")
	cap, _ := IssueProvenanceLocal(goodBinding(), src)
	if err := cap.Consume(goodBinding(), src); err != nil {
		t.Fatal(err)
	}
	if err := cap.Consume(goodBinding(), src); err == nil {
		t.Error("a consumed capability must not be consumable again")
	}
}

// Single use must hold under a RACE, not merely in the happy path — that is why
// the flag is atomic rather than a plain bool.
func TestProvenanceSingleUseUnderRace(t *testing.T) {
	src := []byte("payload")
	cap, _ := IssueProvenanceLocal(goodBinding(), src)

	const racers = 64
	var wg sync.WaitGroup
	var mu sync.Mutex
	wins := 0
	wg.Add(racers)
	for i := 0; i < racers; i++ {
		go func() {
			defer wg.Done()
			if err := cap.Consume(goodBinding(), src); err == nil {
				mu.Lock()
				wins++
				mu.Unlock()
			}
		}()
	}
	wg.Wait()
	if wins != 1 {
		t.Errorf("%d concurrent consumers succeeded, want exactly 1", wins)
	}
}

func TestProvenanceSizeCap(t *testing.T) {
	over := make([]byte, ProvenanceMaxSource+1)
	if _, err := IssueProvenanceLocal(goodBinding(), over); err == nil {
		t.Error("a payload over the cap must be refused")
	}
	cap, _ := IssueProvenanceLocal(goodBinding(), []byte("x"))
	if err := cap.Consume(goodBinding(), over); err == nil {
		t.Error("consuming an over-cap payload must be refused")
	}
}

// An empty payload is legitimate and must round-trip.
func TestProvenanceEmptySource(t *testing.T) {
	cap, err := IssueProvenanceLocal(goodBinding(), nil)
	if err != nil {
		t.Fatal(err)
	}
	if err := cap.Consume(goodBinding(), nil); err != nil {
		t.Errorf("an empty payload should round-trip: %v", err)
	}
}

func TestProvenanceDestroyPreventsUse(t *testing.T) {
	src := []byte("payload")
	cap, _ := IssueProvenanceLocal(goodBinding(), src)
	cap.Destroy()
	if err := cap.Consume(goodBinding(), src); err == nil {
		t.Error("a destroyed capability must not be consumable")
	}
	var nilCap *ProvenanceCapability
	nilCap.Destroy() // must not panic
	if err := nilCap.Consume(goodBinding(), src); err == nil {
		t.Error("a nil capability must not be consumable")
	}
}
