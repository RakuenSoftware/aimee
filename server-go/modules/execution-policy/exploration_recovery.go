package executionpolicy

import (
	"errors"
	"path/filepath"
	"strings"
	"time"
)

// These outcomes are written by the task owner after tool completion, never
// decoded from model tool arguments or a client-authored hook cache.
type explorationIndexedOutcome struct {
	ID         string             `json:"id"`
	Binding    explorationBinding `json:"binding"`
	ContractID string             `json:"contract_id"`
	Revision   uint64             `json:"revision"`
	Gap        string             `json:"gap"`
	Class      string             `json:"fallback_class"`
	Path       string             `json:"canonical_scope"`
	Status     string             `json:"status"`
	Completed  time.Time          `json:"completed"`
}
type explorationFallback struct {
	Outcome   explorationIndexedOutcome `json:"outcome"`
	Expires   time.Time                 `json:"expires"`
	Remaining int                       `json:"remaining"`
}

func canonicalDiscoveryPath(p string) bool {
	return p != "" && filepath.IsAbs(p) && filepath.Clean(p) == p
}
func (f explorationFallback) permits(c explorationContract, a explorationAttempt, now time.Time) bool {
	o := f.Outcome
	if f.Remaining <= 0 || !f.Expires.After(now) || o.Completed.After(now) || o.Binding != a.Binding || o.ContractID != c.ID || o.Revision != c.Revision || o.Class != a.Class || !canonicalDiscoveryPath(a.Path) {
		return false
	}
	return a.Path == o.Path || strings.HasPrefix(a.Path, strings.TrimSuffix(o.Path, string(filepath.Separator))+string(filepath.Separator))
}
func (l *explorationLedger) recordIndexedOutcome(o explorationIndexedOutcome, now time.Time) error {
	l.mu.Lock()
	defer l.mu.Unlock()
	s := l.state.clone()
	if len(s.Revisions) == 0 {
		return errors.New("no host contract")
	}
	c := s.Revisions[len(s.Revisions)-1]
	if !c.valid(now) || o.Binding != c.Binding || o.ContractID != c.ID || o.Revision != c.Revision || o.ID == "" || len(o.ID) > 256 || o.Gap == "" || len(o.Gap) > 256 || o.Class != "raw_scan" || !canonicalDiscoveryPath(o.Path) || len(o.Path) > 4096 || o.Completed.After(now) || now.Sub(o.Completed) > 10*time.Minute || (o.Status != "empty" && o.Status != "failed") {
		return errors.New("invalid host indexed outcome")
	}
	if old, ok := s.Outcomes[o.ID]; ok {
		if old != o {
			return errors.New("indexed attempt identity changed")
		}
		return nil
	}
	if len(s.Outcomes) >= 128 {
		return errors.New("indexed outcome history full")
	}
	s.Outcomes[o.ID] = o
	return l.save(s)
}

// Expansion input is an explanation and a reference, not a supplied outcome.
// A recent host observation grants at most one matching scan for one minute.
// This explicit bounded escape complements the completed-turn relaxation.
func (l *explorationLedger) expand(reason, gap, outcomeID string, now time.Time) error {
	l.mu.Lock()
	defer l.mu.Unlock()
	s := l.state.clone()
	if strings.TrimSpace(reason) == "" || len(reason) > 1024 || len(s.Revisions) == 0 {
		return errors.New("expansion requires a reason and host contract")
	}
	c := s.Revisions[len(s.Revisions)-1]
	o, ok := s.Outcomes[outcomeID]
	if !ok || o.Gap != gap || o.Binding != c.Binding || o.ContractID != c.ID || !c.valid(now) || o.Completed.After(now) || now.Sub(o.Completed) > 10*time.Minute {
		return errors.New("expansion requires a recent matching indexed outcome")
	}
	for _, f := range s.Fallbacks {
		if f.Outcome.ID == outcomeID {
			if f.Outcome.Revision != c.Revision {
				return errors.New("fallback belongs to a superseded revision")
			}
			return nil
		}
	}
	if o.Revision != c.Revision {
		return errors.New("indexed outcome belongs to a superseded revision")
	}
	if len(s.Fallbacks) >= 64 || len(s.Revisions) >= 128 {
		return errors.New("expansion history full")
	}
	c.Revision++
	s.Revisions = append(s.Revisions, c)
	// The old outcome remains immutable. The capability binds the new revision.
	o.Revision = c.Revision
	expires := now.Add(time.Minute)
	if expires.After(c.Expires) {
		expires = c.Expires
	}
	s.Fallbacks = append(s.Fallbacks, explorationFallback{Outcome: o, Expires: expires, Remaining: 1})
	return l.save(s)
}
