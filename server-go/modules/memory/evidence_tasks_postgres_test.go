package memory

import (
	"context"
	"encoding/json"
	"fmt"
	"os"
	"testing"
	"time"

	"github.com/JBailes/aimee/server-go/bus"
	"github.com/jackc/pgx/v5"
)

func TestEvidenceTemporalCanonicalOwnerPostgres(t *testing.T) {
	dsn := os.Getenv("AIMEE_DB2_REPLAY_URL")
	if dsn == "" {
		t.Skip("set AIMEE_DB2_REPLAY_URL")
	}
	ctx := context.Background()
	conn, err := pgx.Connect(ctx, dsn)
	if err != nil {
		t.Fatal(err)
	}
	defer conn.Close(ctx)
	tx, err := conn.Begin(ctx)
	if err != nil {
		t.Fatal(err)
	}
	defer tx.Rollback(ctx)
	if _, err = tx.Exec(ctx, `SET LOCAL jit=off; SELECT set_config('aimee.memory_scope_all','1',true)`); err != nil {
		t.Fatal(err)
	}
	s := &postgresDataStore{db: runtimeRoleTx{evalQueryer{tx}, t}, placement: PlacementKB}
	subject := fmt.Sprintf("temporal-mr05-%d", time.Now().UnixNano())
	actor := FactActor{Principal: "test:mr05", TransportIdentity: "test", Role: "user", Rank: 30, Authenticated: 1}
	for i, value := range []string{"old database", "current database"} {
		_, err = s.assertFact(ctx, factAssertion{Functional: true, FactCandidate: FactCandidate{Subject: subject, Relation: "uses", Object: value, AssertionKind: "world_fact", ValidFrom: fmt.Sprintf("202%d-01-01", i+4), Actor: actor, Evidence: FactEvidence{SourceKind: "observation", SourceID: value, Stance: "supports"}}})
		if err != nil {
			t.Fatal(err)
		}
	}
	r := coverageFixture(t)
	r.Requirements.QueryMode = "temporal_change"
	r.Requirements.Obligations[0].Subject = subject
	r.Channels["historical_assertions"].Enabled = true
	r.Channels["historical_assertions"].Status = "ok"
	req := DataRequest{TypedContext: &typedContextOptions{Requirements: r.Requirements}, Assertions: &assertionSearchRequest{Historical: true}}
	hits, err := s.assertionCandidates(ctx, req, Scope{}, subject, 32, "")
	if err != nil {
		t.Fatal(err)
	}
	if len(hits) != 2 {
		t.Fatalf("canonical predecessor not available under explicit history: %+v", hits)
	}
	for _, h := range hits {
		channel := "current_assertions"
		if h.Historical {
			channel = "historical_assertions"
		}
		source := h.sourceVersion()
		source.ReadPolicy = &sourceReadPolicy{Historical: true}
		r.add(channel, typedItem{id: h.StableID, value: h, text: h.Rendered, source: source})
	}
	if err = r.finish(); err != nil || r.Sufficiency != "complete" {
		t.Fatal(err, r.Coverage, hits)
	}
	// Revocation is checked by the same source owner used by the dispatch fence;
	// retaining the old projection or its complete verdict cannot authorize reuse.
	check := &sourceRevalidation{SchemaVersion: 1, CheckID: "0123456789abcdef0123456789abcdef", Sources: r.Retained}
	if eligible, err := s.revalidateSources(ctx, check, Scope{}); err != nil || !eligible {
		t.Fatal(eligible, err)
	}
	if n, err := s.invalidateFacts(ctx, actor, subject, "uses", "current database"); err != nil || n != 1 {
		t.Fatal(n, err)
	}
	if eligible, err := s.revalidateSources(ctx, check, Scope{}); err != nil || eligible {
		t.Fatal("old complete projection survived revocation", eligible, err)
	}
	// User/model request fields cannot supply the authenticated host grant.
	cfg := typedTestOptions(t, `{}`)
	cfg.Requirements = r.Requirements
	cfg.Requirements.Recovery = &evidenceRecoveryBudget{MaxRounds: 1, MaxItems: 1, MaxTokens: 512, MaxElapsedMS: 100}
	cfg.ExecuteRecovery = true
	body, _ := json.Marshal(DataRequest{Operation: "typed-context", TypedContext: cfg, Assertions: &assertionSearchRequest{}, Query: subject, Limit: 32})
	outer := &postgresDataStore{db: runtimeRoleDB{evalQueryer{tx}, t}, placement: PlacementKB}
	for _, caller := range []*bus.CommandContext{nil, {Authenticated: true, Principal: "model", UserAuthority: false}, {Principal: "forged", UserAuthority: true}} {
		_, status := handleData(handlerOptions{placement: PlacementKB, data: outer, commandContext: caller}, bus.ModuleInvocation{}, body)
		if status != bus.ModuleStatusInvalidRequest {
			t.Fatal("unadmitted recovery", status)
		}
	}
}
