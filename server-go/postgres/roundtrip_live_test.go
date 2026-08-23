package postgres_test

import (
	"context"
	"os"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
	module "github.com/JBailes/aimee/server-go/modules/postgres"
	client "github.com/JBailes/aimee/server-go/postgres"
)

// Client and server, against a real database.
//
// Every other test in this package drives the client against a frame this
// package also wrote, which proves the client is self-consistent and proves
// nothing about the module. These drive the ACTUAL handler, so a disagreement
// between the two halves of the contract fails here rather than in production.
//
//	AIMEE_DB2_URL=postgres://... go test ./postgres/ -run Live
//
// Skipped without a database: a run on a machine with no PostgreSQL is not
// evidence, and a test that failed there would be turned off.

func liveClient(t *testing.T) *client.Client {
	t.Helper()
	if os.Getenv("AIMEE_DB2_URL") == "" {
		t.Skip("set AIMEE_DB2_URL to run the live round trip")
	}
	handler := module.NewSQLHandler()
	// A caller the runtime could attribute. Zero is refused, which is what an
	// unattributed call gets in production.
	invocation := bus.ModuleInvocation{
		StageID: module.StageSQL, PrincipalRef: 32, SrcHandle: 1}
	return client.New(func(ctx context.Context, body []byte) ([]byte, error) {
		reply, status := handler.Handle(invocation, body)
		if status != bus.ModuleStatusOK {
			t.Fatalf("module status = %v", status)
		}
		return reply, nil
	})
}

func TestLiveRoundTripThroughBothHalves(t *testing.T) {
	c := liveClient(t)
	ctx := context.Background()

	if _, err := c.Exec(ctx, "roundtrip_setup", 0,
		`CREATE TABLE IF NOT EXISTS rt (id BIGINT PRIMARY KEY, body TEXT, blob BYTEA, flag BOOLEAN, weight DOUBLE PRECISION)`); err != nil {
		t.Fatalf("create: %v", err)
	}
	if _, err := c.Exec(ctx, "roundtrip_clear", 0, `DELETE FROM rt`); err != nil {
		t.Fatalf("clear: %v", err)
	}

	// Bytes with nothing valid as UTF-8: a digest routed through a string is
	// corrupted silently and stops comparing equal to itself.
	blob := []byte{0x00, 0xff, 0xfe, 0x80, 0x7f}
	if _, err := c.Exec(ctx, "roundtrip_insert", 0,
		`INSERT INTO rt (id, body, blob, flag, weight) VALUES ($1, $2, $3, $4, $5)`,
		client.Int(1), client.Text("hello"), client.Bytes(blob),
		client.Bool(true), client.Float(1.5)); err != nil {
		t.Fatalf("insert: %v", err)
	}

	row, err := c.QueryRow(ctx, "roundtrip_read", 0,
		`SELECT body, blob, flag, weight FROM rt WHERE id = $1`, client.Int(1))
	if err != nil {
		t.Fatalf("read: %v", err)
	}
	if row[0].Text != "hello" {
		t.Errorf("text = %q", row[0].Text)
	}
	if string(row[1].Bytes) != string(blob) {
		t.Errorf("bytes = %#v, want %#v", row[1].Bytes, blob)
	}
	if !row[2].Bool {
		t.Error("a BOOLEAN did not come back as a bool")
	}
	if row[3].Float != 1.5 {
		t.Errorf("float = %v", row[3].Float)
	}
}

func TestLiveNullSurvivesBothHalves(t *testing.T) {
	c := liveClient(t)
	ctx := context.Background()
	if _, err := c.Exec(ctx, "null_setup", 0,
		`CREATE TABLE IF NOT EXISTS rt_null (id BIGINT PRIMARY KEY, body TEXT, blob BYTEA)`); err != nil {
		t.Fatalf("create: %v", err)
	}
	if _, err := c.Exec(ctx, "null_clear", 0, `DELETE FROM rt_null`); err != nil {
		t.Fatalf("clear: %v", err)
	}
	if _, err := c.Exec(ctx, "null_insert", 0,
		`INSERT INTO rt_null (id, body, blob) VALUES ($1, $2, $3), ($4, $5, $6)`,
		client.Int(1), client.Null(), client.Bytes(nil),
		client.Int(2), client.Text(""), client.Bytes([]byte{})); err != nil {
		t.Fatalf("insert: %v", err)
	}

	nulls, err := c.QueryRow(ctx, "null_read", 0,
		`SELECT body, blob FROM rt_null WHERE id = 1`)
	if err != nil {
		t.Fatalf("read null: %v", err)
	}
	if nulls[0].Type != 0 || nulls[1].Type != 0 {
		t.Errorf("NULL came back as types %d/%d", nulls[0].Type, nulls[1].Type)
	}
	empties, err := c.QueryRow(ctx, "empty_read", 0,
		`SELECT body, blob FROM rt_null WHERE id = 2`)
	if err != nil {
		t.Fatalf("read empty: %v", err)
	}
	if empties[0].Type == 0 || empties[1].Type == 0 {
		t.Error("an empty value came back as NULL; the distinction the wire " +
			"exists for did not survive a real driver")
	}
}

func TestLiveTransactionCommitsAndRollsBack(t *testing.T) {
	c := liveClient(t)
	ctx := context.Background()
	if _, err := c.Exec(ctx, "tx_setup", 0,
		`CREATE TABLE IF NOT EXISTS rt_tx (id BIGINT PRIMARY KEY)`); err != nil {
		t.Fatalf("create: %v", err)
	}
	if _, err := c.Exec(ctx, "tx_clear", 0, `DELETE FROM rt_tx`); err != nil {
		t.Fatalf("clear: %v", err)
	}

	if err := c.InTx(ctx, "tx_commit", func(handle uint64) error {
		_, err := c.Exec(ctx, "tx_commit", handle,
			`INSERT INTO rt_tx (id) VALUES (1)`)
		return err
	}); err != nil {
		t.Fatalf("commit: %v", err)
	}

	rolled := errorString("deliberate")
	if err := c.InTx(ctx, "tx_rollback", func(handle uint64) error {
		if _, err := c.Exec(ctx, "tx_rollback", handle,
			`INSERT INTO rt_tx (id) VALUES (2)`); err != nil {
			return err
		}
		return rolled
	}); err != rolled {
		t.Fatalf("rollback returned %v, want the body's own error", err)
	}

	rows, err := c.Query(ctx, "tx_read", 0, `SELECT id FROM rt_tx ORDER BY id`)
	if err != nil {
		t.Fatalf("read: %v", err)
	}
	if len(rows) != 1 || rows[0][0].Int != 1 {
		t.Fatalf("rows = %+v; the rolled-back insert survived or the commit did not",
			rows)
	}
}

func TestLiveUniqueViolationIsClassifiable(t *testing.T) {
	c := liveClient(t)
	ctx := context.Background()
	if _, err := c.Exec(ctx, "dup_setup", 0,
		`CREATE TABLE IF NOT EXISTS rt_dup (id BIGINT PRIMARY KEY)`); err != nil {
		t.Fatalf("create: %v", err)
	}
	if _, err := c.Exec(ctx, "dup_clear", 0, `DELETE FROM rt_dup`); err != nil {
		t.Fatalf("clear: %v", err)
	}
	if _, err := c.Exec(ctx, "dup_first", 0, `INSERT INTO rt_dup (id) VALUES (1)`); err != nil {
		t.Fatalf("first insert: %v", err)
	}
	_, err := c.Exec(ctx, "dup_second", 0, `INSERT INTO rt_dup (id) VALUES (1)`)
	if err == nil {
		t.Fatal("a duplicate insert succeeded")
	}
	if !client.IsUniqueViolation(err) {
		t.Errorf("a duplicate was not classified as a unique violation: %v", err)
	}
	if client.IsTransactionClosed(err) {
		t.Error("a unique violation read as a lost transaction; a caller would " +
			"abandon work that is still live")
	}
}

func TestLiveMigrationIsVersionedAndChecksummed(t *testing.T) {
	c := liveClient(t)
	ctx := context.Background()
	statements := []string{`CREATE TABLE IF NOT EXISTS rt_migrated (id BIGINT)`}

	// The checksum the module recomputes over what it receives; a caller that
	// sent a different one would be refused, which is the point.
	sum := checksumOf(statements)
	if err := c.Migrate(ctx, "roundtrip", 1, sum, statements); err != nil {
		t.Fatalf("migrate: %v", err)
	}
	version, recorded, err := c.CurrentVersion(ctx, "roundtrip")
	if err != nil {
		t.Fatalf("current version: %v", err)
	}
	if version != 1 || recorded != sum {
		t.Fatalf("head = %d/%s, want 1/%s", version, recorded, sum)
	}
	// Re-sending the same version with different statements is the edit case,
	// and it is the failure nothing else detects.
	edited := []string{`CREATE TABLE IF NOT EXISTS rt_migrated (id BIGINT, extra TEXT)`}
	if err := c.Migrate(ctx, "roundtrip", 1, checksumOf(edited), edited); err == nil {
		t.Fatal("a migration edited after it ran was accepted")
	}
}

type errorString string

func (e errorString) Error() string { return string(e) }

// checksumOf uses the module's own hash rather than reimplementing it. A second
// implementation of a checksum is a second thing that can disagree, and the
// whole point of the checksum is that two sides agree about what ran.
func checksumOf(statements []string) string {
	return module.Migration{Statements: statements}.Checksum()
}
