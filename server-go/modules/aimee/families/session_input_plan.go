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

const (
	opSessionInputProbe        uint32 = 62
	opSessionInputPlanReserve  uint32 = 63
	opSessionInputPlannedBegin uint32 = 64
	opSessionInputPlanTerminal uint32 = 65
)

type inputPlanRecord struct {
	SchemaVersion  int    `json:"schema_version"`
	ID             string `json:"plan_id"`
	Session        string `json:"session_id"`
	Operation      string `json:"operation_uuid"`
	Actor          string `json:"actor"`
	Receiver       string `json:"receiver_tag"`
	TextHash       string `json:"safe_text_sha256"`
	Hash           string `json:"plan_sha256"`
	Opaque         string `json:"opaque_plan"`
	State          string `json:"state"`
	Begin          string `json:"begin_sequence"`
	Input          string `json:"input_sequence"`
	Turn           string `json:"original_turn"`
	Delivery       string `json:"delivery_id"`
	TerminalResult string `json:"terminal_result"`
}

const inputPlanSelect = `SELECT id::text,session_id,operation_uuid,actor,receiver_tag,safe_text_sha256,plan_sha256,opaque_plan,state,coalesce(begin_sequence::text,''),coalesce(input_sequence::text,''),coalesce(original_turn,''),coalesce(delivery_id::text,''),terminal_result FROM session_input_plans WHERE session_id=$1 AND operation_uuid=$2`

func readInputPlan(ctx context.Context, q store.Queryer, session, operation string, lock bool) (*inputPlanRecord, error) {
	sql := inputPlanSelect
	if lock {
		sql += " FOR UPDATE"
	}
	var p inputPlanRecord
	err := q.QueryRow(ctx, sql, session, operation).Scan(&p.ID, &p.Session, &p.Operation, &p.Actor, &p.Receiver, &p.TextHash, &p.Hash, &p.Opaque, &p.State, &p.Begin, &p.Input, &p.Turn, &p.Delivery, &p.TerminalResult)
	if store.IsNoRows(err) {
		return nil, nil
	}
	if err != nil {
		return nil, err
	}
	p.SchemaVersion = 1
	return &p, nil
}
func inputPlanOwner(ctx context.Context, q store.Queryer, session, operation, actor string) (uint32, error) {
	if !deliveryText(session, 1024) || !deliveryText(actor, 1024) || !canonical.ValidInputOperationUUID(operation) {
		return store.StatusInvalid, nil
	}
	var owner string
	err := q.QueryRow(ctx, sessionIRLockSQL, session).Scan(&owner)
	if store.IsNoRows(err) {
		return store.StatusMissing, nil
	}
	if err != nil {
		return 0, err
	}
	if owner != actor {
		return store.StatusInvalid, nil
	}
	return store.StatusOK, nil
}

type inputPlanCanonical struct {
	Begin string `json:"begin_sequence"`
	Input string `json:"input_sequence"`
	Turn  string `json:"original_turn"`
}

// Exact two deterministic row keys, not a history/read grant. Bodies are only
// checked internally; the probe never returns text or opaque input attachment.
func inputPlanCanonicalReceipt(ctx context.Context, q store.Queryer, session, operation, actor string) (*inputPlanCanonical, bool, error) {
	type event struct {
		seq                  int64
		turn, actor, payload string
		present              bool
	}
	beginKey := "host-original-input-batch-v1:" + operation + ":begin"
	inputKey := "host-original-input-batch-v1:" + operation + ":input"
	// Both exact deterministic keys are read under the existing caller-owned
	// session lock/transaction. No history scan, disclosure or write is added.
	rows, err := q.Query(ctx, `SELECT event_id,sequence,turn_id,actor,payload
 FROM session_ir_events WHERE session_id=$1 AND event_id IN ($2,$3)`, session, beginKey, inputKey)
	if err != nil {
		return nil, false, err
	}
	defer rows.Close()
	var a, b event
	for rows.Next() {
		var key string
		var value event
		if err = rows.Scan(&key, &value.seq, &value.turn, &value.actor, &value.payload); err != nil {
			return nil, false, err
		}
		value.present = true
		switch key {
		case beginKey:
			if a.present {
				return nil, false, nil
			}
			a = value
		case inputKey:
			if b.present {
				return nil, false, nil
			}
			b = value
		default:
			return nil, false, nil
		}
	}
	if err = rows.Err(); err != nil {
		return nil, false, err
	}
	if !a.present && !b.present {
		return nil, true, nil
	}
	if !a.present || !b.present || a.seq <= 0 || a.seq == int64(^uint64(0)>>1) || b.seq != a.seq+1 || a.actor != actor || b.actor != actor || a.turn != b.turn || !canonical.ValidSessionEvent([]byte(a.payload)) || !canonical.ValidSessionEvent([]byte(b.payload)) {
		return nil, false, nil
	}
	var begin struct {
		Kind    string `json:"kind"`
		Payload struct {
			Status string `json:"status"`
		} `json:"payload"`
	}
	var input struct {
		Kind      string            `json:"kind"`
		MessageID string            `json:"message_id"`
		Payload   canonical.Message `json:"payload"`
	}
	if !canonical.Decode([]byte(a.payload), &begin) || begin.Kind != "execution" || begin.Payload.Status != "running" || !canonical.Decode([]byte(b.payload), &input) || input.Kind != "message" || input.Payload.Role != "user" || len(input.Payload.Blocks) != 1 || input.Payload.Blocks[0].InputAttachment == nil || input.Payload.Blocks[0].InputAttachment.OperationUUID != operation {
		return nil, false, nil
	}
	return &inputPlanCanonical{strconv.FormatInt(a.seq, 10), strconv.FormatInt(b.seq, 10), a.turn}, true, nil
}
func inputPlanDelivery(ctx context.Context, q store.Queryer, session, operation, actor string) (string, error) {
	var id, owner string
	err := q.QueryRow(ctx, `SELECT id::text,actor FROM session_input_deliveries WHERE session_id=$1 AND operation_uuid=$2`, session, operation).Scan(&id, &owner)
	if store.IsNoRows(err) {
		return "", nil
	}
	if err != nil {
		return "", err
	}
	if owner != actor {
		return "", nil
	}
	return id, nil
}
func sessionInputProbe(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	if len(f) != 3 {
		return store.StatusInvalid, nil, nil
	}
	status, err := inputPlanOwner(ctx, q, f[0], f[1], f[2])
	if err != nil || status != store.StatusOK {
		return status, nil, err
	}
	canonicalReceipt, valid, err := inputPlanCanonicalReceipt(ctx, q, f[0], f[1], f[2])
	if err != nil {
		return 0, nil, err
	}
	if !valid {
		return store.StatusConflict, nil, nil
	}
	plan, err := readInputPlan(ctx, q, f[0], f[1], false)
	if err != nil {
		return 0, nil, err
	}
	delivery, err := inputPlanDelivery(ctx, q, f[0], f[1], f[2])
	if err != nil {
		return 0, nil, err
	}
	presence := "owned_absent"
	if canonicalReceipt != nil {
		presence = "legacy"
		if delivery != "" {
			presence = "delivery"
		}
	}
	if plan != nil {
		if plan.Actor != f[2] || ((plan.State == "reserved" || plan.State == "terminal") && canonicalReceipt != nil) || (plan.State == "committed" && (canonicalReceipt == nil || plan.Begin != canonicalReceipt.Begin || plan.Input != canonicalReceipt.Input || plan.Turn != canonicalReceipt.Turn || plan.Delivery != delivery)) {
			return store.StatusConflict, nil, nil
		}
		if plan.State == "reserved" {
			presence = "reserved"
		} else if plan.State == "terminal" {
			presence = "terminal"
		} else if plan.State == "committed" {
			presence = "committed"
		} else {
			return store.StatusConflict, nil, nil
		}
	}
	reply := struct {
		SchemaVersion int                 `json:"schema_version"`
		Presence      string              `json:"presence"`
		Session       string              `json:"session_id"`
		Operation     string              `json:"operation_uuid"`
		Actor         string              `json:"actor"`
		Canonical     *inputPlanCanonical `json:"canonical"`
		Delivery      string              `json:"delivery_id"`
		Plan          *inputPlanRecord    `json:"plan"`
	}{1, presence, f[0], f[1], f[2], canonicalReceipt, delivery, plan}
	raw, err := json.Marshal(reply)
	if err != nil {
		return 0, nil, err
	}
	return store.StatusOK, []string{string(raw)}, nil
}
func sessionInputPlanReserve(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	if len(f) != 7 || !deliveryText(f[3], 128) || !deliveryDigest(f[4]) || !deliveryDigest(f[5]) || !utf8.ValidString(f[6]) || len(f[6]) == 0 || len(f[6]) > 16384 || strings.ContainsRune(f[6], 0) || deliveryHash(f[6]) != f[5] {
		return store.StatusInvalid, nil, nil
	}
	status, err := inputPlanOwner(ctx, q, f[0], f[1], f[2])
	if err != nil || status != store.StatusOK {
		return status, nil, err
	}
	canonicalReceipt, valid, err := inputPlanCanonicalReceipt(ctx, q, f[0], f[1], f[2])
	if err != nil {
		return 0, nil, err
	}
	if !valid || canonicalReceipt != nil {
		return store.StatusConflict, nil, nil
	}
	plan, err := readInputPlan(ctx, q, f[0], f[1], true)
	if err != nil {
		return 0, nil, err
	}
	if plan != nil {
		if plan.Actor != f[2] || plan.Receiver != f[3] || plan.TextHash != f[4] || plan.Hash != f[5] || plan.Opaque != f[6] || plan.State != "reserved" {
			return store.StatusConflict, nil, nil
		}
	} else {
		var id string
		err = q.QueryRow(ctx, `INSERT INTO session_input_plans(session_id,operation_uuid,actor,receiver_tag,safe_text_sha256,plan_sha256,opaque_plan) VALUES($1,$2,$3,$4,$5,$6,$7) RETURNING id::text`, f[0], f[1], f[2], f[3], f[4], f[5], f[6]).Scan(&id)
		if err != nil {
			return 0, nil, err
		}
		plan = &inputPlanRecord{SchemaVersion: 1, ID: id, Session: f[0], Operation: f[1], Actor: f[2], Receiver: f[3], TextHash: f[4], Hash: f[5], Opaque: f[6], State: "reserved"}
	}
	raw, err := json.Marshal(plan)
	if err != nil {
		return 0, nil, err
	}
	return store.StatusOK, []string{string(raw)}, nil
}
func sessionInputPlannedBegin(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	if len(f) != 8 || !deliveryDigest(f[7]) {
		return store.StatusInvalid, nil, nil
	}
	id, err := strconv.ParseInt(f[6], 10, 64)
	if err != nil || id < 1 {
		return store.StatusInvalid, nil, nil
	}
	var delivery inputDeliveryIntent
	if len(f[5]) > 32768 || !canonical.Decode([]byte(f[5]), &delivery) || delivery.SchemaVersion != 1 {
		return store.StatusInvalid, nil, nil
	}
	var batch struct {
		SchemaVersion int             `json:"schema_version"`
		Begin         json.RawMessage `json:"begin"`
		Input         json.RawMessage `json:"input"`
	}
	var input struct {
		Kind      string            `json:"kind"`
		MessageID string            `json:"message_id"`
		Payload   canonical.Message `json:"payload"`
	}
	if len(f[4]) > 1048576 || !canonical.Decode([]byte(f[4]), &batch) || batch.SchemaVersion != 1 || !canonical.ValidSessionEvent(batch.Input) || !canonical.Decode(batch.Input, &input) || len(input.Payload.Blocks) != 1 || input.Payload.Blocks[0].InputAttachment == nil {
		return store.StatusInvalid, nil, nil
	}
	status, err := inputPlanOwner(ctx, q, f[0], f[1], f[3])
	if err != nil || status != store.StatusOK {
		return status, nil, err
	}
	plan, err := readInputPlan(ctx, q, f[0], f[1], true)
	if err != nil {
		return 0, nil, err
	}
	if plan == nil || (plan.State != "reserved" && plan.State != "committed") || plan.ID != f[6] || plan.Actor != f[3] || plan.Receiver != delivery.Receiver || plan.Hash != f[7] || plan.TextHash != input.Payload.Blocks[0].InputAttachment.TextSHA256 {
		return store.StatusConflict, nil, nil
	}
	status, cells, err := sessionInputDeliveryBegin(ctx, q, f[:6])
	if err != nil || status != store.StatusOK {
		return status, nil, err
	}
	if len(cells) != 6 {
		return store.StatusInvalid, nil, nil
	}
	if plan.State == "committed" {
		if cells[4] != "replay" || cells[0] != plan.Begin || cells[1] != plan.Input || cells[2] != plan.Turn || cells[5] != plan.Delivery {
			return store.StatusConflict, nil, nil
		}
	} else if plan.State == "reserved" {
		if cells[4] != "created" {
			return store.StatusConflict, nil, nil
		}
		_, err = q.Exec(ctx, `UPDATE session_input_plans SET state='committed',begin_sequence=$3::text::bigint,input_sequence=$4::text::bigint,original_turn=$5,delivery_id=$6::text::bigint WHERE id=$1 AND session_id=$2`, id, f[0], cells[0], cells[1], cells[2], cells[5])
		if err != nil {
			return 0, nil, err
		}
	} else {
		return store.StatusConflict, nil, nil
	}
	return store.StatusOK, cells, nil
}

// The trusted fixed owner callback verifies durable target closure. This generic
// operation stores an opaque documentary result, never treats a body boolean as
// target authority, and never concludes an unknown or committed delivery.
func sessionInputPlanTerminal(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	if len(f) != 7 || !deliveryText(f[4], 128) || !deliveryDigest(f[5]) || len(f[6]) == 0 || len(f[6]) > 32768 || !canonical.ValidJSON([]byte(f[6])) {
		return store.StatusInvalid, nil, nil
	}
	id, err := strconv.ParseInt(f[0], 10, 64)
	if err != nil || id < 1 {
		return store.StatusInvalid, nil, nil
	}
	status, err := inputPlanOwner(ctx, q, f[1], f[2], f[3])
	if err != nil || status != store.StatusOK {
		return status, nil, err
	}
	plan, err := readInputPlan(ctx, q, f[1], f[2], true)
	if err != nil {
		return 0, nil, err
	}
	if plan == nil || plan.ID != f[0] || plan.Actor != f[3] || plan.Receiver != f[4] || plan.Hash != f[5] {
		return store.StatusConflict, nil, nil
	}
	receipt, valid, err := inputPlanCanonicalReceipt(ctx, q, f[1], f[2], f[3])
	if err != nil {
		return 0, nil, err
	}
	if !valid || receipt != nil {
		return store.StatusConflict, nil, nil
	}
	delivery, err := inputPlanDelivery(ctx, q, f[1], f[2], f[3])
	if err != nil {
		return 0, nil, err
	}
	if delivery != "" {
		return store.StatusConflict, nil, nil
	}
	if plan.State == "terminal" {
		if plan.TerminalResult != f[6] {
			return store.StatusConflict, nil, nil
		}
		return store.StatusOK, []string{"1"}, nil
	}
	if plan.State != "reserved" {
		return store.StatusConflict, nil, nil
	}
	_, err = q.Exec(ctx, `UPDATE session_input_plans SET state='terminal',terminal_result=$2 WHERE id=$1`, id, f[6])
	if err != nil {
		return 0, nil, err
	}
	return store.StatusOK, []string{"1"}, nil
}
