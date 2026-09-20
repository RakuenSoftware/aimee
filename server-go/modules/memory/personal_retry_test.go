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

func TestPersonalMutationRetry(t *testing.T) {
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
	name := fmt.Sprintf("private_retries_%d", time.Now().UnixNano())
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
	for _, name := range []string{"changes", "versions", "acl", "authority", "proposals", "retries"} {
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
	target := create("retry-target")
	original := one(call(nil, DataRequest{Operation: "get", ID: target.ID, IncludeVersion: true}))
	request := DataRequest{Operation: "supersede", Authority: AuthorityUser, ID: target.ID, Content: "durable private correction", Confidence: &certainty, ExpectedVersion: original.Version, IdempotencyKey: "private-retry-key-01"}
	receiptOf := func(out DataResponse) *MemoryMutationReceipt {
		t.Helper()
		if out.MutationReceipt == nil || out.MutationReceipt.CommitID == "" {
			t.Fatal(out)
		}
		return out.MutationReceipt
	}
	refusal := func(out DataResponse, code int) {
		t.Helper()
		if out.Code == nil || *out.Code != code {
			t.Fatal(code, out)
		}
	}
	// A failure at the last write must undo content, authorship, history and journal.
	before := scalar(`SELECT generation FROM user_memory_collection_generation`)
	exec(`ALTER TABLE user_memory_mutation_receipts ADD CONSTRAINT fixture_failure CHECK(false) NOT VALID`)
	tx := begin(conn)
	_, status := invoke(tx, human, request)
	_ = tx.Rollback(ctx)
	if status != bus.ModuleStatusInternal {
		t.Fatal("receipt failure admitted write", status)
	}
	if scalar(`SELECT generation FROM user_memory_collection_generation`) != before || scalar(`SELECT count(*) FROM user_memory_versions`) != 0 {
		t.Fatal("receipt failure leaked canonical effects")
	}
	exec(`ALTER TABLE user_memory_mutation_receipts DROP CONSTRAINT fixture_failure`)
	first := call(human, request)
	receipt := *receiptOf(first)
	if receipt.Replayed || one(first).Content != request.Content || receipt.Version != *one(first).Version || receipt.Version.RecordRevision == original.Version.RecordRevision {
		t.Fatal(first)
	}
	before = scalar(`SELECT generation FROM user_memory_collection_generation`)
	// Ignore the committed response and retry from a new connection, as after loss.
	second, err := pgx.Connect(ctx, dsn)
	if err != nil {
		t.Fatal(err)
	}
	defer second.Close(context.Background())
	nextTx := begin(second)
	replay, status := invoke(nextTx, human, request)
	if status != bus.ModuleStatusOK || !receiptOf(replay).Replayed || receiptOf(replay).CommitID != receipt.CommitID {
		t.Fatal(replay, status)
	}
	if err := nextTx.Commit(ctx); err != nil {
		t.Fatal(err)
	}
	if scalar(`SELECT generation FROM user_memory_collection_generation`) != before || scalar(`SELECT count(*) FROM user_memory_mutation_receipts`) != 1 {
		t.Fatal("replay repeated mutation")
	}
	changed := request
	changed.Content = "different admitted payload"
	refusal(call(human, changed), MutationIdempotencyConflict)
	changed = request
	changed.Authority = AuthorityModel
	refusal(call(human, changed), MutationIdempotencyConflict)
	other := *human
	other.Principal = "fixture:other-user"
	refusal(call(&other, request), MutationVersionConflict)
	// The same key is independent for another verified actor and a current target.
	changed = request
	changed.ExpectedVersion = &receipt.Version
	changed.Content = "other actor's correction"
	otherResult := call(&other, changed)
	if receiptOf(otherResult).Replayed || receiptOf(otherResult).CommitID == receipt.CommitID {
		t.Fatal("actor namespace collapsed")
	}
	refusal(call(human, request), MutationReplayUnavailable)
	// Retirement and erasure do not release old keys or return cached payloads.
	call(human, DataRequest{Operation: "delete", Authority: AuthorityUser, ID: target.ID})
	refusal(call(&other, changed), MutationReplayUnavailable)
	exec(`DELETE FROM user_memories WHERE id=$1`, target.ID)
	refusal(call(human, request), MutationReplayUnavailable)
	if scalar(`SELECT count(*) FROM user_memory_mutation_receipts`) != 2 {
		t.Fatal("erasure freed committed key")
	}
	// Model review-required is a durable outcome too, including rejection/erasure.
	protected := create("retry-proposal")
	version := one(call(nil, DataRequest{Operation: "get", ID: protected.ID, IncludeVersion: true})).Version
	proposed := DataRequest{Operation: "supersede", ID: protected.ID, Content: "model draft", Confidence: &certainty, ExpectedVersion: version, IdempotencyKey: "private-proposal-key"}
	draft := call(model, proposed)
	refusal(draft, MutationReviewRequired)
	if draft.Proposal == nil || draft.Proposal.Draft != nil {
		t.Fatal(draft)
	}
	repeated := call(model, proposed)
	if repeated.Proposal == nil || !repeated.Proposal.Replayed || repeated.Proposal.ID != draft.Proposal.ID {
		t.Fatal(repeated)
	}
	review := correctionReviewRequest{ProposalID: draft.Proposal.ID, Digest: draft.Proposal.Digest, Expected: draft.Proposal.Target, Action: "reject"}
	call(human, DataRequest{Operation: "correction-review", CorrectionReview: &review})
	repeated = call(model, proposed)
	if repeated.Proposal == nil || repeated.Proposal.State != "rejected" || !repeated.Proposal.Replayed {
		t.Fatal("rejected draft reopened", repeated)
	}
	changed = proposed
	changed.Content = "different model draft"
	refusal(call(model, changed), MutationIdempotencyConflict)
	exec(`DELETE FROM user_memories WHERE id=$1`, protected.ID)
	refusal(call(model, proposed), MutationReplayUnavailable)
	// Approval changes the proposal outcome, but never repeats the keyed draft.
	approvedParent := create("approved-retry-proposal")
	approvedRequest := proposed
	approvedRequest.ID = approvedParent.ID
	approvedRequest.ExpectedVersion = one(call(nil, DataRequest{Operation: "get", ID: approvedParent.ID, IncludeVersion: true})).Version
	approvedRequest.IdempotencyKey = "private-approved-proposal"
	approvedDraft := call(model, approvedRequest).Proposal
	if approvedDraft == nil {
		t.Fatal("missing approved retry draft")
	}
	approval := correctionReviewRequest{ProposalID: approvedDraft.ID, Digest: approvedDraft.Digest, Expected: approvedDraft.Target, Action: "approve"}
	call(human, DataRequest{Operation: "correction-review", CorrectionReview: &approval})
	approvedGeneration := scalar(`SELECT generation FROM user_memory_collection_generation`)
	approvedReplay := call(model, approvedRequest)
	if approvedReplay.Proposal == nil || !approvedReplay.Proposal.Replayed || approvedReplay.Proposal.State != "approved" || approvedReplay.Proposal.Result == nil || approvedReplay.Proposal.Draft != nil || scalar(`SELECT generation FROM user_memory_collection_generation`) != approvedGeneration {
		t.Fatal("approved draft retry lost decision or repeated effect", approvedReplay)
	}
	call(human, DataRequest{Operation: "delete", Authority: AuthorityUser, ID: approvedParent.ID})
	refusal(call(model, approvedRequest), MutationReplayUnavailable)
	exec(`DELETE FROM user_memories WHERE id=$1`, approvedParent.ID)
	// A failed receipt insert cannot leave even a proposal committed on its own.
	protected = create("retry-proposal-rollback")
	proposed.ID = protected.ID
	proposed.ExpectedVersion = one(call(nil, DataRequest{Operation: "get", ID: protected.ID, IncludeVersion: true})).Version
	proposed.IdempotencyKey = "private-proposal-rollback"
	exec(`ALTER TABLE user_memory_mutation_receipts ADD CONSTRAINT fixture_failure CHECK(false) NOT VALID`)
	tx = begin(conn)
	_, status = invoke(tx, model, proposed)
	_ = tx.Rollback(ctx)
	if status != bus.ModuleStatusInternal || scalar(`SELECT count(*) FROM user_memory_correction_proposals`) != 0 {
		t.Fatal("receipt failure orphaned proposal", status)
	}
	exec(`ALTER TABLE user_memory_mutation_receipts DROP CONSTRAINT fixture_failure`)
	// Actual concurrent identical callers serialize; the waiter receives one receipt.
	concurrent := create("concurrent-retry")
	race := request
	race.ID = concurrent.ID
	race.ExpectedVersion = one(call(nil, DataRequest{Operation: "get", ID: concurrent.ID, IncludeVersion: true})).Version
	race.IdempotencyKey = "concurrent-private-key"
	observer, err := pgx.Connect(ctx, dsn)
	if err != nil {
		t.Fatal(err)
	}
	defer observer.Close(context.Background())
	firstTx, secondTx := begin(conn), begin(second)
	defer firstTx.Rollback(context.Background())
	defer secondTx.Rollback(context.Background())
	first, status = invoke(firstTx, human, race)
	if status != bus.ModuleStatusOK {
		t.Fatal(status)
	}
	type result struct {
		out    DataResponse
		status bus.ModuleStatus
	}
	done := make(chan result, 1)
	go func() { out, status := invoke(secondTx, human, race); done <- result{out, status} }()
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
			t.Fatal("retry did not serialize", r)
		case <-ctx.Done():
			t.Fatal(ctx.Err())
		case <-time.After(time.Millisecond):
		}
	}
	if err := firstTx.Commit(ctx); err != nil {
		t.Fatal(err)
	}
	next := <-done
	if next.status != bus.ModuleStatusOK || !receiptOf(next.out).Replayed || receiptOf(next.out).CommitID != receiptOf(first).CommitID {
		t.Fatal("concurrent duplicate", next)
	}
	if err := secondTx.Commit(ctx); err != nil {
		t.Fatal(err)
	}
	// Disconnect before commit: both mutation and reserved key disappear.
	lost := create("disconnected-retry")
	race.ID = lost.ID
	race.ExpectedVersion = one(call(nil, DataRequest{Operation: "get", ID: lost.ID, IncludeVersion: true})).Version
	race.IdempotencyKey = "disconnected-private-key"
	disconnected, err := pgx.Connect(ctx, dsn)
	if err != nil {
		t.Fatal(err)
	}
	lostTx := begin(disconnected)
	lostOut, status := invoke(lostTx, human, race)
	if status != bus.ModuleStatusOK {
		t.Fatal(status)
	}
	if err := disconnected.Close(ctx); err != nil {
		t.Fatal(err)
	}
	recovered := call(human, race)
	if receiptOf(recovered).Replayed || receiptOf(recovered).CommitID == receiptOf(lostOut).CommitID {
		t.Fatal("uncommitted receipt survived disconnect")
	}
	// Host runtime forwarding preserves receipts and refuses forged peer context.
	args, _ := json.Marshal(map[string]any{"operation": "user-mcp-supersede", "id": lost.ID, "content": race.Content, "expected_version": race.ExpectedVersion, "idempotency_key": race.IdempotencyKey})
	tx = begin(conn)
	backend, _ := NewPostgresDataStore(runtimeRoleDB{evalQueryer{tx}, t}, PlacementServer)
	handler := NewHandler(nil, WithDataStore(PlacementServer, backend))
	if _, status := invokeContextCommand(t, handler, 41, *human, "runtime", string(args)); status != bus.ModuleStatusInvalidRequest {
		t.Fatal("peer forged caller", status)
	}
	// MCP model context cannot impersonate the user even with an identical key.
	envelope, status := invokeContextCommand(t, handler, 0, *model, "runtime", string(args))
	if status != bus.ModuleStatusOK || !strings.Contains(envelope["json"].(string), "expected_version_conflict") {
		t.Fatal(envelope, status)
	}
	envelope, status = invokeContextCommand(t, handler, 0, *human, "runtime", string(args))
	if status != bus.ModuleStatusOK || !strings.Contains(envelope["json"].(string), `"replayed":true`) || !strings.Contains(envelope["json"].(string), `"mutation_receipt"`) {
		t.Fatal(envelope, status)
	}
	if err := tx.Commit(ctx); err != nil {
		t.Fatal(err)
	}

	// Expiry after commit cannot be renewed by replay.
	exec(`BEGIN; SELECT set_config('aimee.private_authority','user',true),set_config('aimee.private_principal','fixture:clock',true)`)
	exec(`UPDATE user_memories SET valid_until=now()-interval '1 second' WHERE id=$1`, lost.ID)
	exec(`COMMIT`)
	refusal(call(human, race), MutationReplayUnavailable)
	for _, sql := range []string{`UPDATE user_memory_mutation_receipts SET request_hash=repeat('0',64)`, `DELETE FROM user_memory_mutation_receipts`, `TRUNCATE user_memory_mutation_receipts`, `INSERT INTO user_memory_mutation_receipts(owner_id,actor_principal,key_hash,request_hash,target_id,target_revision,result_revision) SELECT owner_id,'forged',repeat('0',64),repeat('0',64),1,1,1 FROM user_memory_collection_generation`} {
		tx = begin(conn)
		_, err := tx.Exec(ctx, sql)
		_ = tx.Rollback(ctx)
		if err == nil {
			t.Fatal("runtime forged or erased receipt", sql)
		}
	}
}
