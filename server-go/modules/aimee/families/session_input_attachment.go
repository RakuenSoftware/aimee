package families

import (
	"context"
	"encoding/json"
	canonical "github.com/JBailes/aimee/server-go/ir"
	store "github.com/JBailes/aimee/server-go/modules/aimee"
	"strconv"
	"strings"
	"unicode/utf8"
)

// CANDIDATE allocation only; public operation catalog is unchanged.
const opSessionInputAttachmentAppend uint32 = 56
const opSessionInputAttachmentHistorySave uint32 = 57

func sessionInputAttachmentAppend(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	if len(f) != 5 || len(f[4]) > 1048576 {
		return store.StatusInvalid, nil, nil
	}
	for _, s := range f[:4] {
		if s == "" || len(s) > 1024 || !utf8.ValidString(s) || strings.ContainsRune(s, 0) {
			return store.StatusInvalid, nil, nil
		}
	}
	var batch struct {
		SchemaVersion int             `json:"schema_version"`
		Begin         json.RawMessage `json:"begin"`
		Input         json.RawMessage `json:"input"`
	}
	if !canonical.Decode([]byte(f[4]), &batch) || batch.SchemaVersion != 1 || !canonical.ValidSessionEvent(batch.Begin) || !canonical.ValidSessionEvent(batch.Input) {
		return store.StatusInvalid, nil, nil
	}
	var begin struct {
		Kind    string `json:"kind"`
		Payload struct {
			Status string `json:"status"`
		} `json:"payload"`
	}
	if !canonical.Decode(batch.Begin, &begin) || begin.Kind != "execution" || begin.Payload.Status != "running" {
		return store.StatusInvalid, nil, nil
	}
	var input struct {
		Kind      string            `json:"kind"`
		MessageID string            `json:"message_id"`
		Payload   canonical.Message `json:"payload"`
	}
	if !canonical.Decode(batch.Input, &input) || input.Kind != "message" || input.Payload.Role != "user" || len(input.Payload.Blocks) != 1 || input.Payload.Blocks[0].Type != "text" || input.Payload.Blocks[0].InputAttachment == nil || input.Payload.Blocks[0].InputAttachment.OperationUUID != f[1] {
		return store.StatusInvalid, nil, nil
	}
	var owner string
	if err := q.QueryRow(ctx, sessionIRLockSQL, f[0]).Scan(&owner); err != nil {
		if store.IsNoRows(err) {
			return store.StatusMissing, nil, nil
		}
		return 0, nil, err
	}
	if owner != f[3] {
		return store.StatusInvalid, nil, nil
	}
	prefix := "host-original-input-batch-v1:" + f[1]
	type row struct {
		seq                  int64
		turn, actor, payload string
		found                bool
	}
	read := func(key string) (row, error) {
		var r row
		err := q.QueryRow(ctx, sessionIRExistingSQL, f[0], key).Scan(&r.seq, &r.turn, &r.actor, &r.payload)
		if store.IsNoRows(err) {
			return r, nil
		}
		r.found = err == nil
		return r, err
	}
	a, err := read(prefix + ":begin")
	if err != nil {
		return 0, nil, err
	}
	b, err := read(prefix + ":input")
	if err != nil {
		return 0, nil, err
	}
	if a.found || b.found {
		if !a.found || !b.found || a.seq >= b.seq || a.turn != b.turn || a.actor != f[3] || b.actor != f[3] || a.payload != string(batch.Begin) || b.payload != string(batch.Input) {
			return store.StatusInvalid, nil, nil
		}
		var interleaved bool
		err = q.QueryRow(ctx, originalInputInterleavedSQL, f[0], a.seq, b.seq).Scan(&interleaved)
		if err != nil {
			return 0, nil, err
		}
		if interleaved {
			return store.StatusInvalid, nil, nil
		}
		return store.StatusOK, []string{strconv.FormatInt(a.seq, 10), strconv.FormatInt(b.seq, 10), a.turn, "1", "replay"}, nil
	}
	// A newly admitted input owns a new turn. Retry reconciliation above must
	// precede this check so a changed caller turn cannot invalidate exact replay.
	var occupied bool
	if err := q.QueryRow(ctx, originalInputTurnOccupiedSQL, f[0], f[2]).Scan(&occupied); err != nil {
		return 0, nil, err
	}
	if occupied {
		return store.StatusInvalid, nil, nil
	}
	// One registered Tx=true operation: any failed second insert/outbox must roll
	// back the first. Real PostgreSQL transaction qualification is still required.
	status, first, err := sessionIRAppendInputAuthorized(ctx, q, []string{f[0], prefix + ":begin", f[2], f[3], string(batch.Begin)}, false, false, false, false, false, false)
	if err != nil || status != store.StatusOK {
		return status, nil, err
	}
	status, last, err := sessionIRAppendInputAuthorized(ctx, q, []string{f[0], prefix + ":input", f[2], f[3], string(batch.Input)}, false, false, false, false, false, true)
	if err != nil || status != store.StatusOK {
		return status, nil, err
	}
	if len(first) != 1 || len(last) != 1 {
		return store.StatusInvalid, nil, nil
	}
	return status, []string{first[0], last[0], f[2], "1", "created"}, nil
}

const originalInputInterleavedSQL = `SELECT EXISTS(SELECT 1 FROM session_ir_events WHERE session_id=$1 AND sequence>$2 AND sequence<$3 LIMIT 1)`

func sessionInputAttachmentHistorySave(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	ctx, finishTiming := startHistoryTiming(ctx, historyTimingCheckpoint, -1)
	defer finishTiming()
	if len(f) != 6 {
		return store.StatusInvalid, nil, nil
	}
	next, err := canonical.DecodeMessages([]byte(f[5]))
	if err != nil {
		return store.StatusInvalid, nil, nil
	}
	var owner string
	if err = q.QueryRow(ctx, sessionIRLockSQL, f[0]).Scan(&owner); err != nil {
		if store.IsNoRows(err) {
			return store.StatusMissing, nil, nil
		}
		return 0, nil, err
	}
	if owner != f[3] {
		return store.StatusInvalid, nil, nil
	}
	status, cells, err := sessionIRRunningHistoryLoad(ctx, q, []string{f[0], f[3], f[4]})
	if err != nil || status != store.StatusOK {
		return status, nil, err
	}
	if len(cells) != 1 {
		return store.StatusInvalid, nil, nil
	}
	current, err := canonical.DecodeMessages([]byte(cells[0]))
	if err != nil || !canonical.InputAttachmentsPreserved(current, next) || !toolSourceHistoryPreserved(current, next) {
		return store.StatusInvalid, nil, nil
	}
	status, _, err = sessionIRHistorySaveInputAuthorized(ctx, q, []string{f[0], f[1], f[2], f[3], f[5]}, true, true)
	if err != nil || status != store.StatusOK {
		return status, nil, err
	}
	return status, []string{"1"}, nil
}

// Called only with the real session owner row locked. Ordinary writers cannot
// represent an admitted attachment, so primary replacements must use op57.
// Non-primary histories and ordinary appended observations remain unchanged.
const originalInputPresenceSQL = `/* original-input-presence-v1 */ SELECT EXISTS(
 SELECT 1 FROM session_ir_events e WHERE e.session_id=$1
 AND e.payload::jsonb->>'kind'='message'
 AND EXISTS(SELECT 1 FROM jsonb_array_elements(CASE WHEN jsonb_typeof(e.payload::jsonb->'payload'->'blocks')='array' THEN e.payload::jsonb->'payload'->'blocks' ELSE '[]'::jsonb END) b WHERE b ? 'input_attachment'))`

func ordinaryInputHistoryAllowed(ctx context.Context, q store.Queryer, session, scope string) (bool, error) {
	if scope != "primary" {
		return true, nil
	}
	var attached bool
	err := q.QueryRow(ctx, originalInputPresenceSQL, session).Scan(&attached)
	if err != nil {
		return false, err
	}
	return !attached, nil
}

// Exact source identity, not a caller-supplied authority badge. Checked under
// the same owner lock before any ordinary replacement message insert.
const originalInputMessageSQL = `/* original-input-message-v1 */ SELECT EXISTS(
 SELECT 1 FROM session_ir_events e WHERE e.session_id=$1 AND e.turn_id=$2
 AND e.payload::jsonb->>'kind'='message' AND e.payload::jsonb->>'message_id'=$3
 AND EXISTS(SELECT 1 FROM jsonb_array_elements(CASE WHEN jsonb_typeof(e.payload::jsonb->'payload'->'blocks')='array' THEN e.payload::jsonb->'payload'->'blocks' ELSE '[]'::jsonb END) b WHERE b ? 'input_attachment'))`

const originalInputTurnOccupiedSQL = `SELECT EXISTS(SELECT 1 FROM session_ir_events WHERE session_id=$1 AND turn_id=$2 LIMIT 1)`
