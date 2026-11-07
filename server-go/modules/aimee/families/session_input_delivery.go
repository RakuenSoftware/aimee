package families

import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	canonical "github.com/JBailes/aimee/server-go/ir"
	store "github.com/JBailes/aimee/server-go/modules/aimee"
	"strconv"
	"strings"
	"unicode"
	"unicode/utf8"
)

const (
	opSessionInputDeliveryBegin uint32 = 58
	opSessionInputDeliveryGet   uint32 = 59
	opSessionInputDeliveryPage  uint32 = 60
	opSessionInputDeliveryAck   uint32 = 61
)

type inputDeliveryIntent struct {
	SchemaVersion int    `json:"schema_version"`
	Receiver      string `json:"receiver_tag"`
	Key           string `json:"intent_key"`
	Opaque        string `json:"opaque_intent"`
}

func deliveryText(value string, max int) bool {
	return value != "" && len(value) <= max && utf8.ValidString(value) && !strings.ContainsFunc(value, unicode.IsControl)
}
func deliveryHash(value string) string {
	digest := sha256.Sum256([]byte(value))
	return hex.EncodeToString(digest[:])
}
func deliveryDigest(value string) bool {
	if len(value) != 64 {
		return false
	}
	for _, c := range value {
		if !strings.ContainsRune("0123456789abcdef", c) {
			return false
		}
	}
	return true
}
func sessionInputDeliveryBegin(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	if len(f) != 6 || len(f[5]) > 32768 {
		return store.StatusInvalid, nil, nil
	}
	var intent inputDeliveryIntent
	if !canonical.Decode([]byte(f[5]), &intent) || intent.SchemaVersion != 1 || !deliveryText(intent.Receiver, 128) || !deliveryDigest(intent.Key) || !utf8.ValidString(intent.Opaque) || len(intent.Opaque) == 0 || len(intent.Opaque) > 16384 || strings.ContainsRune(intent.Opaque, 0) {
		return store.StatusInvalid, nil, nil
	}
	// The legacy operation validates and locks the actual owner, the exact event
	// pair, replay identity and proposed-turn vacancy. Its wire remains unchanged.
	status, receipt, err := sessionInputAttachmentAppend(ctx, q, f[:5])
	if err != nil || status != store.StatusOK {
		return status, nil, err
	}
	if len(receipt) != 5 {
		return store.StatusInvalid, nil, nil
	}
	var id, receiver, key string
	err = q.QueryRow(ctx, `SELECT id::text,receiver_tag,intent_key FROM session_input_deliveries WHERE session_id=$1 AND operation_uuid=$2 FOR UPDATE`, f[0], f[1]).Scan(&id, &receiver, &key)
	if receipt[4] == "replay" {
		// An ordinary historical56 row is not an approved delivery. Never retrofit.
		if store.IsNoRows(err) {
			return store.StatusInvalid, nil, nil
		}
		if err != nil {
			return 0, nil, err
		}
		if receiver != intent.Receiver || key != intent.Key {
			return store.StatusInvalid, nil, nil
		}
		return store.StatusOK, append(receipt, id), nil
	}
	if !store.IsNoRows(err) {
		if err != nil {
			return 0, nil, err
		}
		return store.StatusInvalid, nil, nil
	}
	var batch struct {
		SchemaVersion int             `json:"schema_version"`
		Begin         json.RawMessage `json:"begin"`
		Input         json.RawMessage `json:"input"`
	}
	if !canonical.Decode([]byte(f[4]), &batch) {
		return store.StatusInvalid, nil, nil
	}
	err = q.QueryRow(ctx, `INSERT INTO session_input_deliveries(session_id,operation_uuid,actor,receiver_tag,intent_key,opaque_intent,begin_sequence,input_sequence,original_turn,begin_sha256,input_sha256) VALUES($1,$2,$3,$4,$5,$6,$7::text::bigint,$8::text::bigint,$9,$10,$11) RETURNING id::text`, f[0], f[1], f[3], intent.Receiver, intent.Key, intent.Opaque, receipt[0], receipt[1], receipt[2], deliveryHash(string(batch.Begin)), deliveryHash(string(batch.Input))).Scan(&id)
	if err != nil {
		return 0, nil, err
	}
	return store.StatusOK, append(receipt, id), nil
}

type inputDeliveryRecord struct {
	SchemaVersion int    `json:"schema_version"`
	ID            string `json:"delivery_id"`
	Session       string `json:"session_id"`
	Operation     string `json:"operation_uuid"`
	Actor         string `json:"actor"`
	Receiver      string `json:"receiver_tag"`
	Key           string `json:"intent_key"`
	Intent        string `json:"opaque_intent"`
	Begin         string `json:"begin_sequence"`
	Input         string `json:"input_sequence"`
	Turn          string `json:"original_turn"`
	BeginHash     string `json:"begin_sha256"`
	InputHash     string `json:"input_sha256"`
	InputJSON     string `json:"input_event_json"`
	Outcome       string `json:"outcome"`
	Result        string `json:"result_json"`
}

const deliverySelect = `SELECT d.id::text,d.session_id,d.operation_uuid,d.actor,d.receiver_tag,d.intent_key,d.opaque_intent,d.begin_sequence::text,d.input_sequence::text,d.original_turn,d.begin_sha256,d.input_sha256,i.payload,d.outcome,d.result_json,b.payload,b.turn_id,i.turn_id,b.actor,i.actor,s.principal FROM session_input_deliveries d JOIN server_sessions s ON s.id=d.session_id JOIN session_ir_events b ON b.session_id=d.session_id AND b.sequence=d.begin_sequence JOIN session_ir_events i ON i.session_id=d.session_id AND i.sequence=d.input_sequence `

func readInputDelivery(row store.Row) (inputDeliveryRecord, bool, error) {
	var r inputDeliveryRecord
	var begin, beginTurn, inputTurn, beginActor, inputActor, owner string
	err := row.Scan(&r.ID, &r.Session, &r.Operation, &r.Actor, &r.Receiver, &r.Key, &r.Intent, &r.Begin, &r.Input, &r.Turn, &r.BeginHash, &r.InputHash, &r.InputJSON, &r.Outcome, &r.Result, &begin, &beginTurn, &inputTurn, &beginActor, &inputActor, &owner)
	if err != nil {
		return r, false, err
	}
	first, e1 := strconv.ParseUint(r.Begin, 10, 64)
	last, e2 := strconv.ParseUint(r.Input, 10, 64)
	valid := e1 == nil && e2 == nil && first > 0 && first != ^uint64(0) && last == first+1 && r.Turn == beginTurn && r.Turn == inputTurn && r.Actor == owner && r.Actor == beginActor && r.Actor == inputActor && deliveryHash(begin) == r.BeginHash && deliveryHash(r.InputJSON) == r.InputHash && canonical.ValidSessionEvent([]byte(begin)) && canonical.ValidSessionEvent([]byte(r.InputJSON))
	var event struct {
		Kind      string            `json:"kind"`
		MessageID string            `json:"message_id"`
		Payload   canonical.Message `json:"payload"`
	}
	valid = valid && canonical.Decode([]byte(r.InputJSON), &event) && event.Kind == "message" && event.Payload.Role == "user" && len(event.Payload.Blocks) == 1 && event.Payload.Blocks[0].Type == "text" && event.Payload.Blocks[0].InputAttachment != nil && event.Payload.Blocks[0].InputAttachment.OperationUUID == r.Operation
	r.SchemaVersion = 1
	return r, valid, nil
}
func sessionInputDeliveryGet(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	if len(f) != 4 || !deliveryText(f[0], 1024) || !deliveryText(f[1], 1024) || !deliveryText(f[2], 1024) || !deliveryText(f[3], 128) {
		return store.StatusInvalid, nil, nil
	}
	r, valid, err := readInputDelivery(q.QueryRow(ctx, deliverySelect+`WHERE d.session_id=$1 AND d.operation_uuid=$2 AND d.actor=$3 AND d.receiver_tag=$4`, f[0], f[1], f[2], f[3]))
	if store.IsNoRows(err) {
		return store.StatusMissing, nil, nil
	}
	if err != nil {
		return 0, nil, err
	}
	if !valid {
		return store.StatusInvalid, nil, nil
	}
	raw, err := json.Marshal(r)
	if err != nil {
		return 0, nil, err
	}
	if len(raw) > 3*1024*1024 {
		return store.StatusInvalid, nil, nil
	}
	return store.StatusOK, []string{string(raw)}, nil
}
func sessionInputDeliveryPage(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	if len(f) != 5 || !deliveryText(f[0], 128) || !deliveryText(f[1], 1024) {
		return store.StatusInvalid, nil, nil
	}
	after, e1 := strconv.ParseInt(f[2], 10, 64)
	limit, e2 := strconv.Atoi(f[3])
	budget, e3 := strconv.Atoi(f[4])
	if e1 != nil || e2 != nil || e3 != nil || after < 0 || limit < 1 || limit > 32 || budget < 1024 || budget > 4*1024*1024 {
		return store.StatusInvalid, nil, nil
	}
	// IDs only; complete row decoding below rechecks actual owner and event pair.
	rows, err := q.Query(ctx, `SELECT d.session_id,d.operation_uuid FROM session_input_deliveries d JOIN server_sessions s ON s.id=d.session_id WHERE d.receiver_tag=$1 AND d.actor=$2 AND s.principal=d.actor AND d.outcome='pending' AND d.id>$3 ORDER BY d.id LIMIT $4`, f[0], f[1], after, limit+1)
	if err != nil {
		return 0, nil, err
	}
	type key struct{ session, operation string }
	keys := []key{}
	for rows.Next() {
		var k key
		if err = rows.Scan(&k.session, &k.operation); err != nil {
			rows.Close()
			return 0, nil, err
		}
		keys = append(keys, k)
	}
	err = rows.Err()
	rows.Close()
	if err != nil {
		return 0, nil, err
	}
	page := struct {
		SchemaVersion int               `json:"schema_version"`
		Rows          []json.RawMessage `json:"rows"`
		Next          string            `json:"next_cursor"`
		More          bool              `json:"has_more"`
	}{1, []json.RawMessage{}, f[2], false}
	for index, k := range keys {
		if index == limit {
			page.More = true
			break
		}
		status, fields, e := sessionInputDeliveryGet(ctx, q, []string{k.session, k.operation, f[1], f[0]})
		if e != nil {
			return 0, nil, e
		}
		if status != store.StatusOK || len(fields) != 1 {
			return store.StatusInvalid, nil, nil
		}
		var record inputDeliveryRecord
		if !canonical.Decode([]byte(fields[0]), &record) {
			return store.StatusInvalid, nil, nil
		}
		old := page.Next
		page.Rows = append(page.Rows, json.RawMessage(fields[0]))
		page.Next = record.ID
		raw, e := json.Marshal(page)
		if e != nil {
			return 0, nil, e
		}
		if len(raw) > budget {
			page.Rows = page.Rows[:len(page.Rows)-1]
			page.Next = old
			page.More = true
			if len(page.Rows) == 0 {
				return store.StatusInvalid, nil, nil
			}
			break
		}
	}
	raw, err := json.Marshal(page)
	if err != nil {
		return 0, nil, err
	}
	if len(raw) > budget {
		return store.StatusInvalid, nil, nil
	}
	return store.StatusOK, []string{string(raw)}, nil
}
func sessionInputDeliveryAck(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	if len(f) != 6 || !deliveryText(f[1], 128) || !deliveryText(f[2], 1024) || !deliveryDigest(f[3]) || (f[4] != "delivered" && f[4] != "terminal") || len(f[5]) == 0 || len(f[5]) > 32768 || !utf8.ValidString(f[5]) || !canonical.ValidJSON([]byte(f[5])) {
		return store.StatusInvalid, nil, nil
	}
	id, err := strconv.ParseInt(f[0], 10, 64)
	if err != nil || id < 1 {
		return store.StatusInvalid, nil, nil
	}
	var session, owner, actor, receiver, key, state, result string
	// Match begin58's lock order explicitly. A combined joined FOR UPDATE does
	// not state which relation's row lock the executor obtains first.
	err = q.QueryRow(ctx, `SELECT session_id FROM session_input_deliveries WHERE id=$1`, id).Scan(&session)
	if store.IsNoRows(err) {
		return store.StatusMissing, nil, nil
	}
	if err != nil {
		return 0, nil, err
	}
	err = q.QueryRow(ctx, sessionIRLockSQL, session).Scan(&owner)
	if store.IsNoRows(err) {
		return store.StatusMissing, nil, nil
	}
	if err != nil {
		return 0, nil, err
	}
	err = q.QueryRow(ctx, `SELECT actor,receiver_tag,intent_key,outcome,result_json FROM session_input_deliveries WHERE id=$1 AND session_id=$2 FOR UPDATE`, id, session).Scan(&actor, &receiver, &key, &state, &result)
	if store.IsNoRows(err) {
		return store.StatusMissing, nil, nil
	}
	if err != nil {
		return 0, nil, err
	}
	if owner != f[2] || actor != f[2] || receiver != f[1] || key != f[3] {
		return store.StatusInvalid, nil, nil
	}
	if state != "pending" {
		if state != f[4] || result != f[5] {
			return store.StatusInvalid, nil, nil
		}
		return store.StatusOK, []string{"1"}, nil
	}
	_, err = q.Exec(ctx, `UPDATE session_input_deliveries SET outcome=$2,result_json=$3 WHERE id=$1`, id, f[4], f[5])
	if err != nil {
		return 0, nil, err
	}
	return store.StatusOK, []string{"1"}, nil
}

// Only our fixed trigger diagnostic is classified; unrelated database failures
// retain the ordinary failure path. No caller-controlled text is surfaced.
func originalInputDeliveryConflict(err error) bool {
	var refusal *store.StoreError
	return errors.As(err, &refusal) && refusal != nil && refusal.SQLState == "P0001" && refusal.Message == "canonical_input_delivery_conflict"
}
