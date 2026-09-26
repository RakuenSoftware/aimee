package memory

import (
	"encoding/json"
	"strings"
	"testing"
	"time"

	"github.com/JBailes/aimee/server-go/bus"
)

func TestHealthQueryTokenIsBoundedSingleUseAndDoesNotExposeQuery(t *testing.T) {
	t.Setenv("AIMEE_MEMORY_HEALTH_ENABLED", "1")
	s := &sourceReleaseState{}
	args := sourceReleaseArgsForHealth("private low entropy query")
	s.captureHealthQueryToken(args)
	token := args.stringOr("_health_query_token", "")
	if !releaseTokenValid(token) || strings.Contains(token, "private") {
		t.Fatal(token)
	}
	if query := s.consumeHealthQuery(token); query != "private low entropy query" {
		t.Fatal(query)
	}
	if s.consumeHealthQuery(token) != "" {
		t.Fatal("query token reused")
	}
	for i := 0; i < 65; i++ {
		s.captureHealthQueryToken(args)
	}
	if len(s.healthQueries) != 64 || args.stringOr("_health_query_token", "") != "" {
		t.Fatal("unbounded query cache")
	}
	for key, row := range s.healthQueries {
		row.Expires = time.Now().Add(-time.Second)
		s.healthQueries[key] = row
	}
	s.captureHealthQueryToken(args)
	if len(s.healthQueries) != 1 {
		t.Fatal("expired query cache not reclaimed")
	}
	args = sourceReleaseArgsForHealth(strings.Repeat("x", 4097))
	s.captureHealthQueryToken(args)
	if args.stringOr("_health_query_token", "") != "" {
		t.Fatal("oversize query captured")
	}
	t.Setenv("AIMEE_MEMORY_HEALTH_ENABLED", "")
	args = sourceReleaseArgsForHealth("query")
	s.captureHealthQueryToken(args)
	if len(args["_health_query_token"]) != 0 {
		t.Fatal("disabled collector captured query")
	}
}

func TestHealthQueryCaptureUsesPrivateScopeKeyAndFailsOpen(t *testing.T) {
	t.Setenv("AIMEE_MEMORY_HEALTH_ENABLED", "1")
	s := &postgresDataStore{placement: PlacementServer, health: &healthOwnerState{keys: map[string]healthKeyCacheEntry{}}}
	options := handlerOptions{placement: PlacementServer, data: s}
	args := sourceReleaseArgs(map[string]any{"principal": "alice", "project": "p", "workspace": "w"})
	cacheKey := releaseDigest([]string{"alice", "p", "w"})
	s.health.keys[cacheKey] = healthKeyCacheEntry{Namespace: "owner", Key: []byte(strings.Repeat("a", 32)), Expires: time.Now().Add(time.Minute)}
	result := captureHealthQuery(options, bus.ModuleInvocation{}, args, "yes", "ingress_query")
	if result == nil || !receiptDigestValid(result.Fingerprint) || result.Project != "p" || result.Workspace != "w" {
		t.Fatal(result)
	}
	raw, _ := json.Marshal(result)
	if strings.Contains(string(raw), "yes") || strings.Contains(string(raw), strings.Repeat("a", 32)) {
		t.Fatal("query or key escaped capture")
	}
	if captureHealthQuery(options, bus.ModuleInvocation{PrincipalRef: 1}, args, "yes", "ingress_query") != nil {
		t.Fatal("module caller captured host scope")
	}
	options.commandContext = &bus.CommandContext{Authenticated: true, Principal: "bob"}
	if captureHealthQuery(options, bus.ModuleInvocation{}, args, "yes", "ingress_query") != nil {
		t.Fatal("foreign caller used cached key")
	}
	options = handlerOptions{placement: PlacementServer}
	if captureHealthQuery(options, bus.ModuleInvocation{}, args, "yes", "ingress_query") != nil {
		t.Fatal("missing optional store invented capture")
	}
}

func TestHealthFingerprintReceiptMetadataIsScopeBound(t *testing.T) {
	state := &sourceReleaseState{}
	args := receiptTestAdmission(t, state)
	plan := sourceReleaseCall(t, state, args)
	var prepared providerReceiptEvent
	json.Unmarshal([]byte(plan["prepared_detail"].(string)), &prepared)
	j, _ := newHealthJournal("owner", "operator", "private", "", time.Now())
	metadata, _ := json.Marshal(map[string]any{"health_context": healthQueryContext{Namespace: "owner", Project: "private", Fingerprint: strings.Repeat("a", 64), Source: "ingress_query"}})
	receipt := inspectedHealthReceipt{Prepared: &prepared, Sequence: "1", Assembly: metadata}
	snapshot, err := healthSnapshotFromReceipt(receipt, j, 1000000)
	if err != nil || snapshot.Invocation.Fingerprint != strings.Repeat("a", 64) {
		t.Fatal(snapshot, err)
	}
	for _, gap := range snapshot.Invocation.MetadataGaps {
		if gap == "query_fingerprint" {
			t.Fatal("available fingerprint reported missing")
		}
	}
	j.Namespace = "different-owner"
	snapshot, err = healthSnapshotFromReceipt(receipt, j, 1000000)
	if err != nil || snapshot.Invocation.Fingerprint != "" {
		t.Fatal("foreign namespace fingerprint accepted", err)
	}
}

func TestHealthNativeMetadataOnlyDescribesRetainedVersions(t *testing.T) {
	version := &MemoryRecordVersion{SchemaVersion: 1, OwnerID: "00000000-0000-4000-8000-000000000001", RecordID: "1", RecordRevision: "2"}
	bundle := nativeRecallFixture()
	bundle.ActiveContext = []RecallRecord{{Record: Record{ID: 1, Kind: "fact", Version: version, Authorship: &PersonalAuthorship{Category: "agent_message"}}, Store: "user"},
		{Record: Record{ID: 2, Kind: "constraint", Version: &MemoryRecordVersion{SchemaVersion: 1, OwnerID: version.OwnerID, RecordID: "2", RecordRevision: "1"}}, Store: "user"}}
	ref := typedProjectionRef{Source: &typedSourceVersion{Kind: "user_memory_record", Version: *version}}
	metadata := nativeHealthRecords(bundle, []typedProjectionRef{ref, ref})
	if len(metadata) != 1 || metadata[0].Kind != "fact" || metadata[0].LowTrust == nil || !*metadata[0].LowTrust || metadata[0].Family != "" {
		t.Fatal(metadata)
	}
	prior, _ := json.Marshal(map[string]any{"health_records": metadata})
	ref.Source.Version.RecordRevision = "3"
	if got := mergeHealthSelectionMetadata(prior, map[string]any{}, []typedProjectionRef{ref}); len(got) != 0 {
		t.Fatal("stale version metadata retained", got)
	}
	if got := mergeHealthSelectionMetadata(prior, map[string]any{}, nil); len(got) != 0 {
		t.Fatal("omitted row metadata retained", got)
	}
}

func TestHealthNativeCaptureDoesNotChangeServingProjection(t *testing.T) {
	bundle := nativeRecallFixture()
	bundle.ActiveContext = recallItems([]Record{{ID: 1, Kind: "fact", Content: "retained evidence", Version: &MemoryRecordVersion{SchemaVersion: 1, OwnerID: "00000000-0000-4000-8000-000000000001", RecordID: "1", RecordRevision: "1"}}})
	raw, _ := json.Marshal(map[string]any{"status": "ok", "recall": bundle})
	args := sourceReleaseArgs(map[string]any{"native_context_bytes": 4096, "task_hint": "private health query", "_health_query_token": strings.Repeat("c", 32)})
	t.Setenv("AIMEE_MEMORY_HEALTH_ENABLED", "")
	before, err := nativeRecallEnvelope(raw, args)
	if err != nil {
		t.Fatal(err)
	}
	t.Setenv("AIMEE_MEMORY_HEALTH_ENABLED", "1")
	after, err := nativeRecallEnvelope(raw, args)
	if err != nil {
		t.Fatal(err)
	}
	var old, new struct {
		Projection nativeRecallProjection `json:"native_context"`
	}
	json.Unmarshal(before, &old)
	json.Unmarshal(after, &new)
	if old.Projection.Text != new.Projection.Text || old.Projection.SelectionDigest != new.Projection.SelectionDigest || old.Projection.Digest != new.Projection.Digest || len(new.Projection.HealthRecords) != 1 {
		t.Fatal("optional metadata changed retained input")
	}
	if strings.Contains(string(after), "private health query") || new.Projection.HealthQueryToken != strings.Repeat("c", 32) {
		t.Fatal("query token transport exposed or lost private text")
	}
}

func TestHealthOptionalMetadataCannotRefuseRequiredReceiptCapacity(t *testing.T) {
	t.Setenv("AIMEE_MEMORY_HEALTH_ENABLED", "1")
	args := sourceReleaseArgs(map[string]any{"request_id": "request", "principal": "owner", "project": "app"})
	assembly := map[string]any{"facts_projection": map[string]any{"retained_items": []typedProjectionRef{releaseTestRef()}}}
	baseline := &sourceReleaseState{}
	if _, err := baseline.prepare(args, assembly); err != nil {
		t.Fatal(err)
	}
	constrained := &sourceReleaseState{bytes: sourceReleaseMaxBytes - baseline.bytes}
	assembly["health_context"] = &healthQueryContext{Namespace: "owner", Project: "app", Fingerprint: strings.Repeat("a", 64), Source: "ingress_query"}
	token, err := constrained.prepare(args, assembly)
	if err != nil {
		t.Fatal("optional telemetry refused required source handle", err)
	}
	if strings.Contains(string(constrained.entries[token].assemblyMetadata), "health_context") || constrained.bytes != sourceReleaseMaxBytes || constrained.healthBytes == 0 || constrained.healthBytes > sourceReleaseHealthMaxBytes {
		t.Fatal("optional metadata consumed required receipt capacity")
	}
	if !strings.Contains(string(receiptMetadataWithHealth(constrained.entries[token])), "health_context") {
		t.Fatal("separately bounded metadata not projected")
	}
	for _, entry := range baseline.entries {
		if constrained.entries[token].assemblyDigest != entry.assemblyDigest {
			t.Fatal("health observations changed required assembly commitment")
		}
	}
	saturated := &sourceReleaseState{bytes: sourceReleaseMaxBytes - baseline.bytes, healthBytes: sourceReleaseHealthMaxBytes}
	token, err = saturated.prepare(args, assembly)
	if err != nil || len(saturated.entries[token].healthMetadata) != 0 {
		t.Fatal("optional pool exhaustion refused serving or exceeded its bound", err)
	}
}

func TestHealthVerifierLabelsRequireFinalSourceCommitment(t *testing.T) {
	state := &sourceReleaseState{}
	plan := sourceReleaseCall(t, state, receiptTestAdmission(t, state))
	var prepared providerReceiptEvent
	json.Unmarshal([]byte(plan["prepared_detail"].(string)), &prepared)
	j, _ := newHealthJournal("owner", "operator", "private", "", time.Now())
	valid := false
	labels := &healthLabelsEnvelope{SourcesDigest: prepared.Binding.SourcesDigest, Labels: &healthSelectionLabels{Verifier: "fixture-policy-v1", Candidates: []healthCandidateLabel{{ID: "fixture-record", Arm: "lexical", Population: "deliveries", Invalid: &valid}}}}
	metadata, _ := json.Marshal(map[string]any{"health_labels": labels})
	receipt := inspectedHealthReceipt{Prepared: &prepared, Sequence: "1", Assembly: metadata}
	snapshot, err := healthSnapshotFromReceipt(receipt, j, 1000000)
	if err != nil || snapshot.Invocation.Labels == nil {
		t.Fatal(snapshot, err)
	}
	labels.SourcesDigest = strings.Repeat("f", 64)
	receipt.Assembly, _ = json.Marshal(map[string]any{"health_labels": labels})
	snapshot, err = healthSnapshotFromReceipt(receipt, j, 1000000)
	if err != nil || snapshot.Invocation.Labels != nil {
		t.Fatal("labels for a different selection were used", err)
	}
}

func BenchmarkHealthWarmQueryCapture(b *testing.B) {
	b.Setenv("AIMEE_MEMORY_HEALTH_ENABLED", "1")
	args := sourceReleaseArgsForHealth("yes")
	args["principal"] = json.RawMessage(`"alice"`)
	key := releaseDigest([]string{"alice", "", ""})
	s := &postgresDataStore{placement: PlacementServer, health: &healthOwnerState{keys: map[string]healthKeyCacheEntry{key: {Namespace: "owner", Key: []byte(strings.Repeat("a", 32)), Expires: time.Now().Add(time.Hour)}}}}
	options := handlerOptions{placement: PlacementServer, data: s}
	b.ReportAllocs()
	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		if captureHealthQuery(options, bus.ModuleInvocation{}, args, "yes", "ingress_query") == nil {
			b.Fatal("capture unavailable")
		}
	}
}
