package wfe

import (
	"errors"
	"os"
	"path/filepath"
	"sync"
	"testing"
)

func TestRegistrySaveIsLiveAndOptimisticallyLocked(t *testing.T) {
	registry, err := NewRegistry(filepath.Join(t.TempDir(), "workflows"))
	if err != nil {
		t.Fatal(err)
	}
	v1 := []byte("name: ui-flow\nstart: first\nnodes:\n  - id: first\n    block: author.proposal\n")
	report, err := registry.Save("ui-flow", v1, "")
	if err != nil {
		t.Fatal(err)
	}
	loaded, err := registry.Load("ui-flow")
	if err != nil {
		t.Fatal(err)
	}
	if loaded.Version != report.Version || loaded.Nodes[0].Block != "author.proposal" {
		t.Fatalf("loaded=%+v report=%+v", loaded, report)
	}
	v2 := []byte("name: ui-flow\nstart: first\nnodes:\n  - id: first\n    block: author.proposal\n    params:\n      persona: engineer\n      delegate: kimi\n")
	if _, err := registry.Save("ui-flow", v2, "stale"); err == nil {
		t.Fatal("stale UI save unexpectedly overwrote workflow")
	} else {
		var conflict *VersionConflictError
		if !errors.As(err, &conflict) || conflict.Current != report.Version {
			t.Fatalf("conflict=%v", err)
		}
	}
	report2, err := registry.Save("ui-flow", v2, report.Version)
	if err != nil {
		t.Fatal(err)
	}
	loaded, err = registry.Load("ui-flow")
	if err != nil {
		t.Fatal(err)
	}
	if loaded.Version != report2.Version || loaded.Nodes[0].Params["delegate"] != "kimi" {
		t.Fatalf("GUI params not live: %+v", loaded)
	}
	rows, err := registry.List()
	if err != nil {
		t.Fatal(err)
	}
	if len(rows) != 1 || rows[0].Name != "ui-flow" || !rows[0].Valid {
		t.Fatalf("rows=%+v", rows)
	}
	if _, err := os.Stat(filepath.Join(registry.dir, "ui-flow.yaml")); err != nil {
		t.Fatal(err)
	}
}

func TestRegistryConcurrentSaveHasSingleWinnerAndKeepsPinnedVersion(t *testing.T) {
	dir := filepath.Join(t.TempDir(), "workflows")
	r1, _ := NewRegistry(dir)
	r2, _ := NewRegistry(dir)
	base := []byte("name: flow\nnodes:\n  - id: start\n    block: author.proposal\n")
	first, err := r1.Save("flow", base, "")
	if err != nil {
		t.Fatal(err)
	}
	variants := [][]byte{
		[]byte("name: flow\nnodes:\n  - id: start\n    block: author.proposal\n    params: {persona: one}\n"),
		[]byte("name: flow\nnodes:\n  - id: start\n    block: author.proposal\n    params: {persona: two}\n"),
	}
	regs := []*Registry{r1, r2}
	results := make(chan error, 2)
	var wg sync.WaitGroup
	for i := range regs {
		wg.Add(1)
		go func(i int) {
			defer wg.Done()
			_, err := regs[i].Save("flow", variants[i], first.Version)
			results <- err
		}(i)
	}
	wg.Wait()
	close(results)
	success, conflicts := 0, 0
	for err := range results {
		if err == nil {
			success++
		} else {
			var conflict *VersionConflictError
			if errors.As(err, &conflict) {
				conflicts++
			} else {
				t.Fatal(err)
			}
		}
	}
	if success != 1 || conflicts != 1 {
		t.Fatalf("success=%d conflicts=%d", success, conflicts)
	}
	pinned, err := r1.LoadVersion("flow", first.Version)
	if err != nil {
		t.Fatal(err)
	}
	if pinned.Version != first.Version {
		t.Fatalf("pinned=%s", pinned.Version)
	}
}

func TestPinnedWorkflowKeepsResolvedCustomBlock(t *testing.T) {
	registry, err := NewRegistry(filepath.Join(t.TempDir(), "workflows"))
	if err != nil {
		t.Fatal(err)
	}
	block := BlockDefinition{Name: "custom", Persona: "engineer", Prompt: "old prompt", Consumes: "none", Produces: "none"}
	if err := registry.SaveDelegateBlock(block); err != nil {
		t.Fatal(err)
	}
	if _, err := registry.Save("flow", []byte("name: flow\nnodes:\n  - id: run\n    block: custom\n"), ""); err != nil {
		t.Fatal(err)
	}
	pinned, err := registry.Pin("flow")
	if err != nil {
		t.Fatal(err)
	}
	block.Prompt = "new prompt"
	if err := registry.SaveDelegateBlock(block); err != nil {
		t.Fatal(err)
	}
	old, err := registry.BlockVersion("flow", pinned.Version, "custom")
	if err != nil {
		t.Fatal(err)
	}
	if old.Prompt != "old prompt" {
		t.Fatalf("pinned prompt=%q", old.Prompt)
	}
	current, err := registry.Pin("flow")
	if err != nil {
		t.Fatal(err)
	}
	if current.Version == pinned.Version {
		t.Fatal("custom block edit did not create a new workflow execution version")
	}
}
