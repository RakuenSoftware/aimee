package families

// Storage integrity only. Learning owns receipt admission and outcome policy.
import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"strconv"
	"strings"

	canonical "github.com/JBailes/aimee/server-go/ir"
	store "github.com/JBailes/aimee/server-go/modules/aimee"
)

const procedureSnapshotMax = 2 * 1024 * 1024

type localProcedureVersion struct {
	Owner    string `json:"owner_id"`
	ID       string `json:"procedure_id"`
	Revision string `json:"revision"`
}
type localProcedureRow struct {
	Event     json.RawMessage `json:"event"`
	Exposure  json.RawMessage `json:"exposure"`
	ScopeKind string          `json:"scope_kind"`
	ScopeID   string          `json:"scope_id"`
}

func localProcedureIdentity(actor, encoded string) (localProcedureVersion, string, bool) {
	var version localProcedureVersion
	if actor == "" || len(actor) > 600 || strings.ContainsRune(actor, 0) || !canonical.Decode([]byte(encoded), &version) || version.Owner == "" || len(version.Owner) > 128 || version.ID == "" || len(version.ID) > 256 || version.Revision == "" || len(version.Revision) > 256 {
		return version, "", false
	}
	for _, value := range []string{version.Owner, version.ID, version.Revision} {
		if strings.ContainsRune(value, 0) {
			return version, "", false
		}
	}
	raw, err := json.Marshal(version)
	return version, string(raw), err == nil
}

func localProcedureSnapshot(ctx context.Context, q store.Queryer, actor, procedure string) (string, []json.RawMessage, int, error) {
	rows, err := q.Query(ctx, `SELECT event,exposure,scope_kind,scope_id FROM local_procedure_events WHERE principal=$1 AND procedure=$2::jsonb ORDER BY event_id LIMIT 4097`, actor, procedure)
	if err != nil {
		return "", nil, 0, err
	}
	defer rows.Close()
	entries := []localProcedureRow{}
	events := []json.RawMessage{}
	bytes := 0
	for rows.Next() {
		var row localProcedureRow
		if err := rows.Scan(&row.Event, &row.Exposure, &row.ScopeKind, &row.ScopeID); err != nil {
			return "", nil, 0, err
		}
		bytes += len(row.Event) + len(row.Exposure) + len(row.ScopeKind) + len(row.ScopeID)
		if len(entries) >= 4096 || bytes > procedureSnapshotMax-65536 {
			return "", nil, 0, errors.New("local procedure cohort exceeds reply budget")
		}
		entries = append(entries, row)
		events = append(events, row.Event)
	}
	if err := rows.Err(); err != nil {
		return "", nil, 0, err
	}
	raw, err := json.Marshal(entries)
	if err != nil {
		return "", nil, 0, err
	}
	if len(raw) > procedureSnapshotMax-65536 {
		return "", nil, 0, errors.New("local procedure snapshot exceeds reply budget")
	}
	digest := sha256.Sum256(raw)
	return hex.EncodeToString(digest[:]), events, len(raw), nil
}

func procedureEventLoad(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	_, procedure, ok := localProcedureIdentity(f[0], f[1])
	if !ok {
		return store.StatusInvalid, nil, nil
	}
	revision, events, _, err := localProcedureSnapshot(ctx, q, f[0], procedure)
	if err != nil {
		return store.StatusFailed, nil, err
	}
	raw, err := json.Marshal(struct {
		Revision string            `json:"revision"`
		Events   []json.RawMessage `json:"events"`
	}{revision, events})
	if err != nil || len(raw) >= procedureSnapshotMax {
		return store.StatusFailed, nil, err
	}
	return store.StatusOK, []string{string(raw)}, nil
}

func procedureEventAppend(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	version, procedure, ok := localProcedureIdentity(f[0], f[1])
	if !ok || len(f[2]) != 64 || len(f[3]) > procedureSnapshotMax || len(f[4]) > procedureSnapshotMax || len(f[6]) > 1024 || strings.ContainsRune(f[6], 0) || (f[5] != "global" && f[5] != "project" && f[5] != "workspace") || (f[5] == "global" && f[6] != "") {
		return store.StatusInvalid, nil, nil
	}
	var event, exposure map[string]json.RawMessage
	if !canonical.Decode([]byte(f[3]), &event) || !canonical.Decode([]byte(f[4]), &exposure) || event == nil || exposure == nil {
		return store.StatusInvalid, nil, nil
	}
	text := func(obj map[string]json.RawMessage, key string) string {
		var value string
		_ = json.Unmarshal(obj[key], &value)
		return value
	}
	var eventProcedure localProcedureVersion
	var exposureProcedure localProcedureVersion
	var schema int
	if json.Unmarshal(event["schema_version"], &schema) != nil || schema != 1 || !canonical.Decode(event["procedure"], &eventProcedure) || !canonical.Decode(exposure["procedure"], &exposureProcedure) || eventProcedure != version || exposureProcedure != version || text(event, "actor") != f[0] || text(event, "event_id") == "" || len(text(event, "event_id")) > 256 || strings.ContainsRune(text(event, "event_id"), 0) || text(exposure, "producer") != "local_host_ledger" || text(exposure, "request_id") == "" || text(exposure, "receipt_ref") == "" || text(exposure, "receipt_ref") != text(event, "receipt_ref") || text(exposure, "attempt_id") == "" || text(exposure, "attempt_id") != text(event, "attempt_id") {
		return store.StatusInvalid, nil, nil
	}
	// The family descriptor wraps this entire handler in a transaction. The lock
	// serializes writers; the snapshot digest makes projection+append optimistic
	// without putting Learning policy in DB1 or holding a transaction across a bus call.
	if _, err := q.Exec(ctx, `SELECT generation FROM user_memory_erasure_epoch WHERE id=1 FOR SHARE`); err != nil {
		return store.StatusFailed, nil, err
	}
	if _, err := q.Exec(ctx, `SELECT pg_advisory_xact_lock(hashtextextended($1,46))`, strconv.Itoa(len(f[0]))+":"+f[0]+procedure); err != nil {
		return store.StatusFailed, nil, err
	}
	revision, events, snapshotBytes, err := localProcedureSnapshot(ctx, q, f[0], procedure)
	if err != nil {
		return store.StatusFailed, nil, err
	}
	if revision != f[2] {
		return store.StatusOK, []string{"retry"}, nil
	}
	var identical bool
	err = q.QueryRow(ctx, `SELECT procedure=$3::jsonb AND event=$4::jsonb AND exposure=$5::jsonb AND scope_kind=$6 AND scope_id=$7 FROM local_procedure_events WHERE principal=$1 AND event_id=$2`, f[0], text(event, "event_id"), procedure, f[3], f[4], f[5], f[6]).Scan(&identical)
	if err == nil {
		if !identical {
			return store.StatusInvalid, nil, nil
		}
		return store.StatusOK, []string{"duplicate"}, nil
	}
	if !store.IsNoRows(err) {
		return store.StatusFailed, nil, err
	}
	candidate, err := json.Marshal(localProcedureRow{json.RawMessage(f[3]), json.RawMessage(f[4]), f[5], f[6]})
	if err != nil || len(events) >= 4096 || snapshotBytes+len(candidate)+1 > procedureSnapshotMax-65536 {
		return store.StatusInvalid, nil, err
	}

	if _, err = q.Exec(ctx, `INSERT INTO local_procedure_events(principal,event_id,procedure,event,exposure,scope_kind,scope_id) VALUES($1,$2,$3::jsonb,$4::jsonb,$5::jsonb,$6,$7)`, f[0], text(event, "event_id"), procedure, f[3], f[4], f[5], f[6]); err != nil {
		return store.StatusFailed, nil, err
	}
	return store.StatusOK, []string{"inserted"}, nil
}
