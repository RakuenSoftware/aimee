package memory

import (
	"context"
	"errors"
	"math"
	"strings"

	store "github.com/JBailes/aimee/server-go/db"
)

func (s *postgresDataStore) upsertWorkflow(ctx context.Context, request DataRequest) (Record, error) {
	if _, ok := s.db.(store.Tx); !ok {
		return Record{}, errors.New("memory: workflow update requires a transaction")
	}
	key := strings.ToLower("workflow:" + request.Workspace + ":" + request.SignalType)
	// Serialise the lookup and canonical write even when no row exists yet.
	if _, err := s.db.Exec(ctx, `SELECT pg_advisory_xact_lock(hashtextextended($1,5746))`, request.Workspace+"\x1f"+key); err != nil {
		return Record{}, err
	}
	confidence := 0.6
	if request.Confidence != nil && *request.Confidence > 0 {
		confidence = *request.Confidence
	}
	var previous float64
	var existingKey string
	err := s.db.QueryRow(ctx, `SELECT key,confidence FROM memories WHERE scope_type='workspace' AND scope_value=$1
 AND lower(key)=$2 AND kind='workflow' AND lifecycle_state='active' ORDER BY id LIMIT 1 FOR UPDATE`, request.Workspace, key).Scan(&existingKey, &previous)
	if err != nil && !store.IsNoRows(err) {
		return Record{}, err
	}
	if err == nil {
		key = existingKey
		confidence = math.Max(confidence, previous+0.2)
	}
	if request.SessionID == "" {
		request.SessionID = "workflow_learning"
	}
	request.Scope, request.Tier, request.Kind, request.Key, request.Content = Scope{Type: ScopeWorkspace, Value: request.Workspace}, "L1", "workflow", key, request.Rule
	request.Authority, request.Confidence = AuthorityModel, &confidence
	record, err := s.InsertEpistemic(ctx, request)
	if err != nil {
		return Record{}, err
	}
	if _, err = s.ScopeTag(ctx, record.ID, request.Scope); err != nil {
		return Record{}, err
	}
	if err = s.captureStoredFactActor(ctx, record.ID, AuthorityModel, nil); err != nil {
		return Record{}, err
	}
	return record, nil
}
