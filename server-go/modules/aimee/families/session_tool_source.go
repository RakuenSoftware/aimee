package families

import (
	"context"
	canonical "github.com/JBailes/aimee/server-go/ir"
	store "github.com/JBailes/aimee/server-go/modules/aimee"
	"strconv"
	"strings"
	"unicode/utf8"
)

// Family 6's highest prior operation is 53. Only actual C Core0 may invoke54.
const opSessionToolSourceAppend uint32 = 54

// A normal provider host has no Codex/Claude reservation. Its authenticated
// Core0 custody still requires an owned running canonical call, not a source
// reference string supplied to the ordinary append/import path.
func sessionToolSourceAppend(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	if len(f) != 5 || !canonical.ValidSessionEvent([]byte(f[4])) || !canonical.EventHasToolSources([]byte(f[4])) {
		return store.StatusInvalid, nil, nil
	}
	for _, identity := range f[:4] {
		if identity == "" || len(identity) > 1024 || !utf8.ValidString(identity) || strings.ContainsRune(identity, 0) {
			return store.StatusInvalid, nil, nil
		}
	}
	var event struct {
		Kind      string            `json:"kind"`
		MessageID string            `json:"message_id"`
		Role      string            `json:"role,omitempty"`
		Payload   canonical.Message `json:"payload"`
	}
	if !canonical.Decode([]byte(f[4]), &event) || event.Kind != "message" || event.Payload.Role != "tool" || len(event.Payload.Blocks) != 1 {
		return store.StatusInvalid, nil, nil
	}
	block := event.Payload.Blocks[0]
	if block.Type != "tool_result" || block.ToolSource == nil || block.ToolID == "" {
		return store.StatusInvalid, nil, nil
	}
	var actor string
	if err := q.QueryRow(ctx, sessionIRLockSQL, f[0]).Scan(&actor); err != nil {
		if store.IsNoRows(err) {
			return store.StatusMissing, nil, nil
		}
		return 0, nil, err
	}
	if actor != f[3] {
		return store.StatusInvalid, nil, nil
	}
	// An immutable identical retry is not another call/result or new authority.
	var sequence int64
	var turn, previousActor, payload string
	err := q.QueryRow(ctx, sessionIRExistingSQL, f[0], f[1]).Scan(&sequence, &turn, &previousActor, &payload)
	if err == nil {
		if turn != f[2] || previousActor != f[3] || payload != f[4] {
			return store.StatusInvalid, nil, nil
		}
		return store.StatusOK, []string{strconv.FormatInt(sequence, 10)}, nil
	}
	if !store.IsNoRows(err) {
		return 0, nil, err
	}
	status, cells, err := sessionIRRunningHistoryLoad(ctx, q, []string{f[0], f[3], f[2]})
	if err != nil || status != store.StatusOK {
		return status, nil, err
	}
	if len(cells) != 1 {
		return store.StatusInvalid, nil, nil
	}
	var history []canonical.Message
	if !canonical.Decode([]byte(cells[0]), &history) {
		return store.StatusInvalid, nil, nil
	}
	pending := false
	for _, message := range history {
		for _, b := range message.Blocks {
			if b.ToolID != block.ToolID {
				continue
			}
			switch b.Type {
			case "tool_use":
				if pending {
					return store.StatusInvalid, nil, nil
				}
				pending = true
			case "tool_result":
				pending = false
			}
		}
	}
	if !pending {
		return store.StatusInvalid, nil, nil
	}
	// Shared transaction owns the actor lock; insertion and learning outbox are atomic.
	return sessionIRAppendSourceAuthorized(ctx, q, f, false, false, false, false, true)
}
