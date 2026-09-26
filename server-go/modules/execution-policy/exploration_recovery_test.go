package executionpolicy

import (
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"sync"
	"sync/atomic"
	"testing"
	"time"
)

func indexedOutcome(c explorationContract, now time.Time) explorationIndexedOutcome {
	return explorationIndexedOutcome{ID: "lookup-1", Binding: c.Binding, ContractID: c.ID, Revision: c.Revision, Gap: "missing-definition", Class: "raw_scan", Path: "/project/src", Status: "empty", Completed: now}
}
func TestIndexedFallbackScopeConcurrencyAndRetry(t *testing.T) {
	now := time.Now()
	c := testContract(now)
	c.Limits.RawScans = ceiling(0)
	l := testLedger(t, c)
	o := indexedOutcome(c, now)
	if err := l.expand("need source", o.Gap, o.ID, now); err == nil {
		t.Fatal("invented outcome granted fallback")
	}
	if err := l.recordIndexedOutcome(o, now); err != nil {
		t.Fatal(err)
	}
	if err := l.expand("need source", o.Gap, o.ID, now); err != nil {
		t.Fatal(err)
	}
	if err := l.expand("retry", o.Gap, o.ID, now); err != nil {
		t.Fatal(err)
	}
	if len(l.state.Revisions) != 2 || l.state.Revisions[0].Revision != 1 {
		t.Fatal("lost revision history")
	}
	a := attempt(c, "wrong-path")
	a.Path = "/project/src-secret"
	if d, e := l.reserve(a, explorationLimits{}, true, measured, now); e != nil || !d.Restricted {
		t.Fatal("escaped scope", d, e)
	}
	var wg sync.WaitGroup
	var admitted atomic.Int64
	for i := 0; i < 32; i++ {
		wg.Add(1)
		go func(i int) {
			defer wg.Done()
			a := attempt(c, fmt.Sprint(i))
			a.Path = "/project/src/module"
			d, e := l.reserve(a, explorationLimits{}, true, measured, now)
			if e != nil {
				t.Error(e)
			}
			if !d.Restricted {
				admitted.Add(1)
				again, e := l.reserve(a, explorationLimits{}, true, measured, now)
				if e != nil || again.Restricted {
					t.Error("retry lost admission", e)
				}
			}
		}(i)
	}
	wg.Wait()
	if admitted.Load() != 1 || l.state.Fallbacks[0].Remaining != 0 {
		t.Fatal("fallback overspent", admitted.Load())
	}
}
func TestIndexedFallbackCannotWidenOperatorOrSurviveExpiry(t *testing.T) {
	now := time.Now()
	c := testContract(now)
	c.Limits.RawScans = ceiling(0)
	for _, kind := range []string{"operator", "expiry", "generation"} {
		t.Run(kind, func(t *testing.T) {
			l := testLedger(t, c)
			o := indexedOutcome(c, now)
			if e := l.recordIndexedOutcome(o, now); e != nil {
				t.Fatal(e)
			}
			if e := l.expand("gap", o.Gap, o.ID, now); e != nil {
				t.Fatal(e)
			}
			a := attempt(c, kind)
			a.Path = "/project/src"
			op := explorationLimits{}
			at := now
			switch kind {
			case "operator":
				op.RawScans = ceiling(0)
			case "expiry":
				at = now.Add(2 * time.Minute)
			case "generation":
				next := l.state.Revisions[1]
				next.Revision++
				next.Binding.IndexGeneration = "index-v2"
				if e := l.issue(next, now); e != nil {
					t.Fatal(e)
				}
				a.Binding = next.Binding
			}
			if d, e := l.reserve(a, op, true, measured, at); e != nil || !d.Restricted {
				t.Fatal("invalid fallback admitted", d, e)
			}
			if l.state.Fallbacks[0].Remaining != 1 {
				t.Fatal("failed admission consumed fallback")
			}
		})
	}
}

func TestDiscoveryFallbackAttestsActualRoots(t *testing.T) {
	root := t.TempDir()
	outside := t.TempDir()
	if err := os.Symlink(outside, filepath.Join(root, "escape")); err != nil {
		t.Fatal(err)
	}
	cases := []struct {
		tool    string
		args    map[string]string
		allowed bool
	}{
		{"grep", map[string]string{"path": ".", "pattern": "value"}, true},
		{"grep", map[string]string{"path": "escape", "pattern": "value"}, false},
		{"grep", map[string]string{"path": outside, "pattern": "value"}, false},
		{"bash", map[string]string{"command": "rg value ."}, true},
		{"bash", map[string]string{"command": "rg value . " + outside}, false},
		{"exec_command", map[string]string{"command": "rg --follow value ."}, false},
		{"bash", map[string]string{"command": "rg value escape"}, false},
		{"bash", map[string]string{"command": "rg --files"}, true},
		{"bash", map[string]string{"command": "find ."}, true},
		{"bash", map[string]string{"command": "find -L ."}, false},
	}
	for _, c := range cases {
		raw, _ := json.Marshal(c.args)
		got := explorationDiscoveryPath(c.tool, raw, root)
		if (got == root) != c.allowed {
			t.Fatalf("%s %v: %q", c.tool, c.args, got)
		}
	}
}
