package memory

import (
	"context"
	"encoding/json"
	"errors"
	"strconv"
	"strings"

	store "github.com/JBailes/aimee/server-go/db"
	"github.com/JBailes/aimee/server-go/modules/egress"
)

func (s *postgresDataStore) embedUnit(ctx context.Context, trace uint64, executor egress.Executor, pointID int64, command string, dimension int) EmbedResponse {
	if db, ok := s.db.(store.DB); ok {
		tx, err := db.Begin(ctx)
		if err != nil {
			return EmbedResponse{Error: "embed: unit transaction unavailable"}
		}
		defer tx.Rollback(context.Background())
		bound := *s
		bound.db = tx
		response := bound.embedUnit(ctx, trace, executor, pointID, command, dimension)
		if err = tx.Commit(ctx); err != nil {
			return EmbedResponse{Error: "embed: unit transaction failed"}
		}
		return response
	}
	if _, ok := s.db.(store.Tx); !ok {
		return EmbedResponse{Error: "embed: unit transaction required"}
	}

	if err := s.activeEmbeddingGuard(ctx, command, dimension); err != nil {
		return EmbedResponse{Error: err.Error()}
	}
	var u derivedUnit
	var parent Record
	// Acquire the parent before its unit, matching the reindex lock order.
	// The second query gets a fresh snapshot after any concurrent rebuild.
	if err := s.db.QueryRow(ctx, `SELECT m.id FROM memory_units u JOIN memories m ON m.id=u.memory_id
 WHERE u.id=$1 AND m.lifecycle_state='active' FOR UPDATE OF m`, pointID-unitPointOffset).Scan(&parent.ID); err != nil {
		return EmbedResponse{Error: "embed: memory unit unavailable"}
	}
	if err := s.db.QueryRow(ctx, `SELECT u.id,u.unit_type,u.unit_key,u.unit_text,u.memory_kind,u.weight,
 m.id,m.kind,m.scope_type,m.scope_value FROM memory_units u JOIN memories m ON m.id=u.memory_id
 WHERE u.id=$1 AND m.lifecycle_state='active' FOR UPDATE OF u`, pointID-unitPointOffset).Scan(
		&u.ID, &u.Type, &u.Key, &u.Text, &u.Kind, &u.Weight, &parent.ID, &parent.Kind, &parent.Scope.Type, &parent.Scope.Value); err != nil {
		return EmbedResponse{Error: "embed: memory unit unavailable"}
	}
	response := s.embedActiveVersion(ctx, trace, executor, EmbedRequest{BaseURL: command, InputType: "document", Text: unitEmbeddingText(u), MaxDim: dimension})
	failed := func(response EmbedResponse) EmbedResponse {
		detail := response.Error
		if detail == "" {
			detail = "unit embedding unavailable"
		}
		_ = s.MarkEmbeddingFailure(ctx, pointID, detail)
		return response
	}
	if response.Error != "" || response.Unavailable || response.Unauthorized || response.Truncated {
		return failed(response)
	}
	if err := s.checkActiveEmbeddingIdentity(ctx, response.ServingID); err != nil {
		return failed(EmbedResponse{Error: err.Error()})
	}
	if err := s.withEmbeddingWrite(ctx, func(bound *postgresDataStore) error {
		return bound.upsertUnitEmbedding(ctx, u, parent, response.Vector)
	}); err != nil {
		return failed(EmbedResponse{Error: "embed: unit vector upsert failed: " + err.Error()})
	}
	response.Embedded = true
	return response
}

func (s *postgresDataStore) upsertUnitEmbedding(ctx context.Context, u derivedUnit, parent Record, vector []float32) error {
	if len(vector) == 0 || u.ID <= 0 || u.ID >= unitPointOffset || parent.ID <= 0 {
		return errors.New("memory: invalid unit embedding")
	}
	components := make([]string, len(vector))
	for i, v := range vector {
		components[i] = strconv.FormatFloat(float64(v), 'g', -1, 32)
	}
	workspace, project := "", ""
	if parent.Scope.Type == ScopeWorkspace {
		workspace = parent.Scope.Value
	}
	if parent.Scope.Type == ScopeProject {
		project = parent.Scope.Value
	}
	payload, err := json.Marshal(map[string]any{"record_type": "unit", "memory_id": parent.ID, "unit_id": u.ID, "unit_type": u.Type, "unit_key": u.Key, "memory_kind": u.Kind, "weight": u.Weight, "kind": parent.Kind, "primary_scope": parent.Scope.Type, "workspace": workspace, "project": project})
	if err != nil {
		return err
	}
	point := unitPointOffset + u.ID
	if _, err = s.db.Exec(ctx, `INSERT INTO memory_embeddings(point_id,embedding,record_type,primary_scope,workspace,project,kind,payload_json)
 VALUES($1,$2::vector,'unit',$3,$4,$5,$6,$7) ON CONFLICT(point_id) DO UPDATE SET
 embedding=EXCLUDED.embedding,record_type=EXCLUDED.record_type,primary_scope=EXCLUDED.primary_scope,
 workspace=EXCLUDED.workspace,project=EXCLUDED.project,kind=EXCLUDED.kind,payload_json=EXCLUDED.payload_json`, point, "["+strings.Join(components, ",")+"]", parent.Scope.Type, workspace, project, parent.Kind, string(payload)); err != nil {
		return err
	}
	if err = s.retainActiveEmbedding(ctx, point, vector); err != nil {
		return err
	}
	_, err = s.db.Exec(ctx, `INSERT INTO vector_index_ops(point_id,collection,memory_id,status,attempts,last_error,indexed_at,updated_at)
 VALUES($1,'memory',$2,'ok',0,'',pg_now_text(),pg_now_text()) ON CONFLICT(point_id) DO UPDATE SET
 status='ok',last_error='',indexed_at=pg_now_text(),updated_at=pg_now_text()`, point, parent.ID)
	return err
}
