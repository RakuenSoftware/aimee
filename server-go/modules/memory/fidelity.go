package memory

import (
	"context"
	"encoding/json"
	"errors"

	store "github.com/JBailes/aimee/server-go/db"
)

// Fidelity is read-only retained evidence. Its artifacts are never inputs to
// retrieval-attribution scoring. PostgreSQL owns every connection and query;
// this owner defines the audit interpretation of the retained payload.
func (s *postgresDataStore) fidelityRead(ctx context.Context, turn string) (json.RawMessage, error) {
	response := map[string]any{"status": "ok", "turn_id": turn}
	var payload string
	var count int64
	// One statement observes the report and its attribution count at one snapshot.
	// Keep the existing table and turn key so upgrades can read native-era reports.
	err := s.db.QueryRow(ctx, `SELECT payload::text,
 (SELECT count(*) FROM artifacts WHERE kind='fidelity_attribution' AND turn_id=$1)
 FROM artifacts WHERE kind='fidelity_report' AND turn_id=$1
 ORDER BY created_at DESC,id DESC LIMIT 1`, turn).Scan(&payload, &count)
	if store.IsNoRows(err) {
		response["fidelity_status"] = "not_evaluated"
		response["detail"] = "no fidelity report for this turn"
		return json.Marshal(response)
	}
	if err != nil {
		return nil, err
	}
	var report struct {
		Status      string `json:"status"`
		Supported   int    `json:"supported"`
		Unsupported int    `json:"unsupported"`
		Abstained   int    `json:"abstained"`
	}
	var object map[string]json.RawMessage
	if json.Unmarshal([]byte(payload), &object) != nil || object == nil || json.Unmarshal([]byte(payload), &report) != nil {
		return nil, errors.New("memory: malformed fidelity report")
	}
	if report.Status == "" {
		report.Status = "ok"
	}
	response["fidelity_status"] = report.Status
	response["report"] = map[string]int{"supported": report.Supported, "unsupported": report.Unsupported, "abstained": report.Abstained}
	response["attribution_count"] = count
	return json.Marshal(response)
}
