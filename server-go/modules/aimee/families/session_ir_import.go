package families

import (
	"bytes"
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"fmt"
	canonical "github.com/JBailes/aimee/server-go/ir"
	store "github.com/JBailes/aimee/server-go/modules/aimee"
	"reflect"
	"strconv"
	"strings"
	"unicode/utf8"
)

const opSessionIRHistoryImport uint32 = 50

// Default fields may be materialized; every original value must remain exact.
func historyImportPreserves(before, after any) bool {
	switch b := before.(type) {
	case map[string]any:
		a, ok := after.(map[string]any)
		if !ok {
			return false
		}
		for k, v := range b {
			actual, exists := a[k]
			if !exists || !historyImportPreserves(v, actual) {
				return false
			}
		}
		return true
	case []any:
		a, ok := after.([]any)
		if !ok || len(a) != len(b) {
			return false
		}
		for i := range b {
			if !historyImportPreserves(b[i], a[i]) {
				return false
			}
		}
		return true
	default:
		return reflect.DeepEqual(before, after)
	}
}

func historyImportChunks(raw string) []string {
	parts := []string{}
	for len(raw) > 0 {
		n := len(raw)
		if n > 65536 {
			n = 65536
			for n > 0 && !utf8.RuneStart(raw[n]) {
				n--
			}
		}
		parts = append(parts, raw[:n])
		raw = raw[n:]
	}
	return parts
}

// Host-only one-time source migration. The source row and its hash are checked
// under owner locks, and report/archive/checkpoint commit in one transaction.
func sessionIRHistoryImport(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	if len(f) != 9 || (f[8] != "model" && f[8] != "browse") || f[0] == "" || len(f[0]) >= 128 || f[1] == "" || len(f[1]) > 128 || f[2] == "" || len(f[2]) >= 160 || f[3] == "" || len(f[3]) >= 128 || len(f[4]) >= 64 {
		return store.StatusInvalid, nil, nil
	}
	for _, v := range f[:5] {
		if !utf8.ValidString(v) || strings.ContainsRune(v, 0) {
			return store.StatusInvalid, nil, nil
		}
	}
	var report canonical.HistoryImportReport
	var messages []json.RawMessage
	if !canonical.DecodeHistoryImportReport([]byte(f[7]), &report) || !canonical.Decode([]byte(f[6]), &messages) || messages == nil || len(messages) != report.ImportedMessages || report.SourceSession != f[3] || report.SourceAgent != f[4] || report.SourceSHA256 != f[5] {
		return store.StatusInvalid, nil, nil
	}
	for _, message := range messages {
		if !canonical.ValidMessage(message) {
			return store.StatusInvalid, nil, nil
		}
	}
	rows, err := q.Query(ctx, `SELECT id,principal FROM server_sessions WHERE id=$1 OR id=$2 ORDER BY id FOR UPDATE`, f[0], f[3])
	if err != nil {
		return 0, nil, err
	}
	owned := map[string]bool{}
	for rows.Next() {
		var id, actor string
		if err = rows.Scan(&id, &actor); err != nil {
			rows.Close()
			return 0, nil, err
		}
		if actor != f[2] {
			rows.Close()
			return store.StatusInvalid, nil, nil
		}
		owned[id] = true
	}
	err = rows.Err()
	rows.Close()
	if err != nil {
		return 0, nil, err
	}
	if !owned[f[0]] || !owned[f[3]] {
		return store.StatusInvalid, nil, nil
	}
	provider := report.SourceProvider
	raw, err := legacyPrimaryText(ctx, q, f[3], f[4], provider)
	if store.IsNoRows(err) {
		return store.StatusMissing, nil, nil
	}
	if err != nil {
		return 0, nil, err
	}
	sum := sha256.Sum256([]byte(raw))
	if hex.EncodeToString(sum[:]) != f[5] || report.SourceProvider != provider {
		return store.StatusInvalid, nil, nil
	}
	var source []json.RawMessage
	sourceArray := canonical.Decode([]byte(raw), &source) && source != nil
	screen := provider == "tmux" || provider == "tmux-cli"
	format := "unknown"
	sourceCount := 0
	if screen {
		format = "tmux_text"
	} else if sourceArray {
		sourceCount = len(source)
		isCanonical := true
		for _, m := range source {
			var object map[string]json.RawMessage
			if !canonical.Decode(m, &object) || object["blocks"] == nil {
				isCanonical = false
				break
			}
		}
		if isCanonical {
			format = "canonical"
		} else if provider == "anthropic" {
			format = "anthropic"
		} else if provider == "chatgpt" {
			format = "responses"
		} else {
			format = "openai_chat"
		}
	}
	if report.SourceFormat != format || report.SourceMessages != sourceCount {
		return store.StatusInvalid, nil, nil
	}
	if report.Outcome == "exact" {
		var before, after any
		a := json.NewDecoder(bytes.NewReader([]byte(raw)))
		a.UseNumber()
		b := json.NewDecoder(bytes.NewReader([]byte(f[6])))
		b.UseNumber()
		if a.Decode(&before) != nil || b.Decode(&after) != nil || !historyImportPreserves(before, after) {
			return store.StatusInvalid, nil, nil
		}
	}
	if (report.Outcome == "archived" || report.Outcome == "refused") && len(messages) != 0 {
		return store.StatusInvalid, nil, nil
	}
	seed, _ := json.Marshal([]string{f[0], f[1], f[2], f[3], f[4], f[5], provider})
	identity := sha256.Sum256(seed)
	checkpoint := "history-import:" + hex.EncodeToString(identity[:])

	if report.Outcome == "exact" || report.Outcome == "normalized" {
		var exists, conflict bool
		err = q.QueryRow(ctx, `SELECT EXISTS(SELECT 1 FROM session_ir_events WHERE session_id=$1 AND turn_id=$2),EXISTS(SELECT 1 FROM session_ir_events e WHERE e.session_id=$1 AND ((e.payload::jsonb->>'kind'='history' AND e.payload::jsonb->'payload'->>'phase'='commit' AND e.payload::jsonb->'payload'->>'scope'=$3 AND e.turn_id<>$2) OR (e.payload::jsonb->>'kind'='execution' AND e.payload::jsonb->'payload'->>'status'<>'running' AND EXISTS(SELECT 1 FROM session_ir_events m WHERE m.session_id=e.session_id AND m.turn_id=e.turn_id AND m.payload::jsonb->>'kind'='message'))))`, f[0], checkpoint, f[1]).Scan(&exists, &conflict)
		if err != nil {
			return 0, nil, err
		}
		if conflict && !exists {
			if f[8] == "browse" {
				return store.StatusOK, []string{"skipped"}, nil
			}
			return store.StatusInvalid, nil, nil
		}
	}
	appendHistory := func(index int, h canonical.HistoryEvent) (uint32, error) {
		var body any = h
		if h.Phase == "archive" {
			body = struct {
				Scope string  `json:"scope"`
				Phase string  `json:"phase"`
				Index int     `json:"index"`
				Text  *string `json:"archive_text"`
			}{h.Scope, h.Phase, h.Index, h.ArchiveText}
		}
		event, err := json.Marshal(struct {
			Kind    string `json:"kind"`
			Payload any    `json:"payload"`
		}{"history", body})
		if err != nil {
			return 0, err
		}
		if !canonical.ValidSessionEvent(event) {
			return store.StatusInvalid, nil
		}
		status, _, err := sessionIRAppendImportAuthorized(ctx, q, []string{f[0], checkpoint + ":" + strconv.Itoa(index), checkpoint, f[2], string(event)}, false, false, false, true)
		return status, err
	}

	at := 0
	if report.Outcome == "archived" {
		chunks := historyImportChunks(raw)
		if len(chunks) != report.ArchiveChunks || len(chunks) > 4096 {
			return store.StatusInvalid, nil, nil
		}
		for i := range chunks {
			h := canonical.HistoryEvent{Scope: "legacy-archive", Phase: "archive", Index: i, ArchiveText: &chunks[i]}
			status, err := appendHistory(at, h)
			if err != nil || status != store.StatusOK {
				return status, nil, err
			}
			at++
		}
	}
	scope := f[1]
	if report.Outcome == "archived" {
		scope = "legacy-archive"
	}
	status, err := appendHistory(at, canonical.HistoryEvent{Scope: scope, Phase: "import_report", ImportReport: &report})
	if err != nil || status != store.StatusOK {
		return status, nil, err
	}
	at++
	if report.Outcome == "refused" || report.Outcome == "archived" {
		return store.StatusOK, []string{"imported"}, nil
	}
	// The original source remains untouched even after acknowledged migration.
	for i, message := range messages {
		// One full message may exceed the existing event limit, while the complete
		// import remains within the existing bus frame. Preserve its exact bytes.
		if len(message) > 65536 {
			sum := sha256.Sum256(message)
			digest := hex.EncodeToString(sum[:])
			parts := historyImportChunks(string(message))
			if len(parts) > 4096 {
				return store.StatusInvalid, nil, nil
			}
			for part, text := range parts {
				fragment := &canonical.HistoryMessageFragment{MessageIndex: i, PartIndex: part, PartCount: len(parts), MessageSHA256: digest, Text: text}
				status, err := appendHistory(at, canonical.HistoryEvent{Scope: f[1], Phase: "message_fragment", Fragment: fragment})
				if err != nil || status != store.StatusOK {
					return status, nil, err
				}
				at++
			}
		} else {
			status, err := appendHistory(at, canonical.HistoryEvent{Scope: f[1], Phase: "message", Index: i, Message: message})
			if err != nil || status != store.StatusOK {
				return status, nil, err
			}
			at++
		}
	}

	status, err = appendHistory(at, canonical.HistoryEvent{Scope: f[1], Phase: "commit", Count: len(messages)})
	if err != nil {
		return 0, nil, fmt.Errorf("history import commit: %w", err)
	}
	if status == store.StatusOK {
		return status, []string{"imported"}, nil
	}
	return status, nil, nil
}
