package db2

import (
	"crypto/sha256"
	"encoding/hex"
	"strings"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
	db2contract "github.com/JBailes/aimee/server-go/db2"
)

func TestProjectionEdgesReadOnlyThePublishedGraph(t *testing.T) {
	// A pending generation's edges are real rows; they are just not the answer
	// to what this project looks like. Without the state filter a half-finished
	// projection would be indistinguishable from the published one.
	store := &fakeStore{rows: &fakeRows{values: [][]any{
		{"src/a.c", "defines", "main"},
	}}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeProjectionEdgesRequest("aimee", 64)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageProjectionEdges), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	edges, decodeErr := db2contract.DecodeProjectionEdgesReply(body)
	if decodeErr != nil || len(edges) != 1 || edges[0].StructuralWeight != 3 {
		t.Fatalf("edges = %+v", edges)
	}
	if !strings.Contains(store.lastSQL, "g.state = 'visible'") {
		t.Errorf("an unpublished generation could answer: %q", store.lastSQL)
	}
	if !strings.Contains(store.lastSQL, "p.lifecycle_state = 'current'") {
		t.Errorf("a detached project could answer: %q", store.lastSQL)
	}
}

func TestGenerationEdgesTruncateDeterministically(t *testing.T) {
	// The C explains this one: if the LIMIT boundary cuts through edges sharing
	// a source and target, an incomplete order makes the truncation depend on
	// row ordering -- and the community partition derived from these edges
	// would depend on it too.
	if !strings.Contains(projectionEdgesForGenerationQuery,
		"ORDER BY source, target, relation") {
		t.Errorf("the order is no longer total: %q",
			projectionEdgesForGenerationQuery)
	}
	// The project read orders by two columns, which is the C's difference and
	// is left alone: widening it changes which rows a truncated read returns.
	if strings.Contains(projectionEdgesQuery, "cpe.relation\n") {
		t.Errorf("the project read's order was widened: %q", projectionEdgesQuery)
	}
}

func TestStructuralWeightRanksDefinitionAboveUse(t *testing.T) {
	// A symbol's definition is where it lives; a call is one of many.
	if structuralWeight("defines") != 3 {
		t.Error("a definition no longer outweighs everything")
	}
	for _, relation := range []string{"contains", "exports", "routes", "depends_on"} {
		if structuralWeight(relation) != 2 {
			t.Errorf("%s is no longer a structural relation", relation)
		}
	}
	if structuralWeight("calls") != 1 || structuralWeight("imports") != 1 {
		t.Error("a use no longer weighs one")
	}
	// Unknown is not worthless: a new relation type appearing should not make
	// the edges carrying it vanish from a weighted walk.
	if structuralWeight("something_new") != 1 {
		t.Error("an unweighted relation would disappear from a weighted walk")
	}
}

func TestCodeIndexOpCountsItsAttempts(t *testing.T) {
	// The attempt count is what a retry policy reads, and it only grows on the
	// conflict path -- an insert starts it at one.
	store := &fakeStore{}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeCodeIndexOpRecordRequest(
		900, "aimee", "node:main", "src/a.c", 0, "the extractor timed out")
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageCodeIndexOpRecord), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	recorded, decodeErr := db2contract.DecodeCodeIndexOpRecordReply(body)
	if decodeErr != nil || recorded != 1 {
		t.Fatalf("recorded = %d", recorded)
	}
	if !strings.Contains(store.lastSQL, "attempts   = code_index_ops.attempts + 1") {
		t.Errorf("the attempt count no longer grows: %q", store.lastSQL)
	}
	if store.lastArgs[4] != "failed" ||
		store.lastArgs[5] != "the extractor timed out" {
		t.Errorf("args = %v", store.lastArgs)
	}
	// A failed attempt indexed nothing, so it carries no index time.
	if !strings.Contains(store.lastSQL, "CASE WHEN $5 = 'ok' THEN pg_now_text() ELSE NULL END") {
		t.Errorf("a failure would claim an index time: %q", store.lastSQL)
	}
}

func TestAuditRowHashIsLengthPrefixedUnderADomain(t *testing.T) {
	// Without the length prefixes a record with an action of "ab" and a subject
	// of "c" would hash the same as one with "a" and "bc" -- content could move
	// between fields without changing the digest.
	moved := auditRowHash(1, "", "", "ab", "c", "", "", "", auditWormGenesisPrev)
	other := auditRowHash(1, "", "", "a", "bc", "", "", "", auditWormGenesisPrev)
	if moved == other {
		t.Fatal("content can move between fields without changing the hash")
	}

	// The exact construction, computed independently: the domain, the previous
	// hash, then each of the eight fields as its length, a colon and its bytes.
	want := sha256.Sum256([]byte(
		"aimee.audit.worm.v1\n" + auditWormGenesisPrev + "\n" +
			"1:1" + "5:admin" + "0:" + "6:reject" + "0:" + "0:" + "0:" + "0:"))
	got := auditRowHash(1, "admin", "", "reject", "", "", "", "", auditWormGenesisPrev)
	if got != hex.EncodeToString(want[:]) {
		t.Fatalf("hash = %s, want %s", got, hex.EncodeToString(want[:]))
	}
}

func TestAuditAppendLinksToTheChainTail(t *testing.T) {
	// The sequence number and the previous hash both come from the tail, so a
	// row that linked to anything else would break verification at exactly one
	// point in the chain -- which is what verification is for.
	store := &fakeStore{rowQueue: []*fakeRow{
		{values: []any{int64(41), strings.Repeat("ab", 32)}},
	}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeKBAuditAppendRequest(
		"admin", "jbailes", "reject", "artifact-1", "denied", "wrong scope")
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageKBAuditAppend), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	acknowledged, decodeErr := db2contract.DecodeKBAuditAppendReply(body)
	if decodeErr != nil || acknowledged != 1 {
		t.Fatalf("acknowledged = %d", acknowledged)
	}
	if store.txCalls != 1 || !store.committed {
		t.Fatalf("transactions = %d, committed = %v", store.txCalls, store.committed)
	}
	if store.argsLog[1][0] != int64(42) {
		t.Errorf("sequence = %v, want the tail plus one", store.argsLog[1][0])
	}
	if store.argsLog[1][7] != strings.Repeat("ab", 32) {
		t.Errorf("previous hash = %v", store.argsLog[1][7])
	}
	want := auditRowHash(42, "admin", "jbailes", "reject", "artifact-1",
		"denied", "", "wrong scope", strings.Repeat("ab", 32))
	if store.argsLog[1][8] != want {
		t.Errorf("row hash = %v, want %s", store.argsLog[1][8], want)
	}
}

func TestAuditAppendStartsFromGenesis(t *testing.T) {
	// An empty table is the genesis case rather than a failure.
	store := &fakeStore{}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeKBAuditAppendRequest(
		"admin", "jbailes", "reject", "", "", "")
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	if _, status := handler(
		invocation(db2contract.StageKBAuditAppend), request); status !=
		bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	if store.argsLog[1][0] != int64(1) {
		t.Errorf("sequence = %v, want one", store.argsLog[1][0])
	}
	if store.argsLog[1][7] != auditWormGenesisPrev {
		t.Errorf("previous hash = %v, want the genesis value", store.argsLog[1][7])
	}
	// key_id is written empty and is part of the hashed record, so writing
	// anything else here would break verification for every later row.
	if !strings.Contains(store.sqlLog[1], "$6, $7, '', $8, $9") {
		t.Errorf("the key id is no longer written empty: %q", store.sqlLog[1])
	}
}

func TestAuditAppendRefusesAnActionlessRow(t *testing.T) {
	// An audit row that does not say what happened is not evidence of anything.
	store := &fakeStore{}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeKBAuditAppendRequest(
		"admin", "jbailes", "", "artifact-1", "denied", "")
	if err != nil {
		// The envelope may refuse it first, which is the same answer earlier.
		return
	}
	body, status := handler(invocation(db2contract.StageKBAuditAppend), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	acknowledged, decodeErr := db2contract.DecodeKBAuditAppendReply(body)
	if decodeErr != nil || acknowledged != 0 {
		t.Fatalf("acknowledged = %d, want 0", acknowledged)
	}
	if store.txCalls != 0 {
		t.Errorf("a transaction was opened for a row that cannot be written")
	}
}

func TestUntriedArmReadsAsZeros(t *testing.T) {
	// There is no found flag in this reply, and for a bandit an untried arm and
	// one tried and never rewarded are close enough to the same thing: both
	// mean there is nothing to prefer it on.
	store := &fakeStore{}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeBanditArmStatsReadRequest("recall", "arm-a")
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageBanditArmStatsRead), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	decisions, rewards, sum, alpha, beta, decodeErr :=
		db2contract.DecodeBanditArmStatsReadReply(body)
	if decodeErr != nil || decisions != 0 || rewards != 0 || sum != 0 ||
		alpha != 0 || beta != 0 {
		t.Fatalf("stats = %d %d %v %v %v", decisions, rewards, sum, alpha, beta)
	}
}

func TestArmStatsComeBackAsStored(t *testing.T) {
	// The posteriors are the arm's parameters, which a sampler updates on its
	// own schedule. Deriving them from the counts would answer a different
	// question from the one the table holds.
	store := &fakeStore{row: &fakeRow{values: []any{
		int64(40), int64(12), 9.5, 13.0, 29.0,
	}}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeBanditArmStatsReadRequest("recall", "arm-a")
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageBanditArmStatsRead), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	decisions, rewards, sum, alpha, beta, decodeErr :=
		db2contract.DecodeBanditArmStatsReadReply(body)
	if decodeErr != nil || decisions != 40 || rewards != 12 || sum != 9.5 ||
		alpha != 13 || beta != 29 {
		t.Fatalf("stats = %d %d %v %v %v", decisions, rewards, sum, alpha, beta)
	}
}
