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
