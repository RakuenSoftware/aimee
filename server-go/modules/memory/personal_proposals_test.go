package memory

import (
	"context"
	"encoding/json"
	"fmt"
	"os"
	"strings"
	"testing"
	"time"

	"github.com/JBailes/aimee/server-go/bus"
	"github.com/jackc/pgx/v5"
)

func TestPersonalCorrectionReview(t *testing.T) {
	dsn := os.Getenv("AIMEE_MEMORY_EVAL_URL")
	if dsn == "" {
		if os.Getenv("AIMEE_MEMORY_EVAL_REQUIRED") == "1" {
			t.Fatal("AIMEE_MEMORY_EVAL_URL required")
		}
		t.Skip("set AIMEE_MEMORY_EVAL_URL")
	}
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()
	conn, err := pgx.Connect(ctx, dsn)
	if err != nil {
		t.Fatal(err)
	}
	defer conn.Close(context.Background())
	name := fmt.Sprintf("private_proposals_%d", time.Now().UnixNano())
	schema, role := pgx.Identifier{name}.Sanitize(), pgx.Identifier{name + "_runtime"}.Sanitize()
	exec := func(sql string, args ...any) {
		t.Helper()
		if _, err := conn.Exec(ctx, sql, args...); err != nil {
			t.Fatal(err)
		}
	}
	exec("CREATE SCHEMA " + schema + "; CREATE ROLE " + role + " NOINHERIT NOBYPASSRLS; GRANT USAGE ON SCHEMA " + schema + " TO " + role + "; ALTER DEFAULT PRIVILEGES IN SCHEMA " + schema + " GRANT ALL ON TABLES TO " + role + "; ALTER DEFAULT PRIVILEGES IN SCHEMA " + schema + " GRANT EXECUTE ON FUNCTIONS TO " + role + "; SET search_path=" + schema + ",public")
	defer func() {
		_, err := conn.Exec(context.Background(), "DROP SCHEMA "+schema+" CASCADE; DROP ROLE "+role)
		if err != nil {
			t.Error(err)
		}
	}()
	read := func(name string) string {
		t.Helper()
		b, err := os.ReadFile("../aimee/families/" + name)
		if err != nil {
			t.Fatal(err)
		}
		return string(b)
	}
	base := read("schema_conversation.sql")
	exec(base[strings.Index(base, "CREATE TABLE IF NOT EXISTS user_memories ("):strings.Index(base, "CREATE INDEX IF NOT EXISTS user_memories_recall")])
	for _, name := range []string{"changes", "versions", "acl", "authority", "proposals"} {
		exec(read("schema_personal_memory_" + name + ".sql"))
	}
	human := &bus.CommandContext{Authenticated: true, UserAuthority: true, Principal: "fixture:human", TransportIdentity: "fixture:http"}
	model := &bus.CommandContext{Authenticated: true, Principal: "fixture:model", TransportIdentity: "fixture:mcp"}
	begin := func(c *pgx.Conn) pgx.Tx {
		t.Helper()
		tx, err := c.Begin(ctx)
		if err != nil {
			t.Fatal(err)
		}
		if _, err = tx.Exec(ctx, "SET LOCAL search_path="+schema+",public; SET LOCAL ROLE "+role); err != nil {
			t.Fatal(err)
		}
		return tx
	}
	invoke := func(tx pgx.Tx, caller *bus.CommandContext, req DataRequest) (DataResponse, bus.ModuleStatus) {
		t.Helper()
		backend, err := NewPostgresDataStore(runtimeRoleDB{evalQueryer{tx}, t}, PlacementServer)
		if err != nil {
			t.Fatal(err)
		}
		raw, status := handleData(handlerOptions{placement: PlacementServer, data: backend, commandContext: caller}, bus.ModuleInvocation{StageID: StageData}, dataRequest(t, req))
		var out DataResponse
		if status == bus.ModuleStatusOK && json.Unmarshal(raw, &out) != nil {
			t.Fatal("bad response")
		}
		return out, status
	}
	call := func(caller *bus.CommandContext, req DataRequest) DataResponse {
		t.Helper()
		tx := begin(conn)
		defer tx.Rollback(context.Background())
		out, status := invoke(tx, caller, req)
		if status != bus.ModuleStatusOK {
			t.Fatalf("%s status %v", req.Operation, status)
		}
		if err := tx.Commit(ctx); err != nil {
			t.Fatal(err)
		}
		return out
	}
	scalar := func(sql string) int64 {
		t.Helper()
		var n int64
		if err := conn.QueryRow(ctx, sql).Scan(&n); err != nil {
			t.Fatal(err)
		}
		return n
	}
	one := func(out DataResponse) Record {
		t.Helper()
		if len(out.Records) != 1 {
			t.Fatal(out)
		}
		return out.Records[0]
	}
	certainty := 1.0
	create := func(key string) Record {
		t.Helper()
		return one(call(human, DataRequest{Operation: "store", Authority: AuthorityUser, Key: key, Kind: "fact", Tier: "L2", Content: "original assertion", Confidence: &certainty}))
	}
	target := create("private-review")
	original := one(call(nil, DataRequest{Operation: "get", ID: target.ID, IncludeVersion: true}))
	proposed := DataRequest{Operation: "supersede", ID: target.ID, Content: "model correction draft", Confidence: &certainty, ExpectedVersion: original.Version}
	before := scalar(`SELECT generation FROM user_memory_collection_generation`)
	refused := call(model, proposed)
	if refused.Code == nil || *refused.Code != MutationReviewRequired || refused.Proposal == nil || refused.Proposal.Draft != nil {
		t.Fatal(refused)
	}
	proposal := *refused.Proposal
	if scalar(`SELECT count(*) FROM user_memory_correction_proposals`) != 1 || scalar(`SELECT generation FROM user_memory_collection_generation`) != before || scalar(`SELECT count(*) FROM user_memory_versions`) != 0 {
		t.Fatal("proposal altered serving state or failed to commit")
	}
	retry := call(model, proposed)
	if retry.Proposal == nil || retry.Proposal.ID != proposal.ID {
		t.Fatal("draft retry lost identity", retry)
	}
	inspect := call(nil, DataRequest{Operation: "correction-proposals", ProposalID: proposal.ID, Limit: 1})
	var inspection struct {
		Proposals []correctionProposal `json:"proposals"`
	}
	if json.Unmarshal(inspect.Payload, &inspection) != nil || len(inspection.Proposals) != 1 || inspection.Proposals[0].Draft.Content != proposed.Content || inspection.Proposals[0].Draft.Confidence != .8 {
		t.Fatal(string(inspect.Payload))
	}
	if got := one(call(nil, DataRequest{Operation: "get", ID: target.ID})); got.Content != target.Content {
		t.Fatal("draft served as memory")
	}
	review := correctionReviewRequest{ProposalID: proposal.ID, Digest: proposal.Digest, Expected: proposal.Target, Action: "approve"}
	request := DataRequest{Operation: "correction-review", CorrectionReview: &review}
	tx := begin(conn)
	if _, status := invoke(tx, model, request); status != bus.ModuleStatusInvalidRequest {
		t.Fatal("model approved private draft", status)
	}
	_ = tx.Rollback(ctx)
	mismatched := review
	mismatched.Digest = strings.Repeat("0", 64)
	bad := call(human, DataRequest{Operation: "correction-review", CorrectionReview: &mismatched})
	if !strings.Contains(string(bad.Payload), `"kind":"conflict"`) {
		t.Fatal("mismatched draft approved", string(bad.Payload))
	}
	// Late decision failure must roll back canonical payload, history and outbox.
	exec(`ALTER TABLE user_memory_correction_proposals ADD CONSTRAINT fixture_review_failure CHECK(state='pending')`)
	tx = begin(conn)
	if _, status := invoke(tx, human, request); status != bus.ModuleStatusInternal {
		t.Fatal("late decision failure accepted", status)
	}
	_ = tx.Rollback(ctx)
	if scalar(`SELECT generation FROM user_memory_collection_generation`) != before || scalar(`SELECT count(*) FROM user_memory_versions`) != 0 {
		t.Fatal("decision failure partially committed")
	}
	exec(`ALTER TABLE user_memory_correction_proposals DROP CONSTRAINT fixture_review_failure`)
	extract := func(out DataResponse) correctionProposal {
		t.Helper()
		var result struct {
			Proposal correctionProposal `json:"proposal"`
		}
		if json.Unmarshal(out.Payload, &result) != nil || result.Proposal.ID == "" {
			t.Fatal(out)
		}
		return result.Proposal
	}
	approved := extract(call(human, request))
	if approved.State != "approved" || approved.Reviewer != human.Principal || approved.Result == nil || approved.Result.RecordID != proposal.Target.RecordID || approved.Actor != model.Principal || approved.OriginAuthority != "model" {
		t.Fatal(approved)
	}
	current := one(call(nil, DataRequest{Operation: "get", ID: target.ID, IncludeVersion: true}))
	if current.Authorship.Category != "reviewed_model" || current.Authorship.Principal != model.Principal || current.Authorship.Reviewer != human.Principal || current.Authorship.ProposalID != proposal.ID || current.Confidence != .8 || current.Content != proposed.Content {
		t.Fatal(current)
	}
	old := one(call(nil, DataRequest{Operation: "get", ID: target.ID, AtVersion: original.Version}))
	if old.Content != target.Content || old.Authorship.Category != "user_stated" {
		t.Fatal("review lost prior author", old)
	}
	// Exercise the host-only runtime envelope as used by HTTP forwarding.
	runtimeArgs, _ := json.Marshal(map[string]any{"operation": "user-correction-review", "store": "user", "proposal_id": review.ProposalID, "payload_digest": review.Digest, "expected_version": review.Expected, "action": "approve", "principal": "forged", "authority": "user"})
	tx = begin(conn)
	runtimeBackend, _ := NewPostgresDataStore(runtimeRoleDB{evalQueryer{tx}, t}, PlacementServer)
	runtimeHandler := NewHandler(nil, WithDataStore(PlacementServer, runtimeBackend))
	if _, status := invokeContextCommand(t, runtimeHandler, 41, *human, "runtime", string(runtimeArgs)); status != bus.ModuleStatusInvalidRequest {
		t.Fatal("plugin forged host review context", status)
	}
	for _, caller := range []bus.CommandContext{*model, *human} {
		envelope, status := invokeContextCommand(t, runtimeHandler, 0, caller, "runtime", string(runtimeArgs))
		inner, _ := envelope["json"].(string)
		if status != bus.ModuleStatusOK {
			t.Fatal(status)
		}
		if caller.UserAuthority {
			if !strings.Contains(inner, `"replayed":true`) || !strings.Contains(inner, `"store":"user"`) {
				t.Fatal("private review transport lost result", inner)
			}
		} else if !strings.Contains(inner, `"kind":"forbidden"`) {
			t.Fatal("model approved through runtime envelope", inner)
		}
	}
	if err := tx.Commit(ctx); err != nil {
		t.Fatal(err)
	}
	before = scalar(`SELECT generation FROM user_memory_collection_generation`)
	if p := extract(call(human, request)); !p.Replayed || p.DecisionID != approved.DecisionID {
		t.Fatal("review retry repeated effect", p)
	}
	if scalar(`SELECT generation FROM user_memory_collection_generation`) != before {
		t.Fatal("review retry changed outbox")
	}
	// Reviewed model content remains protected from automatic model replacement.
	proposed.ExpectedVersion = current.Version
	proposed.Content = "another model suggestion"
	another := call(model, proposed)
	if another.Proposal == nil {
		t.Fatal("reviewed model became automatically replaceable", another)
	}
	rejectedRequest := correctionReviewRequest{ProposalID: another.Proposal.ID, Digest: another.Proposal.Digest, Expected: another.Proposal.Target, Action: "reject"}
	rejected := extract(call(human, DataRequest{Operation: "correction-review", CorrectionReview: &rejectedRequest}))
	if rejected.State != "rejected" || rejected.Result != nil {
		t.Fatal(rejected)
	}
	again := call(model, proposed)
	if again.Proposal == nil || again.Proposal.ID != rejected.ID || again.Proposal.State != "rejected" {
		t.Fatal("rejected suggestion reopened", again)
	}
	if scalar(`SELECT generation FROM user_memory_collection_generation`) != before {
		t.Fatal("rejection changed canonical state")
	}
	// Concurrent identical review decisions serialize on the canonical parent.
	other := create("concurrent-review")
	draft := call(model, DataRequest{Operation: "supersede", ID: other.ID, Content: "race review draft", Confidence: &certainty}).Proposal
	if draft == nil {
		t.Fatal("missing concurrent draft")
	}
	raceReview := correctionReviewRequest{ProposalID: draft.ID, Digest: draft.Digest, Expected: draft.Target, Action: "approve"}
	raceRequest := DataRequest{Operation: "correction-review", CorrectionReview: &raceReview}
	second, err := pgx.Connect(ctx, dsn)
	if err != nil {
		t.Fatal(err)
	}
	defer second.Close(context.Background())
	observer, err := pgx.Connect(ctx, dsn)
	if err != nil {
		t.Fatal(err)
	}
	defer observer.Close(context.Background())
	firstTx, secondTx := begin(conn), begin(second)
	defer firstTx.Rollback(context.Background())
	defer secondTx.Rollback(context.Background())
	first, status := invoke(firstTx, human, raceRequest)
	if status != bus.ModuleStatusOK {
		t.Fatal(status)
	}
	type result struct {
		out    DataResponse
		status bus.ModuleStatus
	}
	done := make(chan result, 1)
	go func() { out, status := invoke(secondTx, human, raceRequest); done <- result{out, status} }()
	for {
		var blocked bool
		if err := observer.QueryRow(ctx, `SELECT $1=ANY(pg_blocking_pids($2))`, conn.PgConn().PID(), second.PgConn().PID()).Scan(&blocked); err != nil {
			t.Fatal(err)
		}
		if blocked {
			break
		}
		select {
		case r := <-done:
			t.Fatal("review did not serialize", r)
		case <-ctx.Done():
			t.Fatal(ctx.Err())
		case <-time.After(time.Millisecond):
		}
	}
	if err := firstTx.Commit(ctx); err != nil {
		t.Fatal(err)
	}
	next := <-done
	if next.status != bus.ModuleStatusOK || !extract(next.out).Replayed || extract(next.out).DecisionID != extract(first).DecisionID {
		t.Fatal("concurrent review duplicated decision", next)
	}
	if err := secondTx.Commit(ctx); err != nil {
		t.Fatal(err)
	}
	// A non-owner SQL writer cannot rewrite payloads or decisions or erase drafts.
	for _, sql := range []string{
		`UPDATE user_memory_correction_proposals SET payload='{}'`,
		`UPDATE user_memory_correction_proposals SET state='pending'`,
		`DELETE FROM user_memory_correction_proposals`, `TRUNCATE user_memory_correction_proposals`,
		`UPDATE user_memories SET review_proposal_id='forged'`,
	} {
		denied := begin(conn)
		_, err := denied.Exec(ctx, sql)
		_ = denied.Rollback(ctx)
		if err == nil {
			t.Fatal("runtime bypassed private review guards", sql)
		}
	}
	// Elapsed validity is not renewed by an automatic same-key model upsert.
	expired := one(call(model, DataRequest{Operation: "store", Key: "expired-model", Kind: "fact", Tier: "L2", Content: "expired model assertion", Confidence: &certainty}))
	exec(`BEGIN; SELECT set_config('aimee.private_authority','user',true),set_config('aimee.private_principal','fixture:temporal-controller',true)`)
	exec(`UPDATE user_memories SET valid_until=now()-interval '1 second' WHERE id=$1`, expired.ID)
	exec(`COMMIT`)
	expiredGeneration := scalar(`SELECT generation FROM user_memory_collection_generation`)
	refusal := call(model, DataRequest{Operation: "store", Key: "expired-model", Kind: "fact", Tier: "L2", Content: "expired model assertion", Confidence: &certainty})
	if refusal.Code == nil || *refusal.Code != MutationReviewRequired || refusal.Proposal != nil || scalar(`SELECT generation FROM user_memory_collection_generation`) != expiredGeneration {
		t.Fatal("model revived an expired assertion", refusal)
	}
	denied := begin(conn)
	if _, err := denied.Exec(ctx, `SELECT set_config('aimee.private_authority','model',true)`); err != nil {
		t.Fatal(err)
	}
	_, err = denied.Exec(ctx, `UPDATE user_memories SET valid_until=NULL WHERE id=$1`, expired.ID)
	_ = denied.Rollback(ctx)
	if err == nil {
		t.Fatal("compatibility SQL revived expired model content")
	}
	// Revocation/erasure gates draft access and replay; physical erase removes payloads.
	call(human, DataRequest{Operation: "delete", Authority: AuthorityUser, ID: target.ID})
	if out := call(human, request); out.Code == nil || *out.Code != MutationReplayUnavailable {
		t.Fatal("retired review result replayed", out)
	}
	exec(`DELETE FROM user_memories WHERE id=$1`, target.ID)
	if scalar(`SELECT count(*) FROM user_memory_correction_proposals WHERE target_id=`+fmt.Sprint(target.ID)) != 0 {
		t.Fatal("erasure retained draft content")
	}
}
