package executionpolicy

import (
	"encoding/json"
	"strings"
	"testing"
	"time"
)

func TestExplorationExperimentCannotAuthorizeReleaseOrAnotherSession(t *testing.T) {
	now := time.Now()
	c := calibrationContract(now)
	manifest := strings.Repeat("a", 64)
	a := explorationExperiment{Version: 1, Kind: "experiment", AuthorizedBy: "test-only",
		Created: now.Add(-time.Minute), Expires: now.Add(time.Hour), ManifestSHA256: manifest,
		Principal: c.Binding.Principal, Sessions: []string{c.Binding.Session}, Scope: calibrationScope(c), Limits: c.Limits}
	encode := func(a explorationExperiment) []byte {
		b, err := json.Marshal(a)
		if err != nil {
			t.Fatal(err)
		}
		return b
	}
	raw := encode(a)
	if got, err := parseExplorationExperiment(raw, c, now, manifest); err != nil || got != "experiment:"+manifest {
		t.Fatal(got, err)
	}
	if _, err := parseExplorationCalibration(raw, c, now); err == nil {
		t.Fatal("experimental authorization accepted as a measured calibration")
	}
	if _, err := parseExplorationExperiment(raw, c, now, strings.Repeat("b", 64)); err == nil {
		t.Fatal("experiment silently changed its frozen manifest")
	}
	for name, change := range map[string]func(*explorationExperiment){
		"expiry":     func(a *explorationExperiment) { a.Expires = now },
		"duration":   func(a *explorationExperiment) { a.Expires = a.Created.Add(6*time.Hour + time.Second) },
		"future":     func(a *explorationExperiment) { a.Created = now.Add(time.Second) },
		"kind":       func(a *explorationExperiment) { a.Kind = "calibration" },
		"authorizer": func(a *explorationExperiment) { a.AuthorizedBy = "" },
		"principal":  func(a *explorationExperiment) { a.Principal = "another-principal" },
		"session":    func(a *explorationExperiment) { a.Sessions = []string{"another-session"} },
		"wildcard":   func(a *explorationExperiment) { a.Sessions = []string{"*"} },
		"duplicates": func(a *explorationExperiment) { a.Sessions = []string{c.Binding.Session, c.Binding.Session} },
		"unbounded":  func(a *explorationExperiment) { a.Sessions = make([]string, 513) },
		"scope":      func(a *explorationExperiment) { a.Scope.ProducerBuild = "another-build" },
		"limit":      func(a *explorationExperiment) { a.Limits.RawScans = ceiling(1) },
	} {
		t.Run(name, func(t *testing.T) {
			changed := a
			change(&changed)
			if _, err := parseExplorationExperiment(encode(changed), c, now, manifest); err == nil {
				t.Fatal("invalid experiment accepted")
			}
		})
	}
	for _, change := range []func(*explorationContract){
		func(c *explorationContract) { c.CoverageComplete = false },
		func(c *explorationContract) { c.Binding.IndexObservedCurrent = false },
		func(c *explorationContract) { c.Binding.OwnerObservedCurrent = false },
		func(c *explorationContract) { c.Binding.HostWorktree = false },
		func(c *explorationContract) { c.Binding.WorktreeGeneration = "dirty" },
		func(c *explorationContract) { c.ReceiptDigest = "" },
	} {
		changed := c
		change(&changed)
		if _, err := parseExplorationExperiment(raw, changed, now, manifest); err == nil {
			t.Fatal("experiment bypassed live admission preconditions")
		}
	}
	// An explicit but unavailable/invalid experiment never falls through to a
	// release approval. Removing the experiment opt-in still does not opt in to
	// release enforcement on its own.
	t.Setenv("AIMEE_EXPLORATION_EXPERIMENT", "invalid")
	t.Setenv("AIMEE_EXPLORATION_ENFORCE", "1")
	if approvedExplorationCalibration(c, now) != "" {
		t.Fatal("invalid experiment activated")
	}
	t.Setenv("AIMEE_EXPLORATION_EXPERIMENT", "")
	t.Setenv("AIMEE_EXPLORATION_ENFORCE", "0")
	if approvedExplorationCalibration(c, now) != "" {
		t.Fatal("neither approval was enabled")
	}
}
