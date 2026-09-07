package families

import (
	"context"
	"errors"
	"strings"
	"testing"

	wire "github.com/JBailes/aimee/server-go/aimee"
	"github.com/JBailes/aimee/server-go/bus"
	store "github.com/JBailes/aimee/server-go/modules/aimee"
)

// --- the wire layout ----------------------------------------------------------

// Every offset here was read out of the GENERATED C stage
// (the C store's guardrail_state_stage.c, now only in git history), not the same
// arithmetic this file uses. A layout computed correctly from wrong premises
// would agree with itself and disagree with the caller, and this is the only
// thing that would catch that.
func TestWireLayoutMatchesTheGeneratedStage(t *testing.T) {
	for _, test := range []struct {
		name string
		got  int
		want int
	}{
		{"save field count", saveFields, 387},
		{"load reply width", loadFields, 386},
		{"sid", offSid, 0},
		{"first seen path", offSeenPaths, 1},
		{"seen_count", offSeenCount, 65},
		{"session_mode", offSessionMode, 66},
		{"guardrail_mode", offGuardrailMode, 67},
		{"active_task_id", offActiveTaskID, 68},
		{"hook_call_count", offHookCalls, 69},
		{"dirty", offDirty, 70},
		{"first worktree", offWorktrees, 71},
		{"worktree_count", offWorktreeCnt, 103},
		{"is_delegate", offIsDelegate, 104},
		{"orch_direct_edits", offOrchEdits, 105},
		{"orch_nudge_sent", offOrchNudge, 106},
		{"skill_find_symbols", offSkillSymbols, 107},
		{"skill_condition_waiting", offSkillWaiting, 108},
		{"skill_tdd", offSkillTDD, 109},
		{"tdd_mode", offTDDMode, 110},
		{"first tdd write", offTDDWrites, 111},
		{"tdd_write_count", offTDDWriteCnt, 127},
		{"first read path", offReadPaths, 128},
		{"read_path_count", offReadPathCnt, 192},
		{"first file hash", offFileHashes, 193},
		{"file_hash_count", offFileHashCnt, 321},
		{"first ap hit", offAPHits, 322},
		{"ap_hit_count", offAPHitCnt, 386},
	} {
		if test.got != test.want {
			t.Errorf("%s offset = %d, want %d", test.name, test.got, test.want)
		}
	}
}

// --- a session-state-shaped fake ---------------------------------------------

type guardScalars struct {
	sessionMode, guardrailMode, tddMode string
	activeTaskID, hookCalls             int64
}

type guardRow struct {
	db  *guardDB
	sql string
}

func (r guardRow) Scan(dest ...any) error {
	if r.db.scanErr != nil {
		return r.db.scanErr
	}
	if r.db.scalars == nil {
		return store.ErrNoRows
	}
	s := r.db.scalars
	switch {
	case strings.Contains(r.sql, "SELECT 1 FROM session_state"):
		*(dest[0].(*int)) = 1
	case strings.Contains(r.sql, "SELECT session_id,"):
		*(dest[0].(*string)) = "sess"
		*(dest[1].(*string)) = "2026-08-22 09:00:00"
		*(dest[2].(*int64)) = s.hookCalls
	default:
		*(dest[0].(*string)) = s.sessionMode
		*(dest[1].(*string)) = s.guardrailMode
		*(dest[2].(*string)) = s.tddMode
		*(dest[3].(*int64)) = s.activeTaskID
		*(dest[4].(*int64)) = s.hookCalls
		for i := 5; i < len(dest); i++ {
			*(dest[i].(*int64)) = 0
		}
	}
	return nil
}

// guardRows replays a fixed set of child rows for whichever collection is asked.
type guardRows struct {
	values [][]any
	at     int
}

func (r *guardRows) Close()     {}
func (r *guardRows) Err() error { return nil }
func (r *guardRows) Next() bool {
	if r.at >= len(r.values) {
		return false
	}
	r.at++
	return true
}
func (r *guardRows) Scan(dest ...any) error {
	row := r.values[r.at-1]
	for i := range dest {
		switch p := dest[i].(type) {
		case *string:
			*p = row[i].(string)
		case *int64:
			*p = row[i].(int64)
		case *bool:
			*p = row[i].(bool)
		}
	}
	return nil
}

type guardDB struct {
	scalars  *guardScalars
	children map[string][][]any
	scanErr  error
	execErr  error

	executed []string
	args     [][]any
}

func newGuardDB() *guardDB {
	return &guardDB{children: map[string][][]any{}}
}

func (d *guardDB) Exec(_ context.Context, sql string, args ...any) (store.Tag, error) {
	d.executed = append(d.executed, sql)
	d.args = append(d.args, args)
	if d.execErr != nil {
		return store.RowsAffected(0), d.execErr
	}
	return store.RowsAffected(1), nil
}
func (d *guardDB) Query(_ context.Context, sql string, args ...any) (store.Rows, error) {
	d.executed = append(d.executed, sql)
	d.args = append(d.args, args)
	for key, rows := range d.children {
		if strings.Contains(sql, key) {
			return &guardRows{values: rows}, nil
		}
	}
	return &guardRows{}, nil
}
func (d *guardDB) QueryRow(_ context.Context, sql string, args ...any) store.Row {
	d.executed = append(d.executed, sql)
	d.args = append(d.args, args)
	return guardRow{db: d, sql: sql}
}
func (d *guardDB) Begin(context.Context) (store.Tx, error) { return guardTx{d}, nil }

type guardTx struct{ *guardDB }

func (t guardTx) Commit(context.Context) error   { return nil }
func (t guardTx) Rollback(context.Context) error { return nil }

func guardCall(t *testing.T, db store.DB, op uint32, fields []string) (uint32, []string) {
	t.Helper()
	frame, err := wire.EncodeFields(op, fields)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	response, status := GuardrailState.Handler(db)(
		bus.ModuleInvocation{StageID: StageGuardrailState}, frame)
	if status != bus.ModuleStatusOK {
		t.Fatalf("wire status = %v, want OK", status)
	}
	inBand, cells, err := wire.DecodeFields(response)
	if err != nil {
		t.Fatalf("decode: %v", err)
	}
	return inBand, cells
}

// saveFrame builds a full 387-field save request with sid set and everything
// else blank, so a test can fill exactly the slots it cares about.
func saveFrame(sid string) []string {
	f := make([]string, saveFields)
	f[offSid] = sid
	for _, at := range []int{offSeenCount, offWorktreeCnt, offTDDWriteCnt,
		offReadPathCnt, offFileHashCnt, offAPHitCnt, offDirty, offIsDelegate,
		offActiveTaskID, offHookCalls, offOrchEdits, offOrchNudge,
		offSkillSymbols, offSkillWaiting, offSkillTDD} {
		f[at] = "0"
	}
	return f
}

func ranStatement(db *guardDB, fragment string) bool {
	for _, sql := range db.executed {
		if strings.Contains(sql, fragment) {
			return true
		}
	}
	return false
}

// --- save ---------------------------------------------------------------------

// The child collections are a snapshot, not a log, so every save clears each
// table before refilling it. A save that only inserted would accumulate paths
// from every previous turn of the session.
func TestSaveClearsEveryChildTableBeforeRefilling(t *testing.T) {
	db := newGuardDB()
	f := saveFrame("sess")
	f[offSeenCount] = "2"
	f[offSeenPaths] = "/a"
	f[offSeenPaths+1] = "/b"

	status, _ := guardCall(t, db, opSessionStateSave, f)
	if status != store.StatusOK {
		t.Fatalf("status = %d, want OK", status)
	}
	for _, table := range []string{
		"session_state_seen_paths", "session_state_read_paths",
		"session_state_worktrees", "session_state_tdd_writes",
		"session_state_ap_hits", "session_state_file_hashes",
	} {
		if !ranStatement(db, "DELETE FROM "+table) {
			t.Fatalf("%s was not cleared", table)
		}
	}

	// and the clear must come BEFORE the insert for that table
	var clearedAt, insertedAt = -1, -1
	for i, sql := range db.executed {
		if strings.Contains(sql, "DELETE FROM session_state_seen_paths") {
			clearedAt = i
		}
		if strings.Contains(sql, "INSERT INTO session_state_seen_paths") && insertedAt < 0 {
			insertedAt = i
		}
	}
	if clearedAt < 0 || insertedAt < 0 || clearedAt > insertedAt {
		t.Fatalf("clear at %d, first insert at %d", clearedAt, insertedAt)
	}
}

// A count larger than the slots that exist must be clamped, not trusted: the
// frame is fixed-width, and reading past it would panic.
func TestSaveClampsAnOverstatedCount(t *testing.T) {
	db := newGuardDB()
	f := saveFrame("sess")
	f[offSeenCount] = "10000"
	for i := 0; i < maxSeenPaths; i++ {
		f[offSeenPaths+i] = "/p"
	}
	status, _ := guardCall(t, db, opSessionStateSave, f)
	if status != store.StatusOK {
		t.Fatalf("status = %d, want OK", status)
	}
	inserts := 0
	for _, sql := range db.executed {
		if strings.Contains(sql, "INSERT INTO session_state_seen_paths") {
			inserts++
		}
	}
	if inserts != maxSeenPaths {
		t.Fatalf("wrote %d seen paths, want the capacity %d", inserts, maxSeenPaths)
	}
}

func TestCountInClampsAndRejects(t *testing.T) {
	fields := []string{"5", "-1", "not-a-number", "", "999"}
	for _, test := range []struct {
		at, capacity, want int
	}{
		{0, 10, 5},
		{0, 3, 3},
		{1, 10, 0},
		{2, 10, 0},
		{3, 10, 0},
		{4, 64, 64},
	} {
		if got := countIn(fields, test.at, test.capacity); got != test.want {
			t.Fatalf("countIn(%q, cap %d) = %d, want %d",
				fields[test.at], test.capacity, got, test.want)
		}
	}
}

// The C substituted these when a field arrived empty. They are applied here
// because the module writes every column on every save, so a column default
// would never fire.
func TestSaveAppliesTheModeDefaults(t *testing.T) {
	db := newGuardDB()
	status, _ := guardCall(t, db, opSessionStateSave, saveFrame("sess"))
	if status != store.StatusOK {
		t.Fatalf("status = %d", status)
	}
	var upsert []any
	for i, sql := range db.executed {
		if strings.Contains(sql, "INSERT INTO session_state (") {
			upsert = db.args[i]
		}
	}
	if upsert == nil {
		t.Fatalf("no upsert ran")
	}
	if upsert[1] != defaultSessionMode || upsert[2] != defaultGuardrailMode ||
		upsert[3] != defaultTDDMode {
		t.Fatalf("defaults = %v %v %v", upsert[1], upsert[2], upsert[3])
	}
}

func TestSaveRefusesABlankSessionOrBadScalar(t *testing.T) {
	db := newGuardDB()
	if status, _ := guardCall(t, db, opSessionStateSave, saveFrame("")); status != store.StatusInvalid {
		t.Fatalf("blank sid: status = %d, want invalid", status)
	}
	if len(db.executed) != 0 {
		t.Fatalf("a blank sid ran %d statements", len(db.executed))
	}

	db = newGuardDB()
	f := saveFrame("sess")
	f[offHookCalls] = "many"
	if status, _ := guardCall(t, db, opSessionStateSave, f); status != store.StatusInvalid {
		t.Fatalf("bad scalar: status = %d, want invalid", status)
	}
}

// --- the unsigned hash --------------------------------------------------------

// content_hash crosses the wire unsigned and is stored in a signed BIGINT. Both
// directions must be exact, including above 2^63-1 where the stored value is
// negative.
func TestContentHashRoundTripsThroughSignedStorage(t *testing.T) {
	for _, v := range []uint64{0, 1, 1 << 62, 1<<63 - 1, 1 << 63, 1<<64 - 1, 18446744073709551615} {
		if got := hashFromStored(hashToStored(v)); got != v {
			t.Fatalf("hash %d round-tripped to %d", v, got)
		}
	}
	// The wire spelling must be unsigned too: I64toa would render these
	// negative and the caller's strtoull would not recover them.
	if got := store.U64toa(1 << 63); got != "9223372036854775808" {
		t.Fatalf("U64toa(2^63) = %s", got)
	}
	if got, ok := store.Atou64("18446744073709551615"); !ok || got != 1<<64-1 {
		t.Fatalf("Atou64 = %d, %v", got, ok)
	}
}

func TestSaveStoresTheHashAsItsBits(t *testing.T) {
	db := newGuardDB()
	f := saveFrame("sess")
	f[offFileHashCnt] = "1"
	f[offFileHashes] = "/file"
	f[offFileHashes+1] = "18446744073709551615" // 2^64-1
	status, _ := guardCall(t, db, opSessionStateSave, f)
	if status != store.StatusOK {
		t.Fatalf("status = %d", status)
	}
	for i, sql := range db.executed {
		if strings.Contains(sql, "INSERT INTO session_state_file_hashes") {
			if db.args[i][2].(int64) != -1 {
				t.Fatalf("stored %v, want -1 (the bits of 2^64-1)", db.args[i][2])
			}
			return
		}
	}
	t.Fatalf("no file-hash insert ran")
}

// --- load ---------------------------------------------------------------------

func TestLoadReturnsMissingForAnUnknownSession(t *testing.T) {
	status, cells := guardCall(t, newGuardDB(), opSessionStateLoad, []string{"nope"})
	if status != store.StatusMissing {
		t.Fatalf("status = %d, want %d (missing)", status, store.StatusMissing)
	}
	if len(cells) != 0 {
		t.Fatalf("a miss carried %d cells", len(cells))
	}
}

// The reply is fixed-width whatever the session holds: an unfilled slot is
// blank, not absent, or every offset after it would shift.
func TestLoadReplyIsAlwaysFullWidth(t *testing.T) {
	db := newGuardDB()
	db.scalars = &guardScalars{sessionMode: "review", guardrailMode: "auto",
		tddMode: "on", activeTaskID: 42, hookCalls: 7}
	status, cells := guardCall(t, db, opSessionStateLoad, []string{"sess"})
	if status != store.StatusOK {
		t.Fatalf("status = %d, want OK", status)
	}
	if len(cells) != loadFields {
		t.Fatalf("reply carried %d cells, want %d", len(cells), loadFields)
	}
	if cells[offSessionMode-1] != "review" || cells[offGuardrailMode-1] != "auto" ||
		cells[offTDDMode-1] != "on" {
		t.Fatalf("modes = %q %q %q", cells[offSessionMode-1],
			cells[offGuardrailMode-1], cells[offTDDMode-1])
	}
	if cells[offActiveTaskID-1] != "42" || cells[offHookCalls-1] != "7" {
		t.Fatalf("scalars = %q %q", cells[offActiveTaskID-1], cells[offHookCalls-1])
	}
	// Every collection is empty, so every count is zero rather than blank.
	for _, at := range []int{offSeenCount, offWorktreeCnt, offTDDWriteCnt,
		offReadPathCnt, offFileHashCnt, offAPHitCnt} {
		if cells[at-1] != "0" {
			t.Fatalf("count at %d = %q, want \"0\"", at, cells[at-1])
		}
	}
}

func TestLoadFillsCollectionsAndTheirCounts(t *testing.T) {
	db := newGuardDB()
	db.scalars = &guardScalars{sessionMode: "implement", guardrailMode: "approve", tddMode: "off"}
	db.children["session_state_seen_paths"] = [][]any{{"/a"}, {"/b"}, {"/c"}}
	db.children["session_state_file_hashes"] = [][]any{{"/f", int64(-1)}}
	db.children["session_state_tdd_writes"] = [][]any{{"stem", true}}

	_, cells := guardCall(t, db, opSessionStateLoad, []string{"sess"})
	if cells[offSeenCount-1] != "3" {
		t.Fatalf("seen count = %q", cells[offSeenCount-1])
	}
	if cells[offSeenPaths-1] != "/a" || cells[offSeenPaths] != "/b" || cells[offSeenPaths+1] != "/c" {
		t.Fatalf("seen paths = %v", cells[offSeenPaths-1:offSeenPaths+2])
	}
	if cells[offFileHashCnt-1] != "1" {
		t.Fatalf("file hash count = %q", cells[offFileHashCnt-1])
	}
	// -1 stored is 2^64-1 on the wire.
	if cells[offFileHashes-1] != "/f" || cells[offFileHashes] != "18446744073709551615" {
		t.Fatalf("file hash = %q %q", cells[offFileHashes-1], cells[offFileHashes])
	}
	if cells[offTDDWriteCnt-1] != "1" || cells[offTDDWrites-1] != "stem" ||
		cells[offTDDWrites] != "1" {
		t.Fatalf("tdd write = %q %q %q", cells[offTDDWriteCnt-1],
			cells[offTDDWrites-1], cells[offTDDWrites])
	}
}

// A store holding more rows than the frame has slots is truncated, not written
// past the end of.
func TestLoadTruncatesACollectionThatOverflowsTheFrame(t *testing.T) {
	db := newGuardDB()
	db.scalars = &guardScalars{}
	over := make([][]any, maxSeenPaths+10)
	for i := range over {
		over[i] = []any{"/p"}
	}
	db.children["session_state_seen_paths"] = over

	_, cells := guardCall(t, db, opSessionStateLoad, []string{"sess"})
	if cells[offSeenCount-1] != store.Itoa(maxSeenPaths) {
		t.Fatalf("seen count = %q, want the capacity", cells[offSeenCount-1])
	}
	if len(cells) != loadFields {
		t.Fatalf("reply width = %d, want %d", len(cells), loadFields)
	}
}

func TestLoadReportsAStoreFailure(t *testing.T) {
	db := newGuardDB()
	db.scalars = &guardScalars{}
	db.scanErr = errors.New("store broke")
	if status, _ := guardCall(t, db, opSessionStateLoad, []string{"sess"}); status != store.StatusFailed {
		t.Fatalf("status = %d, want %d (failed)", status, store.StatusFailed)
	}
}

// --- the small operations -----------------------------------------------------

// "Is there one" was answered, and the answer was no -- that is OK with a 0,
// not MISSING.
func TestExistsAnswersZeroRatherThanMissing(t *testing.T) {
	status, cells := guardCall(t, newGuardDB(), opSessionStateExists, []string{"nope"})
	if status != store.StatusOK || len(cells) != 1 || cells[0] != "0" {
		t.Fatalf("status = %d, cells = %v", status, cells)
	}

	db := newGuardDB()
	db.scalars = &guardScalars{}
	status, cells = guardCall(t, db, opSessionStateExists, []string{"sess"})
	if status != store.StatusOK || cells[0] != "1" {
		t.Fatalf("status = %d, cells = %v", status, cells)
	}
}

func TestDeleteRunsOneStatementAndCascades(t *testing.T) {
	db := newGuardDB()
	status, _ := guardCall(t, db, opSessionStateDelete, []string{"sess"})
	if status != store.StatusOK {
		t.Fatalf("status = %d", status)
	}
	if len(db.executed) != 1 {
		t.Fatalf("delete ran %d statements, want 1 -- the children cascade: %v",
			len(db.executed), db.executed)
	}
}

func TestListsValidateTheirBounds(t *testing.T) {
	for _, test := range []struct {
		name   string
		op     uint32
		fields []string
	}{
		{"list max zero", opSessionStateList, []string{"0"}},
		{"list max too large", opSessionStateList, []string{"1000"}},
		{"list max not a number", opSessionStateList, []string{"all"}},
		{"expired negative threshold", opSessionStateListExpired, []string{"-1", "10"}},
		{"expired max zero", opSessionStateListExpired, []string{"60", "0"}},
	} {
		t.Run(test.name, func(t *testing.T) {
			db := newGuardDB()
			status, _ := guardCall(t, db, test.op, test.fields)
			if status != store.StatusInvalid {
				t.Fatalf("status = %d, want %d (invalid)", status, store.StatusInvalid)
			}
			if len(db.executed) != 0 {
				t.Fatalf("an out-of-bounds list queried anyway")
			}
		})
	}
}

// Expiry is measured against the DATABASE's clock: the caller supplies an age,
// so a caller whose clock has drifted cannot expire sessions early.
func TestExpiryUsesTheDatabaseClock(t *testing.T) {
	db := newGuardDB()
	guardCall(t, db, opSessionStateListExpired, []string{"3600", "10"})
	if len(db.executed) != 1 {
		t.Fatalf("statements = %v", db.executed)
	}
	if !strings.Contains(db.executed[0], "now() - make_interval") {
		t.Fatalf("expiry does not use the database clock: %s", db.executed[0])
	}
	if db.args[0][0].(int) != 3600 {
		t.Fatalf("threshold bound as %v", db.args[0][0])
	}
}
