package postgres

import (
	"bytes"
	"context"
	"encoding/binary"
	"os"
	"strings"
	"testing"
	"time"

	"github.com/JBailes/aimee/server-go/bus"
)

// The storage stage against a real PostgreSQL.
//
// Everything else in this package drives fakes, which proves the codec and the
// ownership rules and proves nothing about the driver. The cases below are the
// ones both sides of this contract agreed matter, and several of them are
// failures that only appear against a live server: a value large enough to
// fragment, bytes that are not valid UTF-8, and NULL arriving back through a
// driver that has its own opinion about empty.
//
//	AIMEE_DB2_URL=postgres://... go test ./modules/postgres/ -run Live
//
// Skipped without a database rather than failed: a unit run on a machine with
// no PostgreSQL is not evidence of anything, and a test that fails there would
// be turned off.

func liveHandler(t *testing.T) (*SQLHandler, bus.ModuleInvocation) {
	t.Helper()
	if os.Getenv("AIMEE_DB2_URL") == "" {
		t.Skip("set AIMEE_DB2_URL to run the live storage suite")
	}
	if _, err := productionProbe.getPool(); err != nil {
		t.Skipf("no database: %v", err)
	}
	// A caller the runtime could attribute. Zero would be refused, which is the
	// behaviour an unattributed call gets in production.
	return NewSQLHandler(), bus.ModuleInvocation{
		StageID: StageSQL, PrincipalRef: 30, SrcHandle: 1}
}

// call runs one request through the handler and returns the decoded reply
// header plus the payload that follows it.
func call(t *testing.T, h *SQLHandler, invocation bus.ModuleInvocation, body []byte) (
	uint32, string, string, []byte,
) {
	t.Helper()
	reply, status := h.Handle(invocation, body)
	if status != bus.ModuleStatusOK {
		t.Fatalf("module status = %v", status)
	}
	at := 0
	code := binary.LittleEndian.Uint32(reply[at:])
	at += 4
	length := binary.LittleEndian.Uint32(reply[at:])
	at += 4
	sqlstate := string(reply[at : at+int(length)])
	at += int(length)
	length = binary.LittleEndian.Uint32(reply[at:])
	at += 4
	message := string(reply[at : at+int(length)])
	at += int(length)
	return code, sqlstate, message, reply[at:]
}

func exec(t *testing.T, h *SQLHandler, invocation bus.ModuleInvocation, handle uint64,
	sql string, args ...Value) (uint32, string, string) {
	t.Helper()
	code, state, message, _ := call(t, h, invocation,
		statementRequest(OpExec, "live_test", handle, sql, args...))
	return code, state, message
}

// queryCells runs a QUERY and returns the first row's cells.
func queryCells(t *testing.T, h *SQLHandler, invocation bus.ModuleInvocation,
	sql string, args ...Value) []Value {
	t.Helper()
	code, state, message, payload := call(t, h, invocation,
		statementRequest(OpQuery, "live_test", 0, sql, args...))
	if code != StatusOK {
		t.Fatalf("query failed: status=%d sqlstate=%s %s", code, state, message)
	}
	width := binary.LittleEndian.Uint32(payload[0:])
	count := binary.LittleEndian.Uint32(payload[4:])
	if count == 0 {
		return nil
	}
	r := &reader{buf: payload[8:]}
	cells := make([]Value, 0, width)
	for index := uint32(0); index < width; index++ {
		value, err := r.value()
		if err != nil {
			t.Fatalf("cell %d: %v", index, err)
		}
		cells = append(cells, value)
	}
	return cells
}

func TestLiveTextAndBytesRoundTripAcrossAFrame(t *testing.T) {
	// db1 declares four one-megabyte replies and the bus inline budget is a few
	// kilobytes, so fragmentation is most of its traffic rather than an edge
	// case. Neither side had ever driven a value that large end to end.
	h, invocation := liveHandler(t)
	if code, state, message := exec(t, h, invocation, 0,
		`CREATE TABLE IF NOT EXISTS live_big (id BIGINT PRIMARY KEY, body TEXT, blob BYTEA)`); code != StatusOK {
		t.Fatalf("create: status=%d sqlstate=%s %s", code, state, message)
	}
	if code, _, _ := exec(t, h, invocation, 0, `DELETE FROM live_big`); code != StatusOK {
		t.Fatal("clear failed")
	}

	// Exactly MaxCellBytes. db1's largest declared values are 1 MiB, so this is
	// the boundary they actually sit on; one byte more is refused rather than
	// truncated, which is the behaviour both sides agreed to.
	body := strings.Repeat("q", MaxCellBytes)
	blob := make([]byte, 1<<20) // 1 MiB of bytes
	for index := range blob {
		// Deliberately not valid UTF-8 anywhere: db1's digests and nonce are
		// raw bytes, and anything routing them through a string corrupts them
		// silently and the column stops comparing equal to itself.
		blob[index] = byte(index % 251)
	}

	if code, state, message := exec(t, h, invocation, 0,
		`INSERT INTO live_big (id, body, blob) VALUES ($1, $2, $3)`,
		Value{Type: ValueInt, Int: 1},
		Value{Type: ValueText, Text: body},
		Value{Type: ValueBytes, Bytes: blob}); code != StatusOK {
		t.Fatalf("insert: status=%d sqlstate=%s %s", code, state, message)
	}

	cells := queryCells(t, h, invocation, `SELECT body, blob FROM live_big WHERE id = 1`)
	if len(cells) != 2 {
		t.Fatalf("cells = %d", len(cells))
	}
	if cells[0].Type != ValueText || cells[0].Text != body {
		t.Errorf("text round trip lost data: %d bytes back, %d sent",
			len(cells[0].Text), len(body))
	}
	if cells[1].Type != ValueBytes || !bytes.Equal(cells[1].Bytes, blob) {
		t.Errorf("bytea round trip corrupted: %d bytes back, %d sent",
			len(cells[1].Bytes), len(blob))
	}
}

func TestLiveNullAndEmptySurviveTheDriver(t *testing.T) {
	// The distinction the whole wire exists for, and a live driver is exactly
	// where it would quietly collapse.
	h, invocation := liveHandler(t)
	if code, _, _ := exec(t, h, invocation, 0,
		`CREATE TABLE IF NOT EXISTS live_null (id BIGINT PRIMARY KEY, body TEXT, blob BYTEA)`); code != StatusOK {
		t.Fatal("create failed")
	}
	if code, _, _ := exec(t, h, invocation, 0, `DELETE FROM live_null`); code != StatusOK {
		t.Fatal("clear failed")
	}
	for _, row := range []struct {
		id   int64
		body Value
		blob Value
	}{
		{1, Value{Type: ValueNull}, Value{Type: ValueNull}},
		{2, Value{Type: ValueText, Text: ""}, Value{Type: ValueBytes, Bytes: []byte{}}},
	} {
		if code, state, message := exec(t, h, invocation, 0,
			`INSERT INTO live_null (id, body, blob) VALUES ($1, $2, $3)`,
			Value{Type: ValueInt, Int: row.id}, row.body, row.blob); code != StatusOK {
			t.Fatalf("insert %d: status=%d sqlstate=%s %s", row.id, code, state, message)
		}
	}

	nulls := queryCells(t, h, invocation, `SELECT body, blob FROM live_null WHERE id = 1`)
	if nulls[0].Type != ValueNull || nulls[1].Type != ValueNull {
		t.Errorf("NULL came back as %d/%d, want null/null", nulls[0].Type, nulls[1].Type)
	}
	empties := queryCells(t, h, invocation, `SELECT body, blob FROM live_null WHERE id = 2`)
	if empties[0].Type != ValueText || empties[0].Text != "" {
		t.Errorf("an empty string came back as type %d", empties[0].Type)
	}
	if empties[1].Type != ValueBytes || empties[1].Bytes == nil ||
		len(empties[1].Bytes) != 0 {
		t.Errorf("an empty bytea came back as type %d len %d; a nullable binary "+
			"column becomes unwritable if this collapses",
			empties[1].Type, len(empties[1].Bytes))
	}
}

func TestLiveCommitOnAReclaimedHandleAnswers25P01(t *testing.T) {
	// The case that turns silent data loss into a visible error. A caller whose
	// transaction was reclaimed keeps writing and then commits; if the commit
	// answers OK, every write is gone and the caller was told it succeeded.
	h, invocation := liveHandler(t)

	begin := binary.LittleEndian.AppendUint32(nil, OpBegin)
	begin = binary.LittleEndian.AppendUint32(begin, uint32(len("live_test")))
	begin = append(begin, "live_test"...)
	begin = binary.LittleEndian.AppendUint64(begin, 0)
	begin = binary.LittleEndian.AppendUint32(begin, 0)
	begin = binary.LittleEndian.AppendUint32(begin, 0)

	code, state, message, payload := call(t, h, invocation, begin)
	if code != StatusOK {
		t.Fatalf("begin: status=%d sqlstate=%s %s", code, state, message)
	}
	handle := binary.LittleEndian.Uint64(payload)
	if handle == 0 {
		t.Fatal("begin returned the no-transaction handle")
	}

	// Age it past the idle timeout, then let the reaper run on the next call.
	h.transactions.mu.Lock()
	h.transactions.open[handle].lastUsed = time.Now().Add(-2 * transactionIdleTimeout)
	h.transactions.mu.Unlock()

	commit := binary.LittleEndian.AppendUint32(nil, OpCommit)
	commit = binary.LittleEndian.AppendUint32(commit, uint32(len("live_test")))
	commit = append(commit, "live_test"...)
	commit = binary.LittleEndian.AppendUint64(commit, handle)
	commit = binary.LittleEndian.AppendUint32(commit, 0)
	commit = binary.LittleEndian.AppendUint32(commit, 0)

	code, state, _, _ = call(t, h, invocation, commit)
	if code != StatusStatementFailed || state != sqlStateNoActiveTransaction {
		t.Fatalf("commit on a reclaimed handle: status=%d sqlstate=%q, want %d/%s",
			code, state, StatusStatementFailed, sqlStateNoActiveTransaction)
	}
}

func TestLiveAnotherPrincipalCannotDriveTheTransaction(t *testing.T) {
	// Now that the bus carries who is calling, ownership is the control rather
	// than the handle being hard to guess.
	h, invocation := liveHandler(t)

	begin := binary.LittleEndian.AppendUint32(nil, OpBegin)
	begin = binary.LittleEndian.AppendUint32(begin, uint32(len("live_test")))
	begin = append(begin, "live_test"...)
	begin = binary.LittleEndian.AppendUint64(begin, 0)
	begin = binary.LittleEndian.AppendUint32(begin, 0)
	begin = binary.LittleEndian.AppendUint32(begin, 0)
	code, _, _, payload := call(t, h, invocation, begin)
	if code != StatusOK {
		t.Fatal("begin failed")
	}
	handle := binary.LittleEndian.Uint64(payload)

	intruder := bus.ModuleInvocation{StageID: StageSQL, PrincipalRef: 31, SrcHandle: 1}
	status, state, _ := exec(t, h, intruder, handle, `SELECT 1`)
	if status != StatusStatementFailed || state != sqlStateNoActiveTransaction {
		t.Errorf("another principal drove the transaction: status=%d sqlstate=%q",
			status, state)
	}

	// And the owner still has it -- refusing the intruder must not have
	// disturbed the transaction it was trying to reach.
	if status, _, _ := exec(t, h, invocation, handle, `SELECT 1`); status != StatusOK {
		t.Errorf("the owner lost its transaction: status=%d", status)
	}
}

func TestLiveConstraintViolationCarriesItsSQLSTATE(t *testing.T) {
	// A unique violation is often an expected answer and an outage never is.
	// Without the SQLSTATE a caller cannot tell them apart, and db1 carries a
	// comment working around exactly that absence.
	h, invocation := liveHandler(t)
	if code, _, _ := exec(t, h, invocation, 0,
		`CREATE TABLE IF NOT EXISTS live_unique (id BIGINT PRIMARY KEY)`); code != StatusOK {
		t.Fatal("create failed")
	}
	if code, _, _ := exec(t, h, invocation, 0, `DELETE FROM live_unique`); code != StatusOK {
		t.Fatal("clear failed")
	}
	if code, _, _ := exec(t, h, invocation, 0,
		`INSERT INTO live_unique (id) VALUES (1)`); code != StatusOK {
		t.Fatal("first insert failed")
	}
	code, state, _ := exec(t, h, invocation, 0, `INSERT INTO live_unique (id) VALUES (1)`)
	if code != StatusStatementFailed || state != "23505" {
		t.Fatalf("duplicate insert: status=%d sqlstate=%q, want a unique violation",
			code, state)
	}
}

func TestLiveMigrateRefusesAnEditedMigration(t *testing.T) {
	// The failure nothing previously detected: the source and the database
	// disagree about what the database contains.
	h, invocation := liveHandler(t)
	owner := "livetest"
	if err := h.store.Exec(context.Background(),
		`DELETE FROM aimee_schema_version WHERE owner = $1`, owner); err != nil {
		// The table may not exist yet on a fresh database; the migration path
		// creates it.
		t.Logf("clear: %v", err)
	}

	migration := Migration{Owner: owner, Version: 1,
		Statements: []string{`CREATE TABLE IF NOT EXISTS live_migrated (id BIGINT)`}}
	encode := func(m Migration, checksum string) []byte {
		out := binary.LittleEndian.AppendUint32(nil, OpMigrate)
		out = binary.LittleEndian.AppendUint32(out, uint32(len(m.Owner)))
		out = append(out, m.Owner...)
		out = binary.LittleEndian.AppendUint64(out, uint64(m.Version))
		out = binary.LittleEndian.AppendUint32(out, uint32(len(checksum)))
		out = append(out, checksum...)
		out = binary.LittleEndian.AppendUint32(out, uint32(len(m.Statements)))
		for _, statement := range m.Statements {
			out = binary.LittleEndian.AppendUint32(out, uint32(len(statement)))
			out = append(out, statement...)
		}
		return out
	}

	if code, state, message, _ := call(t, h, invocation,
		encode(migration, migration.Checksum())); code != StatusOK {
		t.Fatalf("migrate: status=%d sqlstate=%s %s", code, state, message)
	}
	// Re-sending the same version with different statements is the edit case.
	edited := Migration{Owner: owner, Version: 1,
		Statements: []string{`CREATE TABLE IF NOT EXISTS live_migrated (id BIGINT, extra TEXT)`}}
	code, _, message, _ := call(t, h, invocation, encode(edited, edited.Checksum()))
	if code != StatusMigrationFailed {
		t.Fatalf("an edited migration was accepted: status=%d %s", code, message)
	}

	// A checksum that does not match the statements it was sent with is refused
	// before anything is read, so a caller cannot record a hash of statements
	// it did not send.
	code, _, _, _ = call(t, h, invocation, encode(migration, "0000"))
	if code != StatusInvalidRequest {
		t.Fatalf("a mismatched checksum was accepted: status=%d", code)
	}
}
