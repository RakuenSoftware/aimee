package db2

import (
	"strings"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
	db2contract "github.com/JBailes/aimee/server-go/db2"
)

func TestCrossKeyPairsComeBackOnce(t *testing.T) {
	// The pairing is symmetric, so a.id < b.id is what stops every pair being
	// returned twice with the sides swapped -- which would halve the useful
	// yield of a sweep that is already bounded by its limit.
	if !strings.Contains(l2CrossKeyPairsQuery, "a.id < b.id") {
		t.Errorf("pairs would be duplicated: %q", l2CrossKeyPairsQuery)
	}
	// The fact-and-decision sweep deliberately does not do this: the two sides
	// are not interchangeable, and their tier ranges differ.
	if strings.Contains(l2FactDecisionPairsQuery, "f.id < d.id") {
		t.Errorf("the fact side is now ordered against the decision side: %q",
			l2FactDecisionPairsQuery)
	}
	if !strings.Contains(l2FactDecisionPairsQuery, "f.id != d.id") {
		t.Errorf("a memory could pair with itself: %q", l2FactDecisionPairsQuery)
	}
}

func TestCrossKeyPrefixSurvivesAKeyWithNoUnderscore(t *testing.T) {
	// STRPOS answers zero when it finds nothing, and SUBSTR of length -1 takes
	// nothing -- so without the concatenated underscore a key like "postgres"
	// would be matched against the empty string, which every row contains.
	if !strings.Contains(l2CrossKeyPairsQuery, "STRPOS(b.key || '_', '_')") ||
		!strings.Contains(l2CrossKeyPairsQuery, "STRPOS(a.key || '_', '_')") {
		t.Errorf("a key with no underscore would match everything: %q",
			l2CrossKeyPairsQuery)
	}
}

func TestPairSweepsReadTheirRows(t *testing.T) {
	store := &fakeStore{rows: &fakeRows{values: [][]any{
		{int64(4), int64(9), "the build is green", "the build is red"},
	}}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeL2CrossKeyPairsRequest(50)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageL2CrossKeyPairs), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	pairs, decodeErr := db2contract.DecodeL2CrossKeyPairsReply(body)
	if decodeErr != nil || len(pairs) != 1 {
		t.Fatalf("pairs = %+v", pairs)
	}
	if pairs[0].MemoryIDA != 4 || pairs[0].MemoryIDB != 9 ||
		pairs[0].ContentA != "the build is green" ||
		pairs[0].ContentB != "the build is red" {
		t.Fatalf("pair = %+v", pairs[0])
	}
	if store.lastArgs[0] != int64(50) {
		t.Errorf("limit = %v", store.lastArgs[0])
	}
}

func TestPairSweepsCannotAskForMoreThanTheReplyCarries(t *testing.T) {
	// The envelope bounds max_pairs at both ends -- one to the reply's row
	// ceiling -- so the clamp inside the operation has nothing to do for these
	// two. It is still there for the prior-in-session read, whose limit admits
	// zero, and this pins where the bound actually lives.
	if _, err := db2contract.EncodeL2FactDecisionPairsRequest(
		uint32(db2contract.L2FactDecisionPairsMaxRows + 1)); err == nil {
		t.Error("a request for more pairs than the reply holds encoded")
	}
	if _, err := db2contract.EncodeL2CrossKeyPairsRequest(0); err == nil {
		t.Error("a request for no pairs at all encoded")
	}
	if limit := pairLimit(0, 200); limit != 200 {
		t.Errorf("pairLimit(0) = %d, want the ceiling", limit)
	}
	if limit := pairLimit(500, 200); limit != 200 {
		t.Errorf("pairLimit(500) = %d, want the ceiling", limit)
	}
	if limit := pairLimit(7, 200); limit != 7 {
		t.Errorf("pairLimit(7) = %d", limit)
	}
}

func TestClustersSkipWhatIsAlreadyFolded(t *testing.T) {
	// merged_into = 0 is what makes the consolidation pass runnable twice: a
	// memory already folded into another is not a candidate for folding again,
	// and without it every pass re-proposes the same clusters.
	store := &fakeStore{rows: &fakeRows{values: [][]any{
		{"session-1", int64(12), 0.25},
	}}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeMemorySummariseClustersRequest(0.4, 3)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageMemorySummariseClusters), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	clusters, decodeErr := db2contract.DecodeMemorySummariseClustersReply(body)
	if decodeErr != nil || len(clusters) != 1 {
		t.Fatalf("clusters = %+v", clusters)
	}
	if clusters[0].SessionID != "session-1" || clusters[0].ClusterCount != 12 ||
		clusters[0].AverageConfidence != 0.25 {
		t.Fatalf("cluster = %+v", clusters[0])
	}
	if !strings.Contains(store.lastSQL, "merged_into = 0") {
		t.Errorf("already-folded memories would be re-proposed: %q", store.lastSQL)
	}
	if !strings.Contains(store.lastSQL, "source_session IS NOT NULL") {
		t.Errorf("memories with no session would form a cluster: %q", store.lastSQL)
	}
	if !strings.Contains(store.lastSQL, "HAVING COUNT(*) >= $2") {
		t.Errorf("HAVING names an alias PostgreSQL rejects there: %q", store.lastSQL)
	}
}

func TestPriorInSessionReadsBackwardsFromTheMemory(t *testing.T) {
	// Identifiers are handed out in order, so id < the one asked about is
	// "before" without needing a timestamp -- and it stays true for two
	// memories written in the same second, which a timestamp would not.
	store := &fakeStore{rows: &fakeRows{values: [][]any{
		{"the build is green", "build-state"},
	}}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeMemoryPriorInSessionRequest("session-1", 900, 5)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageMemoryPriorInSession), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	prior, decodeErr := db2contract.DecodeMemoryPriorInSessionReply(body)
	if decodeErr != nil || len(prior) != 1 {
		t.Fatalf("prior = %+v", prior)
	}
	if prior[0].MemoryContent != "the build is green" ||
		prior[0].MemoryKey != "build-state" {
		t.Fatalf("row = %+v", prior[0])
	}
	if !strings.Contains(store.lastSQL, "id < $2") ||
		!strings.Contains(store.lastSQL, "ORDER BY id DESC") {
		t.Errorf("the read no longer looks backwards: %q", store.lastSQL)
	}
	if store.lastArgs[2] != int64(5) {
		t.Errorf("limit = %v", store.lastArgs[2])
	}
}

func TestPriorInSessionTreatsZeroAsEverythingItCanCarry(t *testing.T) {
	// The C reads a limit of zero or less as "as many as fit", and the envelope
	// admits zero here -- so unlike the pair sweeps this branch is reachable. A
	// literal zero would answer nothing, which reads as a session with no prior
	// memories rather than as a caller who did not say.
	store := &fakeStore{rows: &fakeRows{}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeMemoryPriorInSessionRequest("session-1", 900, 0)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	if _, status := handler(
		invocation(db2contract.StageMemoryPriorInSession), request); status !=
		bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	if store.lastArgs[2] != int64(db2contract.MemoryPriorInSessionMaxRows) {
		t.Errorf("limit = %v, want the reply's ceiling", store.lastArgs[2])
	}
}
