package memory

import (
	"encoding/json"
	"fmt"
	"strings"
	"testing"
	"time"
)

func journalFixture(t *testing.T) (*healthJournal, healthPopulation, healthSnapshot) {
	t.Helper()
	p, events := healthFixture()
	s, err := newHealthJournal(p.Namespace, p.Principal, p.Project, p.Workspace, p.From)
	if err != nil {
		t.Fatal(err)
	}
	return s, p, healthSnapshot{Invocation: events[0], PreparedSequence: 1, Admitted: true}
}

func TestHealthReceiptReconciliationAndRestartDoNotInflateExposure(t *testing.T) {
	s, p, event := journalFixture(t)
	now := p.Until
	if err := s.apply(event, now); err != nil {
		t.Fatal(err)
	}
	r, err := s.report(p, now)
	if err != nil || r.Attempts["unknown_dispatch"] != 1 || r.Metrics.Invocations != 0 {
		t.Fatal(r, err)
	}
	event.Started = true
	if err = s.apply(event, now); err != nil {
		t.Fatal(err)
	}
	r, err = s.report(p, now)
	if err != nil || r.Attempts["network_uncertain"] != 1 || r.Metrics.Invocations != 0 {
		t.Fatal(r, err)
	}
	raw, _ := json.Marshal(s)
	s, err = decodeHealthJournal(raw)
	if err != nil {
		t.Fatal(err)
	}
	event.Acknowledged = true
	for i := 0; i < 4; i++ {
		if err = s.apply(event, now); err != nil {
			t.Fatal(err)
		}
	}
	r, err = s.report(p, now)
	if err != nil || r.Attempts["dispatched"] != 1 || r.Attempts["unknown_dispatch"] != 0 || r.Records["dispatched"] != 2 || r.Metrics.Records.Occurrences != 2 || !r.Complete {
		t.Fatal(r, err)
	}
	// An out-of-order older snapshot cannot undo observed dispatch.
	event.Started, event.Acknowledged = false, false
	if err = s.apply(event, now); err != nil {
		t.Fatal(err)
	}
	r, err = s.report(p, now)
	if err != nil || r.Attempts["dispatched"] != 1 {
		t.Fatal(r, err)
	}
	public, _ := json.Marshal(r)
	if strings.Contains(string(public), "private_key") || strings.Contains(string(public), "record-secret") {
		t.Fatal("private journal leaked")
	}
}

func TestHealthSamplingUsesWholeInvocationAndExactCountersRemainExact(t *testing.T) {
	s, p, event := journalFixture(t)
	s.Key = []byte(strings.Repeat("k", 32))
	event.Acknowledged = true
	event.Invocation.SamplePPM = 500000
	for i := 0; i < 50; i++ {
		event.Invocation.Attempt = fmt.Sprintf("sample-%d", i)
		event.PreparedSequence = uint64(i + 1)
		if err := s.apply(event, p.Until); err != nil {
			t.Fatal(err)
		}
		stored := s.Attempts[event.Invocation.Attempt]
		if stored.Sampled && len(stored.Invocation.Records) != 3 || !stored.Sampled && stored.Invocation.Records != nil {
			t.Fatal("sampled individual records")
		}
	}
	r, err := s.report(p, p.Until)
	if err != nil || r.Attempts["dispatched"] != 50 || r.Records["dispatched"] != 100 || r.Sampled == 0 || r.NotSampled == 0 || r.Sampled+r.NotSampled != 50 || r.Metrics.Records.Occurrences != 2*r.Sampled {
		t.Fatal(r, err)
	}
}

func TestHealthOverflowRetentionAndReplayExposeIncompleteWindows(t *testing.T) {
	s, p, event := journalFixture(t)
	event.Acknowledged = true
	for i := 0; i <= healthMaxAttempts; i++ {
		event.Invocation.Attempt = fmt.Sprint(i)
		event.PreparedSequence = uint64(i + 1)
		if err := s.apply(event, p.Until); err != nil {
			t.Fatal(err)
		}
	}
	r, err := s.report(p, p.Until)
	if err != nil || r.Complete || r.Evicted != 1 || !r.Gap || r.Attempts["dispatched"] != healthMaxAttempts {
		t.Fatal(r, err)
	}
	event.Invocation.Attempt = "0"
	event.PreparedSequence = 1
	if err = s.apply(event, p.Until); err != nil {
		t.Fatal(err)
	}
	if len(s.Attempts) != healthMaxAttempts || s.Evicted != 1 {
		t.Fatal("old replay re-entered bounded window")
	}
	late := p.Until.Add(healthRetention)
	if err = s.apply(event, late); err != nil {
		t.Fatal(err)
	}
	if len(s.Attempts) != 0 || s.Expired != healthMaxAttempts {
		t.Fatal("retention failed")
	}
}

func TestHealthRejectsForgedScopeConflictAndAmbiguousPersistence(t *testing.T) {
	s, p, event := journalFixture(t)
	for _, change := range []func(*healthSnapshot){
		func(e *healthSnapshot) { e.Invocation.Principal = "bob" },
		func(e *healthSnapshot) { e.Invocation.Namespace = "other" },
		func(e *healthSnapshot) { e.Invocation.Project = "other" },
		func(e *healthSnapshot) { e.Invocation.Workspace = "other" },
		func(e *healthSnapshot) { e.Acknowledged = true; e.Admitted = false },
		func(e *healthSnapshot) { e.ResolvedUnsent = true },
		func(e *healthSnapshot) { e.Invocation.At = p.Until.Add(time.Second) },
	} {
		e := event
		change(&e)
		if err := s.apply(e, p.Until); err == nil || len(s.Attempts) != 0 {
			t.Fatal("invalid observation mutated state")
		}
	}
	if err := s.apply(event, p.Until); err != nil {
		t.Fatal(err)
	}
	event.Invocation.Binding = "changed"
	if err := s.apply(event, p.Until); err == nil {
		t.Fatal("changed receipt binding accepted")
	}
	foreign := p
	foreign.Principal = "bob"
	if _, err := s.report(foreign, p.Until); err == nil {
		t.Fatal("foreign health read accepted")
	}
	raw, _ := json.Marshal(s)
	bad := append([]byte(`{"version":1,`), raw[1:]...)
	if _, err := decodeHealthJournal(bad); err == nil {
		t.Fatal("duplicate persistence fields accepted")
	}
	if _, err := decodeHealthJournal([]byte(`{}`)); err == nil {
		t.Fatal("corrupt state became an empty window")
	}
}

func TestHealthUnsentRequiresResolvedDispatcherAndNoAdmission(t *testing.T) {
	s, p, event := journalFixture(t)
	event.Admitted = false
	if err := s.apply(event, p.Until); err != nil {
		t.Fatal(err)
	}
	r, err := s.report(p, p.Until)
	if err != nil || r.Attempts["unknown_dispatch"] != 1 {
		t.Fatal(r, err)
	}
	event.ResolvedUnsent = true
	if err = s.apply(event, p.Until); err != nil {
		t.Fatal(err)
	}
	r, err = s.report(p, p.Until)
	if err != nil || r.Attempts["assembled_unsent"] != 1 || r.Metrics.Invocations != 0 {
		t.Fatal(r, err)
	}
	event.ResolvedUnsent = false
	event.Admitted = true
	if err = s.apply(event, p.Until); err == nil {
		t.Fatal("contradicted a resolved unsent receipt")
	}
}

func TestHealthPersistedScopeAndSelectionCorruptionCannotLookEmpty(t *testing.T) {
	for _, mutate := range []func(*healthStoredAttempt){
		func(e *healthStoredAttempt) { e.Invocation.Principal = "bob" },
		func(e *healthStoredAttempt) { e.Invocation.Records = nil },
		func(e *healthStoredAttempt) { e.RecordCount++ },
		func(e *healthStoredAttempt) { e.Invocation.Stage = "dispatched" },
		func(e *healthStoredAttempt) { e.Admitted = false; e.Started = true },
	} {
		s, p, event := journalFixture(t)
		if err := s.apply(event, p.Until); err != nil {
			t.Fatal(err)
		}
		entry := s.Attempts[event.Invocation.Attempt]
		mutate(&entry)
		s.Attempts[event.Invocation.Attempt] = entry
		raw, _ := json.Marshal(s)
		if _, err := decodeHealthJournal(raw); err == nil {
			t.Fatal("corrupt private journal accepted")
		}
	}
}

// Domain-only overhead. Deployment acceptance must additionally measure the
// authenticated transport, receipt reads and persistent owner transaction.
func BenchmarkHealthJournalAtCapacity(b *testing.B) {
	p, events := healthFixture()
	s, err := newHealthJournal(p.Namespace, p.Principal, p.Project, p.Workspace, p.From)
	if err != nil {
		b.Fatal(err)
	}
	for i := 0; i < healthMaxAttempts; i++ {
		e := healthSnapshot{Invocation: events[0], PreparedSequence: uint64(i + 1), Admitted: true, Acknowledged: true}
		e.Invocation.Attempt = fmt.Sprint(i)
		if err = s.apply(e, p.Until); err != nil {
			b.Fatal(err)
		}
	}
	b.ReportAllocs()
	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		e := healthSnapshot{Invocation: events[0], PreparedSequence: uint64(healthMaxAttempts + i + 1), Admitted: true, Acknowledged: true}
		e.Invocation.Attempt = fmt.Sprint(healthMaxAttempts + i)
		if err = s.apply(e, p.Until); err != nil {
			b.Fatal(err)
		}
	}
	b.StopTimer()
	raw, _ := json.Marshal(s)
	b.ReportMetric(float64(len(raw)), "retained_bytes")
}

func TestHealthOldCapacityLossDoesNotPoisonLaterWindows(t *testing.T) {
	s, p, event := journalFixture(t)
	now := p.Until
	s.markGap(event.Invocation.At, now)
	p.From = event.Invocation.At.Add(time.Second)
	report, err := s.report(p, now)
	if err != nil || !report.Complete || report.Gap || report.Metrics.Records.HHI != nil {
		t.Fatal(report, err)
	}
	p.From = event.Invocation.At
	report, err = s.report(p, now)
	if err != nil || report.Complete || !report.Gap {
		t.Fatal(report, err)
	}
	// A journal written before loss boundaries existed cannot infer a safe past.
	s.GapUntil = nil
	s.markGap(event.Invocation.At, now)
	if s.GapUntil == nil || !s.GapUntil.After(now) {
		t.Fatal(s.GapUntil)
	}
}
