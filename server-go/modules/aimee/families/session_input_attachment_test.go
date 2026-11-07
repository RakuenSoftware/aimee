package families

import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	wire "github.com/JBailes/aimee/server-go/aimee"
	"github.com/JBailes/aimee/server-go/bus"
	canonical "github.com/JBailes/aimee/server-go/ir"
	store "github.com/JBailes/aimee/server-go/modules/aimee"
	"strconv"
	"strings"
	"testing"
)

func inputAttachmentEvent(t *testing.T) string {
	t.Helper()
	text := "safe original"
	h := sha256.Sum256([]byte(text))
	m := canonical.Message{Role: "user", Blocks: []canonical.Block{{Type: "text", Text: &text, InputAttachment: &canonical.InputAttachment{SchemaVersion: 1, Kind: "host.original-input.v1", OperationUUID: "12345678-1234-4234-8234-123456789abc", TextSHA256: hex.EncodeToString(h[:]), Opaque: "  {\"wide\":9007199254740993}\n"}}}}
	v, _ := json.Marshal(map[string]any{"kind": "message", "message_id": "user", "payload": m})
	if !canonical.ValidSessionEvent(v) {
		t.Fatal("fixture")
	}
	return string(v)
}
func TestInputAttachmentHostOnlyAndOrdinaryDeny(t *testing.T) {
	body := inputAttachmentEvent(t)
	fields := []string{"session-1", "12345678-1234-4234-8234-123456789abc", "turn-1", "actor", body}
	for _, id := range []uint32{56, 57} {
		if !Sessions.Ops[id].HostOnly {
			t.Fatal("not HostOnly")
		}
		args := fields
		if id == 57 {
			args = append(args, "[]")
		}
		frame, _ := wire.EncodeFields(id, args)
		db := &sessDB{}
		_, st := Sessions.Handler(db)(bus.ModuleInvocation{StageID: StageSessions, PrincipalRef: 7}, frame)
		if st != bus.ModuleStatusInvalidRequest || len(db.executed) != 0 {
			t.Fatal("forged module reached host op")
		}
	}
	q := &irAppendFixture{sessDB: &sessDB{}}
	status, _, err := sessionIRAppend(context.Background(), q, fields)
	if err != nil || status != store.StatusInvalid || len(q.reads) != 0 {
		t.Fatal("ordinary append admitted")
	}
	var e struct {
		Payload json.RawMessage `json:"payload"`
	}
	json.Unmarshal([]byte(body), &e)
	history, _ := json.Marshal([]json.RawMessage{e.Payload})
	status, _, err = sessionIRHistorySave(context.Background(), q, []string{"s", "checkpoint", "primary", "actor", string(history)})
	if err != nil || status != store.StatusInvalid || len(q.reads) != 0 {
		t.Fatal("ordinary checkpoint admitted")
	}
	report := canonical.HistoryImportReport{Version: 1, SourceFormat: "canonical", Outcome: "exact", SourceSHA256: strings.Repeat("a", 64), SourceSession: "source", SourceAgent: "agent", SourceMessages: 1, ImportedMessages: 1, ToolStructure: "preserved"}
	if !canonical.ValidHistoryImportReport(&report) {
		t.Fatal("report fixture")
	}
	encoded, _ := json.Marshal(report)
	status, _, err = sessionIRHistoryImport(context.Background(), q, []string{"dest", "primary", "actor", "source", "agent", strings.Repeat("a", 64), string(history), string(encoded), "model"})
	if err != nil || status != store.StatusInvalid || len(q.reads) != 0 || len(q.executed) != 0 {
		t.Fatal("ordinary import acquired input custody")
	}
}

type inputBoolRow bool

func (r inputBoolRow) Scan(dest ...any) error {
	if len(dest) != 1 {
		panic("boolean row arity")
	}
	p, ok := dest[0].(*bool)
	if !ok {
		panic("boolean scan type")
	}
	*p = bool(r)
	return nil
}

type inputBatchFixture struct {
	*irAppendFixture
	input                string
	partial, interleaved bool
}

func (d *inputBatchFixture) QueryRow(ctx context.Context, sql string, args ...any) store.Row {
	if sql == sessionIRExistingSQL {
		d.reads = append(d.reads, sql)
		if strings.HasSuffix(args[1].(string), ":begin") {
			if d.partial {
				return sessRow{db: &sessDB{scanErr: store.ErrNoRows}}
			}
			return sessRow{db: &sessDB{}, values: []any{int64(41), "original-turn", "actor", `{"kind":"execution","payload":{"status":"running"}}`}}
		}
		return sessRow{db: &sessDB{}, values: []any{int64(42), "original-turn", "actor", d.input}}
	}
	if sql == originalInputInterleavedSQL {
		d.reads = append(d.reads, sql)
		return inputBoolRow(d.interleaved)
	}
	return d.irAppendFixture.QueryRow(ctx, sql, args...)
}
func batchInput(body string) string {
	return `{"schema_version":1,"begin":{"kind":"execution","payload":{"status":"running"}},"input":` + body + `}`
}
func TestInputAttachmentStableOperationReconcileNoInsert(t *testing.T) {
	body := inputAttachmentEvent(t)
	fields := []string{"session-1", "12345678-1234-4234-8234-123456789abc", "new-turn-after-restart", "actor", batchInput(body)}
	makeq := func() *inputBatchFixture {
		return &inputBatchFixture{irAppendFixture: &irAppendFixture{sessDB: &sessDB{}}, input: body}
	}
	q := makeq()
	status, cells, err := sessionInputAttachmentAppend(context.Background(), q, fields)
	if err != nil || status != store.StatusOK || len(cells) != 5 || cells[0] != "41" || cells[1] != "42" || cells[2] != "original-turn" || cells[3] != "1" || cells[4] != "replay" || len(q.reads) != 4 {
		t.Fatal("did not reconcile exact immutable original", status, cells, err, q.reads)
	}
	changed := append([]string{}, fields...)
	changed[4] = batchInput(strings.Replace(body, `"message_id":"user"`, `"message_id":"changed-safe-intent"`, 1))
	q = makeq()
	status, _, err = sessionInputAttachmentAppend(context.Background(), q, changed)
	if err != nil || status != store.StatusInvalid || len(q.reads) != 3 {
		t.Fatal("same UUID changed intent did not conflict")
	}
	for _, partial := range []bool{true, false} {
		q = makeq()
		q.partial = partial
		q.interleaved = !partial
		status, _, err = sessionInputAttachmentAppend(context.Background(), q, fields)
		if err != nil || status != store.StatusInvalid {
			t.Fatal("partial/interleaved pair accepted")
		}
	}
}

// Actual candidate Handler + canonical reducer over an explicit transactional
// memory fixture. This is not a PostgreSQL/broker qualification.
type originalHistoryFixture struct {
	*sessDB
	events   [][]any
	snapshot int
}

func (d *originalHistoryFixture) Begin(context.Context) (store.Tx, error) {
	d.snapshot = len(d.events)
	return d, nil
}
func (d *originalHistoryFixture) Commit(context.Context) error { return nil }
func (d *originalHistoryFixture) Rollback(context.Context) error {
	d.events = d.events[:d.snapshot]
	return nil
}
func (d *originalHistoryFixture) QueryRow(ctx context.Context, sql string, args ...any) store.Row {
	switch sql {
	case sessionIRLockSQL:
		return sessRow{db: &sessDB{}, values: []any{"actor"}}
	case sessionIRExistingSQL:
		for _, r := range d.events {
			if r[3] == args[1] {
				n, _ := strconv.ParseInt(r[0].(string), 10, 64)
				return sessRow{db: &sessDB{}, values: []any{n, r[1], r[2], r[4]}}
			}
		}
		return sessRow{db: &sessDB{scanErr: store.ErrNoRows}}
	case nativeRecoveryFenceSQL:
		return sessRow{db: &sessDB{scanErr: store.ErrNoRows}}
	case sessionIRInsertSQL:
		n := int64(len(d.events) + 1)
		d.events = append(d.events, []any{strconv.FormatInt(n, 10), args[2], args[3], args[1], args[4]})
		return sessRow{db: &sessDB{}, values: []any{n}}
	}
	if sql == originalInputInterleavedSQL {
		interleaved := false
		for _, e := range d.events {
			seq, _ := strconv.ParseInt(e[0].(string), 10, 64)
			interleaved = interleaved || (seq > args[1].(int64) && seq < args[2].(int64))
		}
		return inputBoolRow(interleaved)
	}
	if sql == originalInputTurnOccupiedSQL {
		occupied := false
		for _, e := range d.events {
			occupied = occupied || e[1] == args[1]
		}
		return inputBoolRow(occupied)
	}
	if strings.Contains(sql, "original-input-message-v1") {
		found := false
		for _, e := range d.events {
			var event struct {
				MessageID string `json:"message_id"`
			}
			json.Unmarshal([]byte(e[4].(string)), &event)
			found = found || (e[1] == args[1] && event.MessageID == args[2] && strings.Contains(e[4].(string), `"input_attachment"`))
		}
		return inputBoolRow(found)
	}
	if strings.Contains(sql, "original-input-presence-v1") {
		found := false
		for _, e := range d.events {
			found = found || strings.Contains(e[4].(string), `"input_attachment"`)
		}
		return inputBoolRow(found)
	}
	return d.sessDB.QueryRow(ctx, sql, args...)
}
func (d *originalHistoryFixture) Query(ctx context.Context, sql string, args ...any) (store.Rows, error) {
	if sql == sessionIRHistoryExistingBatchSQL || sql == sessionIRHistoryInsertBatchSQL {
		var entries []historyAppendEntry
		if err := json.Unmarshal([]byte(args[1].(string)), &entries); err != nil {
			return nil, err
		}
		rows := [][]any{}
		for _, entry := range entries {
			if sql == sessionIRHistoryExistingBatchSQL {
				for _, event := range d.events {
					if event[3] == entry.ID {
						rows = append(rows, []any{entry.ID, event[0], event[1], event[2], event[4]})
					}
				}
			} else {
				sequence := strconv.Itoa(len(d.events) + 1)
				d.events = append(d.events, []any{sequence, args[2], args[3], entry.ID, entry.Payload})
				rows = append(rows, []any{entry.ID, sequence})
			}
		}
		return &sessRows{rows: rows}, nil
	}
	if strings.Contains(sql, "SELECT id,principal FROM server_sessions") {
		return &sessRows{rows: [][]any{{"session-1", "actor"}, {"source", "actor"}}}, nil
	}
	if sql == sessionIRHistorySQL || sql == sessionIRRunningHistorySQL {
		after, _ := strconv.ParseUint(args[2].(string), 10, 64)
		rows := historyCompletionFixturePage(d.events, after)
		return &sessRows{rows: rows}, nil
	}
	return d.sessDB.Query(ctx, sql, args...)
}
func originalHistorySeed(t *testing.T) *originalHistoryFixture {
	t.Helper()
	d := &originalHistoryFixture{sessDB: &sessDB{execRows: 1}}
	status, _ := sessCall(t, d, 56, []string{"session-1", "12345678-1234-4234-8234-123456789abc", "turn-1", "actor", batchInput(inputAttachmentEvent(t))})
	if status != store.StatusOK || len(d.events) != 2 {
		t.Fatal("actual batch failed", status, len(d.events))
	}
	return d
}
func TestInputAttachmentOrdinaryCheckpointCannotDrop(t *testing.T) {
	for _, body := range []string{"[]", `[{"role":"user","blocks":[{"type":"text","text":"safe original"}]}]`} {
		d := originalHistorySeed(t)
		status, _ := sessCall(t, d, 28, []string{"session-1", "terminal", "turn-1", "actor", `{"kind":"execution","payload":{"status":"completed"}}`})
		if status != store.StatusOK {
			t.Fatal("terminal")
		}
		status, cells := sessCall(t, d, 33, []string{"session-1", "primary", "actor", ""})
		if status != store.StatusOK || len(cells) != 1 || !strings.Contains(cells[0], "input_attachment") {
			t.Fatal("seed reducer", status, cells)
		}
		before := len(d.events)
		status, _ = sessCall(t, d, 32, []string{"session-1", "stripped-checkpoint", "primary", "actor", body})
		if status == store.StatusOK {
			status, loaded := sessCall(t, d, 33, []string{"session-1", "primary", "actor", ""})
			t.Fatalf("ordinary32 hid admitted attachment: status=%d history=%v", status, loaded)
		}
		if len(d.events) != before {
			t.Fatal("refusal wrote events")
		}
		status, cells = sessCall(t, d, 33, []string{"session-1", "primary", "actor", ""})
		if status != store.StatusOK || !strings.Contains(cells[0], "input_attachment") {
			t.Fatal("original attachment lost")
		}
	}
}

func TestInputAttachmentOtherWritersAndReducerCannotDrop(t *testing.T) {
	d := originalHistorySeed(t)
	before := len(d.events)
	for _, phase := range []string{`{"scope":"primary","phase":"commit","count":0}`, `{"scope":"primary","phase":"message","index":0,"message":{"role":"user","blocks":[]}}`} {
		status, _ := sessCall(t, d, 28, []string{"session-1", "unsafe-history", "cp", "actor", `{"kind":"history","payload":` + phase + `}`})
		if status == store.StatusOK || len(d.events) != before {
			t.Fatal("ordinary28 bypass", status, len(d.events))
		}
	}
	report := canonical.HistoryImportReport{Version: 1, SourceFormat: "canonical", Outcome: "exact", SourceSHA256: strings.Repeat("a", 64), SourceSession: "source", SourceAgent: "agent", SourceMessages: 0, ImportedMessages: 0, ToolStructure: "preserved"}
	encoded, _ := json.Marshal(report)
	status, _, err := sessionIRHistoryImport(context.Background(), d, []string{"session-1", "primary", "actor", "source", "agent", strings.Repeat("a", 64), "[]", string(encoded), "model"})
	if err != nil || status != store.StatusInvalid || len(d.events) != before {
		t.Fatal("import bypass", status, err)
	}
	// Actual authorized57 is still usable and binds the exact existing text+opaque.
	var e struct {
		Payload canonical.Message `json:"payload"`
	}
	json.Unmarshal([]byte(inputAttachmentEvent(t)), &e)
	body, _ := json.Marshal([]canonical.Message{e.Payload})
	status, _ = sessCall(t, d, 57, []string{"session-1", "safe-cp", "primary", "actor", "turn-1", string(body)})
	if status != store.StatusOK {
		t.Fatal("exact57 refused", status)
	}
	// Ordinary message/terminal and another projection scope remain compatible.
	status, _ = sessCall(t, d, 28, []string{"session-1", "safe-result", "turn-1", "actor", `{"kind":"message","message_id":"reply","payload":{"role":"assistant","blocks":[{"type":"text","text":"safe reply"}]}}`})
	if status != store.StatusOK {
		t.Fatal("ordinary observation refused")
	}
	status, _ = sessCall(t, d, 32, []string{"session-1", "notes-cp", "notes", "actor", "[]"})
	if status != store.StatusOK {
		t.Fatal("unrelated scope refused", status)
	}
	status, notes := sessCall(t, d, 33, []string{"session-1", "notes", "actor", ""})
	if status != store.StatusOK || len(notes) != 1 || notes[0] != "[]" {
		t.Fatal("unrelated projection scope changed", status, notes)
	}
	// Storage/reducer defense: even a manually corrupted checkpoint cannot hide it.
	d.events = append(d.events, []any{strconv.Itoa(len(d.events) + 1), "corrupt-cp", "actor", "corrupt-cp:0", `{"kind":"history","payload":{"scope":"primary","phase":"commit","count":0}}`})
	status, _ = sessCall(t, d, 33, []string{"session-1", "primary", "actor", ""})
	if status == store.StatusOK {
		t.Fatal("reducer accepted stripping checkpoint")
	}
}
func TestInputAttachmentLegacySourceFreeCheckpointStillWorks(t *testing.T) {
	d := &originalHistoryFixture{sessDB: &sessDB{execRows: 1}}
	status, _ := sessCall(t, d, 32, []string{"session-1", "legacy-cp", "primary", "actor", `[{"role":"user","blocks":[{"type":"text","text":"legacy safe"}]}]`})
	if status != store.StatusOK {
		t.Fatal("legacy refused", status)
	}
	status, cells := sessCall(t, d, 33, []string{"session-1", "primary", "actor", ""})
	if status != store.StatusOK || !strings.Contains(cells[0], "legacy safe") {
		t.Fatal("legacy load", status, cells)
	}
}
func TestInputAttachmentOrdinaryMessageCannotReplaceOriginal(t *testing.T) {
	d := originalHistorySeed(t)
	before := len(d.events)
	status, _ := sessCall(t, d, 28, []string{"session-1", "overwrite-user", "turn-1", "actor", `{"kind":"message","message_id":"user","payload":{"role":"user","blocks":[{"type":"text","text":"safe original"}]}}`})
	if status == store.StatusOK {
		sessCall(t, d, 28, []string{"session-1", "done", "turn-1", "actor", `{"kind":"execution","payload":{"status":"completed"}}`})
		st, cells := sessCall(t, d, 33, []string{"session-1", "primary", "actor", ""})
		t.Fatalf("ordinary message replaced attached input: status=%d history=%v", st, cells)
	}
	if len(d.events) != before {
		t.Fatal("rejected overwrite appended")
	}
}

func TestInputAttachmentNewUUIDCannotReopenExistingTurn(t *testing.T) {
	d := originalHistorySeed(t)
	before := len(d.events)
	uuid := "22345678-1234-4234-8234-123456789abc"
	body := strings.ReplaceAll(inputAttachmentEvent(t), "12345678-1234-4234-8234-123456789abc", uuid)
	status, _ := sessCall(t, d, 56, []string{"session-1", uuid, "turn-1", "actor", batchInput(body)})
	if status != store.StatusInvalid || len(d.events) != before {
		t.Fatal("new UUID reused existing input turn", status, len(d.events))
	}
	// Existing ordinary turn remains writable through its ordinary protocol.
	legacy := &originalHistoryFixture{sessDB: &sessDB{execRows: 1}}
	status, _ = sessCall(t, legacy, 28, []string{"session-1", "ordinary-running", "legacy-turn", "actor", `{"kind":"execution","payload":{"status":"running"}}`})
	if status != store.StatusOK {
		t.Fatal("legacy begin")
	}
	status, _ = sessCall(t, legacy, 56, []string{"session-1", uuid, "legacy-turn", "actor", batchInput(body)})
	if status != store.StatusInvalid || len(legacy.events) != 1 {
		t.Fatal("new UUID reused ordinary turn")
	}
	status, _ = sessCall(t, legacy, 28, []string{"session-1", "ordinary-stop", "legacy-turn", "actor", `{"kind":"execution","payload":{"status":"completed"}}`})
	if status != store.StatusOK || len(legacy.events) != 2 {
		t.Fatal("ordinary turn changed")
	}
	// Same UUID lookup takes precedence even when proposed retry turn is occupied.
	status, cells := sessCall(t, d, 56, []string{"session-1", "12345678-1234-4234-8234-123456789abc", "turn-1", "actor", batchInput(inputAttachmentEvent(t))})
	if status != store.StatusOK || cells[4] != "replay" || len(d.events) != before {
		t.Fatal("exact replay refused", status, cells)
	}
}

func selectedInputEvent(t *testing.T, text, operation string) string {
	t.Helper()
	var event struct {
		Kind      string            `json:"kind"`
		MessageID string            `json:"message_id"`
		Payload   canonical.Message `json:"payload"`
	}
	if err := json.Unmarshal([]byte(inputAttachmentEvent(t)), &event); err != nil {
		t.Fatal(err)
	}
	hash := sha256.Sum256([]byte(text))
	event.Payload.Blocks[0].Text = &text
	event.Payload.Blocks[0].InputAttachment.OperationUUID = operation
	event.Payload.Blocks[0].InputAttachment.TextSHA256 = hex.EncodeToString(hash[:])
	event.Payload.Blocks[0].InputAttachment.Opaque = " {\"wide\":18446744073709551615,\"operation\":\"" + operation + "\"}\n"
	body, err := json.Marshal(event)
	if err != nil || !canonical.ValidSessionEvent(body) {
		t.Fatal("invalid test source", err)
	}
	return string(body)
}
func selectedInputPair(t *testing.T) (*originalHistoryFixture, []canonical.Message) {
	t.Helper()
	d := originalHistorySeed(t)
	var first struct {
		Payload canonical.Message `json:"payload"`
	}
	if err := json.Unmarshal([]byte(inputAttachmentEvent(t)), &first); err != nil {
		t.Fatal(err)
	}
	body, _ := json.Marshal([]canonical.Message{first.Payload})
	// Match the real host: full checkpoint before the first completed terminal.
	status, _ := sessCall(t, d, 57, []string{"session-1", "checkpoint-first", "primary", "actor", "turn-1", string(body)})
	if status != store.StatusOK {
		t.Fatal("first checkpoint", status)
	}
	status, _ = sessCall(t, d, 28, []string{"session-1", "terminal-first", "turn-1", "actor", `{"kind":"execution","payload":{"status":"completed"}}`})
	if status != store.StatusOK {
		t.Fatal("first terminal", status)
	}
	operation := "22345678-1234-4234-8234-123456789abc"
	input := selectedInputEvent(t, "second safe original", operation)
	status, cells := sessCall(t, d, 56, []string{"session-1", operation, "turn-2", "actor", batchInput(input)})
	if status != store.StatusOK || len(cells) != 5 || cells[4] != "created" {
		t.Fatal("actual second atomic56", status, cells)
	}
	var second struct {
		Payload canonical.Message `json:"payload"`
	}
	json.Unmarshal([]byte(input), &second)
	return d, []canonical.Message{first.Payload, second.Payload}
}
func expectSelectedInputs(t *testing.T, d *originalHistoryFixture, opcode uint32, fields []string, want []canonical.Message) {
	t.Helper()
	before := len(d.events)
	status, cells := sessCall(t, d, opcode, fields)
	if status != store.StatusOK || len(cells) != 1 {
		t.Fatalf("selected view opcode%d status%d cells%d", opcode, status, len(cells))
	}
	var got []canonical.Message
	if !canonical.Decode([]byte(cells[0]), &got) || !canonical.InputAttachmentsPreserved(want, got) {
		t.Fatal("selected view lost/changed exact input metadata")
	}
	if len(d.events) != before {
		t.Fatal("read appended events")
	}
}
func TestInputAttachmentSelectedCompletedAndRunningViews(t *testing.T) {
	d, inputs := selectedInputPair(t)
	expectSelectedInputs(t, d, 33, []string{"session-1", "primary", "actor", "turn-2"}, inputs[:1])
	expectSelectedInputs(t, d, 35, []string{"session-1", "actor", "turn-2"}, inputs)
	// A forged active turn cannot conceal the actually open execution.
	status, _ := sessCall(t, d, 33, []string{"session-1", "primary", "actor", "foreign-turn"})
	if status == store.StatusOK {
		t.Fatal("forged current turn selected history")
	}
	// A copied/foreign actor does not turn the private input into an accepted view.
	d.events[len(d.events)-1][2] = "foreign-actor"
	status, _ = sessCall(t, d, 33, []string{"session-1", "primary", "actor", "turn-2"})
	if status == store.StatusOK {
		t.Fatal("foreign source actor selected history")
	}
}
func TestInputAttachmentActiveCheckpointRemainsIncluded(t *testing.T) {
	d, inputs := selectedInputPair(t)
	body, _ := json.Marshal(inputs)
	status, _ := sessCall(t, d, 57, []string{"session-1", "checkpoint-second", "primary", "actor", "turn-2", string(body)})
	if status != store.StatusOK {
		t.Fatal("second exact checkpoint", status)
	}
	// Although turn2 is open, its input is already part of this accepted projection.
	expectSelectedInputs(t, d, 33, []string{"session-1", "primary", "actor", "turn-2"}, inputs)
	status, _ = sessCall(t, d, 28, []string{"session-1", "terminal-second", "turn-2", "actor", `{"kind":"execution","payload":{"status":"completed"}}`})
	if status != store.StatusOK {
		t.Fatal("second terminal", status)
	}
	expectSelectedInputs(t, d, 33, []string{"session-1", "primary", "actor", "turn-2"}, inputs)
	expectSelectedInputs(t, d, 33, []string{"session-1", "primary", "actor", ""}, inputs)
}
func TestInputAttachmentSelectedViewCannotDropIncludedInputs(t *testing.T) {
	for _, which := range []int{0, 1} {
		t.Run(strconv.Itoa(which), func(t *testing.T) {
			d, inputs := selectedInputPair(t)
			stripped := append([]canonical.Message(nil), inputs...)
			stripped[which] = canonical.Message{Role: inputs[which].Role, Blocks: append([]canonical.Block(nil), inputs[which].Blocks...)}
			stripped[which].Blocks[0].InputAttachment = nil
			body, _ := json.Marshal(stripped)
			before := len(d.events)
			status, _ := sessCall(t, d, 57, []string{"session-1", "strip-included", "primary", "actor", "turn-2", string(body)})
			if status == store.StatusOK || len(d.events) != before {
				t.Fatal("writer dropped included input", which, status)
			}
			// Simulate corrupt storage, bypassing the writer. The reducer independently refuses.
			for i, m := range stripped {
				msg, _ := json.Marshal(m)
				payload := `{"kind":"history","payload":{"scope":"primary","phase":"message","index":` + strconv.Itoa(i) + `,"message":` + string(msg) + `}}`
				d.events = append(d.events, []any{strconv.Itoa(len(d.events) + 1), "corrupt-cp", "actor", "corrupt:" + strconv.Itoa(i), payload})
			}
			d.events = append(d.events, []any{strconv.Itoa(len(d.events) + 1), "corrupt-cp", "actor", "corrupt:commit", `{"kind":"history","payload":{"scope":"primary","phase":"commit","count":2}}`})
			status, _ = sessCall(t, d, 33, []string{"session-1", "primary", "actor", "turn-2"})
			if status == store.StatusOK {
				t.Fatal("reducer accepted corrupted input checkpoint", which)
			}
		})
	}
}
