package roundtable

import (
	"context"
	"encoding/json"
	"os"
	"path/filepath"
	"sync"
	"testing"
	"time"

	"github.com/JBailes/aimee/server-go/bus"
	"github.com/JBailes/aimee/server-go/delegate"
	delegatemodule "github.com/JBailes/aimee/server-go/modules/delegates"
	"github.com/JBailes/aimee/server-go/modules/roundtable/panel"
)

// moduleStageBridge crosses the same JSON stage contract as the process bus.
// Holding successful group-plan replies until every campaign has planned makes
// the subsequent limiter contention a real admission race rather than ten
// panels that happen to execute serially.
type moduleStageBridge struct {
	handler   bus.ModuleHandler
	planCount int
	planned   int
	plansDone chan struct{}
	doneOnce  sync.Once
	mu        sync.Mutex
}

func (b *moduleStageBridge) releasePlans() {
	b.doneOnce.Do(func() { close(b.plansDone) })
}

func (b *moduleStageBridge) Call(ctx context.Context, _ uint32, stage uint32, _ uint64,
	_ time.Duration, request []byte) ([]byte, error) {
	reply, status := b.handler(bus.ModuleInvocation{StageID: stage}, request)
	if status != bus.ModuleStatusOK {
		if stage == delegate.StageGroupPlan {
			b.releasePlans()
		}
		return nil, &bus.ModuleCallStatusError{Status: status}
	}
	if stage == delegate.StageGroupPlan {
		b.mu.Lock()
		b.planned++
		if b.planned == b.planCount {
			b.releasePlans()
		}
		b.mu.Unlock()
		select {
		case <-b.plansDone:
		case <-ctx.Done():
			b.releasePlans()
			return nil, ctx.Err()
		}
	}
	return reply, nil
}

func TestTenOverlappingPanelsCrossGoProducerAdmissionWithoutUnreachable(t *testing.T) {
	const campaigns = 10
	home := t.TempDir()
	script := filepath.Join(home, "slow-reviewer")
	if err := os.WriteFile(script, []byte("#!/bin/sh\nsleep 2\nprintf done\n"), 0o700); err != nil {
		t.Fatal(err)
	}
	registry := map[string]any{"agents": []map[string]any{{
		"name": "live-reviewer", "cli_kind": "codex", "cli_cmd": script,
		"roles": []string{"review"}, "personas": []string{"all"}, "max_parallel": 1,
	}}}
	body, _ := json.Marshal(registry)
	if err := os.WriteFile(filepath.Join(home, "models.json"), body, 0o600); err != nil {
		t.Fatal(err)
	}
	executor, err := delegatemodule.NewRegistryExecutor(home)
	if err != nil {
		t.Fatal(err)
	}
	bridge := &moduleStageBridge{handler: delegatemodule.NewHandler(executor), planCount: campaigns,
		plansDone: make(chan struct{})}
	client, err := delegate.NewClient(bridge, time.Second)
	if err != nil {
		t.Fatal(err)
	}
	delegates := seatBus{client: client}
	reviewPanel := panel.Panel{Seats: []panel.Seat{{Persona: "qa"}}, MinSuccessful: 1,
		DeadlineMS: 150, Acquired: true}
	content := "a complete frozen implementation diff long enough for live panel review"

	results := make(chan panel.RunResult, campaigns)
	errs := make(chan error, campaigns)
	var wg sync.WaitGroup
	for i := range campaigns {
		wg.Add(1)
		go func(i int) {
			defer wg.Done()
			run := panel.Run{ID: "live-capacity-" + string(rune('a'+i)),
				OriginalRequest: "review the implementation under overlapping delegate load",
				Reviewed:        panel.Artifact{Stage: "frozen_diff", Content: content, Hash: panel.Hash([]byte(content))}}
			result, err := panel.Convene(t.Context(), delegates, run, reviewPanel, "")
			results <- result
			errs <- err
		}(i)
	}
	wg.Wait()
	close(results)
	close(errs)
	for err := range errs {
		if err != nil {
			t.Fatal(err)
		}
	}
	capacityDeadlines, executionDeadlines := 0, 0
	for result := range results {
		switch result.PauseReason {
		case "panel_capacity_deadline":
			capacityDeadlines++
		case "panel_deadline":
			executionDeadlines++
		default:
			t.Fatalf("live producer campaign returned imprecise state %q: %+v", result.PauseReason, result)
		}
		if !result.DeadlineHit {
			t.Fatalf("live producer deadline was not recorded: %+v", result)
		}
	}
	if capacityDeadlines == 0 || executionDeadlines == 0 || capacityDeadlines+executionDeadlines != campaigns {
		t.Fatalf("campaign did not exercise both limiter wait and execution deadlines: capacity=%d execution=%d",
			capacityDeadlines, executionDeadlines)
	}
}
