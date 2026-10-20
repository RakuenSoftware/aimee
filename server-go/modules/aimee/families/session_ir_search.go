package families

import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"strings"
	"unicode/utf8"

	canonical "github.com/JBailes/aimee/server-go/ir"
	store "github.com/JBailes/aimee/server-go/modules/aimee"
)

const opSessionIRSearch uint32 = 37
const sessionSearchResponseMax = 2 << 20
const externalSDKHistoryScope = "external-sdk:claude-code"

const sessionSearchExternalCommitSQL = `SELECT sequence::text,turn_id,event_id,payload
 FROM session_ir_events WHERE session_id=$1 AND actor=$2
 AND payload::jsonb->>'kind'='history'
 AND payload::jsonb->'payload'->>'scope'='external-sdk:claude-code'
 AND payload::jsonb->'payload'->>'phase'='commit'
 ORDER BY sequence DESC LIMIT 1`

// Search displays the canonical current-history reducer by default, including
// imported checkpoint chunks. A separately named observations view exposes
// original immutable messages removed by compaction; the two are never guessed
// into a merged conversation or deduplicated by equal text.
// Plain-string tool results are display text; reasoning, structured tool values,
// tool arguments and provider sidecars are not snippets. Classified hidden or
// confidential/restricted blocks never become search text.
// Each message's last full snapshot wins; its first observation orders the view.
const sessionSearchMessagesCTE = `WITH ordered AS (
 SELECT e.*, e.payload::jsonb AS body,
 min(e.sequence) OVER (PARTITION BY e.session_id,e.turn_id,e.payload::jsonb->>'message_id') AS first_seen
 FROM session_ir_events e JOIN server_sessions s ON s.id=e.session_id
 WHERE s.principal=$1 AND e.actor=$1 AND e.payload::jsonb->>'kind' IN ('message','delta')
), observed AS (SELECT *,row_number() OVER (PARTITION BY session_id,turn_id,body->>'message_id' ORDER BY sequence DESC) AS latest FROM ordered WHERE body->>'kind'='message'), messages AS (SELECT * FROM observed WHERE latest=1),
 searchable AS (SELECT session_id,created_at,body FROM messages WHERE $7 <> 'external_sdk' UNION ALL
 SELECT e.session_id,e.created_at,jsonb_build_object('payload',e.payload::jsonb->'payload'->'message')
 FROM session_ir_events e JOIN server_sessions s ON s.id=e.session_id
 WHERE $7 IN ('current','external_sdk') AND s.principal=$1 AND e.actor=$1
 AND e.payload::jsonb->>'kind'='history'
 AND e.payload::jsonb->'payload'->>'scope'=CASE WHEN $7='external_sdk' THEN 'external-sdk:claude-code' ELSE 'primary' END
 AND e.payload::jsonb->'payload'->>'phase'='message') `
const sessionSearchCandidatesSQL = sessionSearchMessagesCTE + `
 SELECT s.id,s.title,s.client_type,
 to_char(s.created_at AT TIME ZONE 'UTC','YYYY-MM-DD HH24:MI:SS'),
 to_char(max(m.created_at) AT TIME ZONE 'UTC','YYYY-MM-DD HH24:MI:SS')
 FROM server_sessions s JOIN searchable m ON m.session_id=s.id
 WHERE s.principal=$1 AND ($2='' OR s.id=$2)
 AND (s.client_type NOT IN ('mcp','tool','internal','delegate') OR s.client_type=ANY($3::text[]))
 AND ($4='' OR s.id ILIKE $5 ESCAPE '\' OR s.title ILIKE $5 ESCAPE '\' OR EXISTS (
 SELECT 1 FROM searchable candidate
 CROSS JOIN LATERAL (
 SELECT string_agg(CASE WHEN block->>'type'='text' THEN block->>'text'
                        ELSE block->>'content' END,E'\n' ORDER BY ordinal) AS text
 FROM jsonb_array_elements(candidate.body->'payload'->'blocks') WITH ORDINALITY AS blocks(block,ordinal)
 WHERE (NOT (block ? 'context') OR block->'context'='null'::jsonb OR (block->'context'->>'model_visible'='true'
      AND coalesce(block->'context'->>'sensitivity','0') IN ('0','1')))
 AND ((block->>'type'='text' AND jsonb_typeof(block->'text')='string')
   OR (block->>'type'='tool_result' AND jsonb_typeof(block->'content')='string'))
 ) visible
 WHERE candidate.session_id=s.id AND visible.text ILIKE $5 ESCAPE '\'))
 GROUP BY s.id ORDER BY max(m.created_at) DESC,s.id LIMIT $6 OFFSET $8`

// PostgreSQL disallows FOR SHARE on grouped queries. Lock each admitted source
// separately before consuming it; every read also joins its current exact owner.
const sessionSearchWatermarkSQL = `SELECT coalesce(max(e.sequence),0)::text FROM session_ir_events e JOIN server_sessions s ON s.id=e.session_id WHERE e.session_id=$1 AND e.actor=$2 AND s.principal=$2`
const sessionSearchForeignActorSQL = `SELECT EXISTS(SELECT 1 FROM session_ir_events WHERE session_id=$1 AND actor<>$2)`
const sessionSearchSourceLockSQL = `SELECT id FROM server_sessions WHERE id=$1 AND principal=$2 FOR SHARE`
const sessionSearchMessagesSQL = `WITH ordered AS (
 SELECT e.*,min(e.sequence) OVER (PARTITION BY e.turn_id,e.payload::jsonb->>'message_id') AS first_seen
 FROM session_ir_events e JOIN server_sessions s ON s.id=e.session_id
 WHERE s.principal=$1 AND e.actor=$1 AND e.session_id=$2 AND e.payload::jsonb->>'kind' IN ('message','delta')),
 observed AS (SELECT *,row_number() OVER (PARTITION BY turn_id,payload::jsonb->>'message_id' ORDER BY sequence DESC) AS latest FROM ordered WHERE payload::jsonb->>'kind'='message')
 SELECT sequence::text,turn_id,payload FROM observed WHERE latest=1 ORDER BY first_seen,sequence`

type sessionSearchRequest struct {
	Query     string   `json:"query"`
	Session   string   `json:"session_id"`
	Around    *int     `json:"around_message_id"`
	Window    *int     `json:"window"`
	Limit     *int     `json:"limit"`
	Include   []string `json:"include_sources"`
	Principal string   `json:"_principal"`
	View      string   `json:"view"`
	Revision  string   `json:"source_revision"`
	Offset    int      `json:"offset"`
}
type searchMessage struct {
	ID        int    `json:"message_id"`
	Role      string `json:"role"`
	Text      string `json:"text"`
	Sequence  string `json:"sequence,omitempty"`
	Turn      string `json:"turn_id,omitempty"`
	SourceID  string `json:"source_message_id,omitempty"`
	Truncated bool   `json:"text_truncated,omitempty"`
}
type searchSession struct{ id, title, source, created, updated string }

func searchClamp(value *int, fallback, minimum, maximum int) int {
	if value == nil {
		return fallback
	}
	if *value < minimum {
		return minimum
	}
	if *value > maximum {
		return maximum
	}
	return *value
}
func searchClip(text, query string) (string, bool) {
	if utf8.RuneCountInString(text) <= 4096 {
		return text, false
	}
	runes := []rune(text)
	start := 0
	if query != "" {
		if at := strings.Index(strings.ToLower(text), strings.ToLower(query)); at >= 0 {
			// The lowercased byte offset is only a display hint, never a source identity.
			if at > len(text) {
				at = len(text)
			}
			start = utf8.RuneCountInString(text[:at]) - 1024
		}
	}
	if start < 0 {
		start = 0
	}
	if start > len(runes)-4096 {
		start = len(runes) - 4096
	}
	return string(runes[start : start+4096]), true
}
func searchText(message canonical.Message) string {
	parts := make([]string, 0, len(message.Blocks))
	for _, block := range message.Blocks {
		if block.Context != nil && (!block.Context.ModelVisible || block.Context.Sensitivity < 0 || block.Context.Sensitivity > 1) {
			continue
		}
		if block.Type == "text" && block.Text != nil {
			parts = append(parts, *block.Text)
		} else if block.Type == "tool_result" {
			var text string
			if len(block.Content) > 0 && block.Content[0] == '"' && json.Unmarshal(block.Content, &text) == nil {
				parts = append(parts, text)
			}
		}
	}
	return strings.Join(parts, "\n")
}
func sessionSearchRead(ctx context.Context, q store.Queryer, actor string, session searchSession, request sessionSearchRequest, window int) (map[string]any, error) {
	var locked string
	if err := q.QueryRow(ctx, sessionSearchSourceLockSQL, session.id, actor).Scan(&locked); err != nil {
		return nil, err
	}
	head, tail, ring, selected := []searchMessage{}, []searchMessage{}, []searchMessage{}, []searchMessage{}
	match := -1
	snippet := ""
	preview := ""
	count := 0
	var watermark string
	if err := q.QueryRow(ctx, sessionSearchWatermarkSQL, session.id, actor).Scan(&watermark); err != nil {
		return nil, err
	}
	digest := sha256.New()
	prefix, _ := json.Marshal([]string{actor, session.id, request.View, watermark})
	digest.Write(prefix)
	add := func(input canonical.Message, sequence, turn, id, raw string) {
		digest.Write([]byte(sequence + "\x00" + turn + "\x00" + raw + "\x00"))
		text := searchText(input)
		clipped, truncated := searchClip(text, request.Query)
		message := searchMessage{count, input.Role, clipped, sequence, turn, id, truncated}
		if preview == "" && clipped != "" {
			preview = clipped
		}
		if len(head) < window+1 || len(head) < 3 {
			head = append(head, message)
		}
		tail = append(tail, message)
		if len(tail) > 3 {
			tail = tail[1:]
		}
		ring = append(ring, message)
		if len(ring) > window+1 {
			ring = ring[1:]
		}
		if request.Query != "" {
			if match < 0 && strings.Contains(strings.ToLower(text), strings.ToLower(request.Query)) {
				match = count
				snippet = clipped
				selected = append(selected, ring...)
			} else if match >= 0 && count <= match+window {
				selected = append(selected, message)
			}
		} else if request.Around != nil && count >= *request.Around-window && count <= *request.Around+window {
			selected = append(selected, message)
		}
		count++
	}
	if request.View == "current" || request.View == "external_sdk" {
		var foreign bool
		if err := q.QueryRow(ctx, sessionSearchForeignActorSQL, session.id, actor).Scan(&foreign); err != nil {
			return nil, err
		}
		if foreign {
			return nil, store.ErrNoRows
		}
		scope := "primary"
		if request.View == "external_sdk" {
			scope = externalSDKHistoryScope
			var sequence, turn, eventID, payload string
			if err := q.QueryRow(ctx, sessionSearchExternalCommitSQL, session.id, actor).Scan(&sequence, &turn, &eventID, &payload); err != nil {
				return nil, err
			}
			// Equal messages from distinct SDK snapshots are distinct source custody.
			// Bind the selected commit even when unrelated newer host events keep
			// the global watermark unchanged after source erasure.
			source, _ := json.Marshal([]string{sequence, turn, eventID, payload})
			digest.Write(source)
		}
		digest.Write([]byte(scope + "\x00"))
		status, cells, err := sessionIRHistoryLoad(ctx, q, []string{session.id, scope, actor, ""})
		if errors.Is(err, errSessionIRUnfinishedExecution) {
			if request.Session == "" {
				return nil, store.ErrNoRows
			}
			return nil, fmt.Errorf("current canonical history is unfinished; use view observations to inspect original messages: %w", err)
		}
		if err != nil {
			return nil, err
		}
		if status == store.StatusMissing {
			return nil, store.ErrNoRows
		}
		if status != store.StatusOK || len(cells) != 1 {
			return nil, fmt.Errorf("canonical search history unavailable")
		}
		var messages []json.RawMessage
		if json.Unmarshal([]byte(cells[0]), &messages) != nil {
			return nil, fmt.Errorf("invalid canonical search history")
		}
		for _, raw := range messages {
			var input canonical.Message
			if !canonical.ValidMessage(raw) || !canonical.Decode(raw, &input) {
				return nil, fmt.Errorf("invalid canonical search message")
			}
			add(input, "", "", "", string(raw))
		}
	} else {
		rows, err := q.Query(ctx, sessionSearchMessagesSQL, actor, session.id)
		if err != nil {
			return nil, err
		}
		defer rows.Close()
		for rows.Next() {
			var sequence, turn, payload string
			if err := rows.Scan(&sequence, &turn, &payload); err != nil {
				return nil, err
			}
			var event struct {
				Kind    string            `json:"kind"`
				ID      string            `json:"message_id"`
				Payload canonical.Message `json:"payload"`
			}
			if !canonical.ValidSessionEvent([]byte(payload)) || json.Unmarshal([]byte(payload), &event) != nil || event.Kind != "message" {
				return nil, fmt.Errorf("invalid canonical search source")
			}
			add(event.Payload, sequence, turn, event.ID, payload)
		}
		if err := rows.Err(); err != nil {
			return nil, err
		}
	}
	revision := hex.EncodeToString(digest.Sum(nil))
	if request.Revision != "" && request.Revision != revision {
		return nil, fmt.Errorf("canonical search source revision changed")
	}
	result := map[string]any{"session_id": session.id, "agent_name": "", "provider": "", "title": session.title, "source": session.source, "started_at": session.created, "updated_at": session.updated, "message_count": count, "view": request.View, "source_revision": revision}
	if request.Query != "" && match < 0 && !strings.Contains(strings.ToLower(session.id), strings.ToLower(request.Query)) && !strings.Contains(strings.ToLower(session.title), strings.ToLower(request.Query)) {
		return nil, store.ErrNoRows
	}
	if request.Query != "" {
		if match < 0 {
			selected = head
			if len(selected) > window+1 {
				selected = selected[:window+1]
			}
		}
		start := head
		if len(start) > 3 {
			start = start[:3]
		}
		result["match"] = map[string]any{"message_id": match, "snippet": snippet}
		result["window"] = selected
		result["bookend_start"] = start
		result["bookend_end"] = tail
	} else if request.Around != nil {
		result["mode"] = "scroll"
		result["found"] = true
		result["around_message_id"] = *request.Around
		result["window"] = selected
	} else {
		result["preview"] = preview
	}
	return result, nil
}
func sessionIRSearch(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	var request sessionSearchRequest
	if len(f) != 2 || f[0] == "" || len(f[0]) > 1024 || len(f[1]) > 16384 || !canonical.Decode([]byte(f[1]), &request) || request.Principal != f[0] || len(request.Query) > 4096 || len(request.Session) > 1024 || len(request.Include) > 16 || request.Offset < 0 {
		return store.StatusInvalid, nil, nil
	}
	if request.View == "" {
		request.View = "current"
	}
	if request.View != "current" && request.View != "observations" && request.View != "external_sdk" {
		return store.StatusInvalid, nil, nil
	}
	if request.Revision != "" && (len(request.Revision) != 64 || strings.Trim(request.Revision, "0123456789abcdef") != "" || request.Session == "" || request.Query != "") {
		return store.StatusInvalid, nil, nil
	}
	for _, source := range request.Include {
		if len(source) > 128 {
			return store.StatusInvalid, nil, nil
		}
	}
	if request.Session != "" && request.Query == "" && request.Offset != 0 {
		return store.StatusInvalid, nil, nil
	}
	if request.Around != nil && *request.Around < 0 {
		*request.Around = 0
	}
	if request.Query != "" {
		request.Session = ""
		request.Around = nil
	}
	if request.Session == "" {
		request.Around = nil
	}
	window := searchClamp(request.Window, 5, 0, 20)
	limit := searchClamp(request.Limit, 5, 1, 20)
	includes := request.Include
	if includes == nil {
		includes = []string{}
	}
	rows, err := q.Query(ctx, sessionSearchCandidatesSQL, f[0], request.Session, includes, request.Query, likeContains(request.Query), limit*4, request.View, request.Offset)
	if err != nil {
		return 0, nil, err
	}
	candidates := []searchSession{}
	for rows.Next() {
		var row searchSession
		if err = rows.Scan(&row.id, &row.title, &row.source, &row.created, &row.updated); err != nil {
			rows.Close()
			return 0, nil, err
		}
		candidates = append(candidates, row)
	}
	err = rows.Err()
	rows.Close()
	if err != nil {
		return 0, nil, err
	}
	mode := "browse"
	if request.Query != "" {
		mode = "discovery"
	}
	if request.Around != nil {
		mode = "scroll"
	}
	result := map[string]any{"mode": mode, "sessions": []any{}, "count": 0, "view": request.View}
	if mode == "discovery" {
		result["query"] = request.Query
	}
	if mode == "scroll" {
		result = map[string]any{"mode": "scroll", "session_id": request.Session, "found": false, "window": []searchMessage{}}
	}
	sessions := []any{}
	consumed := 0
	for _, candidate := range candidates {
		session, readErr := sessionSearchRead(ctx, q, f[0], candidate, request, window)
		if store.IsNoRows(readErr) {
			consumed++
			continue
		}
		if readErr != nil {
			return 0, nil, readErr
		}
		if mode == "scroll" {
			result = session
			break
		}
		// Admit whole sessions against the actual encoded size, including JSON
		// escapes and continuation metadata. The candidate remains unconsumed
		// when it belongs on the next page; no matching source is skipped.
		page := append(sessions, session)
		result["sessions"] = page
		result["count"] = len(page)
		result["has_more"] = true
		result["next_offset"] = request.Offset + consumed + 1
		encoded, encodeErr := json.Marshal(result)
		if encodeErr != nil {
			return 0, nil, encodeErr
		}
		if len(encoded) > sessionSearchResponseMax {
			if len(sessions) == 0 {
				return 0, nil, fmt.Errorf("canonical session search display exceeds 2 MiB; reduce window or shorten session metadata")
			}
			break
		}
		sessions = page
		consumed++
		if len(sessions) == limit {
			break
		}
	}
	if mode != "scroll" {
		delete(result, "has_more")
		delete(result, "next_offset")
		if consumed < len(candidates) || len(candidates) == limit*4 {
			result["has_more"] = true
			result["next_offset"] = request.Offset + consumed
		}
		result["sessions"] = sessions
		result["count"] = len(sessions)
	}
	encoded, err := json.Marshal(result)
	if err != nil {
		return 0, nil, err
	}
	if len(encoded) > sessionSearchResponseMax {
		return 0, nil, fmt.Errorf("canonical session search display exceeds 2 MiB; reduce window or shorten session metadata")
	}
	return store.StatusOK, []string{string(encoded)}, nil
}
