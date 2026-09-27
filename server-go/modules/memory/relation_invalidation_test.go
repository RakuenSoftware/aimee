package memory

import (
	"context"
	"errors"
	"fmt"
	"os"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/jackc/pgx/v5"
	"github.com/jackc/pgx/v5/pgconn"
)

// Use real committed connections: checkpoint survival, disconnection and competing
// consumers cannot be proved by nested savepoints on one database connection.
func TestRelationInvalidationDurableConsumer(t *testing.T) {
	dsn := os.Getenv("AIMEE_DB2_REPLAY_URL")
	if dsn == "" {
		t.Skip("set AIMEE_DB2_REPLAY_URL for durable relation consumer replay")
	}
	ctx, cancel := context.WithTimeout(context.Background(), 90*time.Second)
	defer cancel()
	owner, err := pgx.Connect(ctx, dsn)
	if err != nil {
		t.Fatal(err)
	}
	defer owner.Close(context.Background())
	exec := func(q string, args ...any) {
		t.Helper()
		if _, err := owner.Exec(ctx, q, args...); err != nil {
			t.Fatal(err)
		}
	}
	var existing int
	if err := owner.QueryRow(ctx, `SELECT count(*) FROM memories`).Scan(&existing); err != nil {
		t.Fatal(err)
	}
	if existing != 0 {
		t.Skip("durable consumer fixture requires an empty replay store")
	}
	key := fmt.Sprintf("relation-consumer-%d", time.Now().UnixNano())
	role := pgx.Identifier{strings.ReplaceAll(key, "-", "_")}.Sanitize()
	exec(`CREATE ROLE ` + role + ` NOINHERIT NOBYPASSRLS; GRANT USAGE ON SCHEMA public TO ` + role + `;
 GRANT EXECUTE ON FUNCTION memory_apply_relation_invalidations(integer) TO ` + role)
	ids := []int64{}
	relationIDs := []int64{}
	defer func() {
		cleanup := context.Background()
		for _, id := range relationIDs {
			_, _ = owner.Exec(cleanup, `DELETE FROM memory_lineage WHERE object_type='relation' AND object_id=$1`, id)
		}
		for _, id := range ids {
			_, _ = owner.Exec(cleanup, `DELETE FROM memories WHERE id=$1`, id)
			_, _ = owner.Exec(cleanup, `DELETE FROM kb_async_jobs WHERE kind='memory_index' AND document_id=$1`, id)
		}
		_, _ = owner.Exec(cleanup, `DROP OWNED BY `+role+`; DROP ROLE `+role)
	}()
	connect := func() *pgx.Conn {
		t.Helper()
		c, err := pgx.Connect(ctx, dsn)
		if err != nil {
			t.Fatal(err)
		}
		if _, err = c.Exec(ctx, `SET ROLE `+role); err != nil {
			t.Fatal(err)
		}
		return c
	}
	a := connect()
	defer a.Close(context.Background())
	apply := func(c *pgx.Conn, n int) {
		t.Helper()
		if _, err := c.Exec(ctx, `SELECT memory_apply_relation_invalidations($1)`, n); err != nil {
			t.Fatal(err)
		}
	}
	// The public helper cannot reveal or rewrite private progress, even though its
	// fixed storage transaction can invalidate a dependent in another scope.
	for _, q := range []string{`SELECT * FROM memory_relation_consumer_positions`,
		`UPDATE memory_relation_consumer_state SET snapshot_after_id=999`,
		`SELECT memory_reset_relation_consumer()`} {
		_, err := a.Exec(ctx, q)
		var pgerr *pgconn.PgError
		if !errors.As(err, &pgerr) || pgerr.Code != "42501" {
			t.Fatalf("progress authority widened: %v", err)
		}
	}

	// Reapplying the migration repairs stale/default grants on private progress.
	schema, err := os.ReadFile("../../../src/modules/db2/c/schema.sql")
	if err != nil {
		t.Fatal(err)
	}
	begin, end := strings.Index(string(schema), "DO $relation_consumer_acl$"), strings.Index(string(schema), "END $relation_consumer_acl$;")
	if begin < 0 || end < begin {
		t.Fatal("consumer ACL migration missing")
	}
	exec(`GRANT SELECT,UPDATE ON memory_relation_consumer_state,memory_relation_consumer_positions TO ` + role + `; GRANT EXECUTE ON FUNCTION memory_reset_relation_consumer() TO ` + role)
	exec(string(schema[begin : end+len("END $relation_consumer_acl$;")]))
	exec(`GRANT EXECUTE ON FUNCTION memory_apply_relation_invalidations(integer) TO ` + role)
	if _, err := a.Exec(ctx, `SELECT * FROM memory_relation_consumer_positions`); err == nil {
		t.Fatal("reapply retained private progress grant")
	}
	for _, n := range []int{0, -1, 257} {
		_, err := a.Exec(ctx, `SELECT memory_apply_relation_invalidations($1)`, n)
		var pgerr *pgconn.PgError
		if !errors.As(err, &pgerr) || pgerr.Code != "22023" {
			t.Fatalf("unbounded batch accepted: %v", err)
		}
	}
	for i, scope := range []string{"global", "project", "project"} {
		value := key + fmt.Sprint(i)
		if i == 0 {
			value = "_global"
		}
		var id int64
		if err := owner.QueryRow(ctx, `INSERT INTO memories(tier,kind,key,content,scope_type,scope_value)
 VALUES('L2','fact',$1,'consumer fixture',$2,$3) RETURNING id`, key+fmt.Sprint(i), scope, value).Scan(&id); err != nil {
			t.Fatal(err)
		}
		ids = append(ids, id)
	}
	for _, parent := range ids[1:] {
		var link, relation int64
		if err := owner.QueryRow(ctx, `INSERT INTO memory_links(source_id,target_id,relation) VALUES($1,$2,'related_to') RETURNING id`, parent, ids[0]).Scan(&link); err != nil {
			t.Fatal(err)
		}
		if err := owner.QueryRow(ctx, `INSERT INTO memory_relations(memory_id,src_entity,relation,dst_entity,fact_text)
 VALUES($1,$2,'related_to','target','copied source') RETURNING id`, parent, key).Scan(&relation); err != nil {
			t.Fatal(err)
		}
		relationIDs = append(relationIDs, relation)
		exec(`INSERT INTO memory_lineage(object_type,object_id,source_kind,source_ref)
 VALUES('relation',$1,'memory-relation-input-v2',jsonb_build_object('record_id',$2::bigint::text,'record_revision','1','link_id',$3::bigint::text)::text)`, relation, ids[0], link)
	}
	exec(`SELECT memory_reset_relation_consumer(); UPDATE kb_async_jobs SET status='done' WHERE kind='memory_index'`)
	apply(a, 2)
	var phase string
	var after, maxID int64
	readState := func() {
		t.Helper()
		if err := owner.QueryRow(ctx, `SELECT phase,snapshot_after_id,snapshot_max_id FROM memory_relation_consumer_state WHERE id=1`).Scan(&phase, &after, &maxID); err != nil {
			t.Fatal(err)
		}
	}
	readState()
	if phase != "snapshot" || after != ids[1] || maxID != ids[2] {
		t.Fatal("snapshot exceeded root budget", phase, after, maxID)
	}
	apply(a, 2)
	readState()
	if phase != "replay" {
		t.Fatal("snapshot failed to finish", phase)
	}
	cursor := func() (int64, int64) {
		t.Helper()
		var generation, target int64
		if err := owner.QueryRow(ctx, `SELECT generation,target_after_id FROM memory_relation_consumer_positions WHERE scope_type='global' AND scope_value='_global'`).Scan(&generation, &target); err != nil {
			t.Fatal(err)
		}
		return generation, target
	}
	jobGeneration := func(id int64) int64 {
		t.Helper()
		var generation int64
		if err := owner.QueryRow(ctx, `SELECT generation FROM kb_async_jobs WHERE kind='memory_index' AND document_id=$1`, id).Scan(&generation); err != nil {
			t.Fatal(err)
		}
		return generation
	}
	jobsDone := func() { exec(`UPDATE kb_async_jobs SET status='done' WHERE kind='memory_index'`) }
	jobsDone()
	before, _ := cursor()
	exec(`UPDATE memories SET content='changed source' WHERE id=$1`, ids[0])
	apply(a, 1)
	g, target := cursor()
	if g != before || target != ids[0] {
		t.Fatal("acknowledged partial fan-out", g, target)
	}
	// A committed partial fan-out resumes from its durable target cursor after a
	// connection disappears. It does not charge/queue the first target twice.
	firstGeneration := jobGeneration(ids[0])
	a.Close(ctx)
	a = connect()
	defer a.Close(context.Background())
	b := connect()
	defer b.Close(context.Background())
	var wg sync.WaitGroup
	errs := make(chan error, 2)
	for _, c := range []*pgx.Conn{a, b} {
		wg.Add(1)
		go func(c *pgx.Conn) {
			defer wg.Done()
			_, err := c.Exec(ctx, `SELECT memory_apply_relation_invalidations(1)`)
			errs <- err
		}(c)
	}
	wg.Wait()
	close(errs)
	for err := range errs {
		if err != nil {
			t.Fatal(err)
		}
	}
	g, target = cursor()
	if g != before+1 || target != 0 || jobGeneration(ids[0]) != firstGeneration {
		t.Fatal("concurrent replay lost progress", g, target)
	}
	var pending int
	if err := owner.QueryRow(ctx, `SELECT count(*) FROM kb_async_jobs WHERE kind='memory_index' AND document_id=ANY($1) AND status='pending'`, ids[1:]).Scan(&pending); err != nil || pending != 2 {
		t.Fatal("hidden dependent not queued", pending, err)
	}
	parentGeneration := jobGeneration(ids[1])
	apply(a, 64)
	if jobGeneration(ids[1]) != parentGeneration {
		t.Fatal("duplicate replay repeated work")
	}
	// Disconnect before commit: both queue changes and progress must roll back.
	jobsDone()
	exec(`UPDATE memories SET content='uncommitted application' WHERE id=$1`, ids[0])
	before, _ = cursor()
	c := connect()
	if _, err := c.Exec(ctx, `BEGIN; SELECT memory_apply_relation_invalidations(64)`); err != nil {
		t.Fatal(err)
	}
	c.Close(ctx)
	g, _ = cursor()
	if g != before || jobGeneration(ids[1]) != parentGeneration {
		t.Fatal("uncommitted consumer progress survived", g)
	}
	// A late queue failure cannot advance the corresponding journal position.
	trigger := pgx.Identifier{strings.ReplaceAll(key, "-", "_") + "_fail"}.Sanitize()
	exec(`CREATE FUNCTION ` + trigger + `() RETURNS trigger LANGUAGE plpgsql AS $$ BEGIN
 IF NEW.kind='memory_index' AND NEW.document_id=` + fmt.Sprint(ids[1]) + ` THEN RAISE EXCEPTION 'fixture queue failure'; END IF; RETURN NEW; END $$;
 CREATE TRIGGER ` + trigger + ` BEFORE INSERT OR UPDATE ON kb_async_jobs FOR EACH ROW EXECUTE FUNCTION ` + trigger + `() `)
	defer func() {
		_, _ = owner.Exec(context.Background(), `DROP TRIGGER IF EXISTS `+trigger+` ON kb_async_jobs; DROP FUNCTION IF EXISTS `+trigger+`() `)
	}()
	if _, err := a.Exec(ctx, `SELECT memory_apply_relation_invalidations(64)`); err == nil {
		t.Fatal("queue failure ignored")
	}
	g, _ = cursor()
	if g != before || jobGeneration(ids[1]) != parentGeneration {
		t.Fatal("failed application advanced", g)
	}
	exec(`DROP TRIGGER ` + trigger + ` ON kb_async_jobs; DROP FUNCTION ` + trigger + `() `)
	apply(a, 64)
	g, _ = cursor()
	if g != before+1 {
		t.Fatal("retry did not apply", g)
	}
	// Retention loss restarts canonical rebuilding. A snapshot frontier is never
	// mislabelled as a replay acknowledgement while rebuilding remains incomplete.
	jobsDone()
	exec(`UPDATE memories SET content='retention gap' WHERE id=$1`, ids[0])
	exec(`DELETE FROM memory_invalidation_outbox WHERE scope_type='global' AND scope_value='_global' AND generation=(SELECT generation FROM memory_collection_generations WHERE scope_type='global' AND scope_value='_global')`)
	apply(a, 1)
	readState()
	if phase != "snapshot" || after != 0 {
		t.Fatal("gap falsely acknowledged", phase, after)
	}
	apply(a, 1)
	readState()
	if phase != "snapshot" || after != ids[0] {
		t.Fatal("resnapshot ignored bound", phase, after)
	}
	apply(a, 64)
	readState()
	if phase != "replay" {
		t.Fatal("resnapshot failed", phase)
	}
	// Deleted link rows cannot hide an erased input's prior dependants: retained
	// input observations supply the reverse mapping until rebuilding removes them.
	jobsDone()
	before, _ = cursor()
	exec(`DELETE FROM memories WHERE id=$1`, ids[0])
	apply(a, 64)
	g, _ = cursor()
	if g != before+1 {
		t.Fatal("erasure event not applied", g)
	}
	if err := owner.QueryRow(ctx, `SELECT count(*) FROM kb_async_jobs WHERE kind='memory_index' AND document_id=ANY($1) AND status='pending'`, ids[1:]).Scan(&pending); err != nil || pending != 2 {
		t.Fatal("erased source lost reverse dependants", pending, err)
	}
	// Owner replacement cannot resume the previous owner's progress. Exercise the
	// actual Go scheduler adapter to finish this bounded canonical resynchronization.
	var oldOwner, newOwner string
	if err := owner.QueryRow(ctx, `SELECT owner_id::text FROM memory_collection_owner WHERE id=1`).Scan(&oldOwner); err != nil {
		t.Fatal(err)
	}
	defer func() {
		_, _ = owner.Exec(context.Background(), `UPDATE memory_collection_owner SET owner_id=$1::uuid WHERE id=1`, oldOwner)
	}()
	if err := owner.QueryRow(ctx, `UPDATE memory_collection_owner SET owner_id=gen_random_uuid() WHERE id=1 RETURNING owner_id::text`).Scan(&newOwner); err != nil {
		t.Fatal(err)
	}
	apply(a, 1)
	readState()
	var progressOwner string
	if err := owner.QueryRow(ctx, `SELECT owner_id::text FROM memory_relation_consumer_state WHERE id=1`).Scan(&progressOwner); err != nil || progressOwner != newOwner || phase != "snapshot" {
		t.Fatal("owner mismatch resumed old cursor", progressOwner, phase, err)
	}
	tx, err := a.Begin(ctx)
	if err != nil {
		t.Fatal(err)
	}
	backend := &postgresDataStore{placement: PlacementKB, db: runtimeRoleDB{evalQueryer{tx}, t}}
	if err = backend.reconcileRelationInputs(ctx); err != nil {
		_ = tx.Rollback(ctx)
		t.Fatal(err)
	}
	if err = tx.Commit(ctx); err != nil {
		t.Fatal(err)
	}
	readState()
	if phase != "replay" {
		t.Fatal("Go consumer failed to resume", phase)
	}

}
