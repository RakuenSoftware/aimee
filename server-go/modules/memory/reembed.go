package memory

import (
	"context"
	"crypto/sha256"
	"encoding/json"
	"errors"
	"fmt"
	"strings"

	store "github.com/JBailes/aimee/server-go/db"
	"github.com/JBailes/aimee/server-go/modules/egress"
)

// Hash every field that affects embedding input or retrieval payload. Confidence,
// usage counters and timestamps do not invalidate an unchanged vector.
const embeddingInputs = `WITH sources AS (
 SELECT m.id AS point_id,m.id AS memory_id,'memory'::text AS record_type,m.key AS input_key,
 m.content AS input_content,m.scope_type,m.scope_value,m.kind,''::text AS unit_type,
 ''::text AS unit_kind,0::double precision AS unit_weight,m.content AS source_content
 FROM memories m WHERE m.lifecycle_state='active' AND m.activation_suppressed=0
 UNION ALL
 SELECT 1000000000000+u.id,m.id,'unit',u.unit_key,u.unit_text,m.scope_type,m.scope_value,m.kind,
 u.unit_type,u.memory_kind,u.weight,m.content FROM memory_units u JOIN memories m ON m.id=u.memory_id
 WHERE m.lifecycle_state='active' AND m.activation_suppressed=0
), inputs AS (SELECT s.*,encode(sha256(convert_to(to_jsonb(s)::text,'UTF8')),'hex') AS input_hash FROM sources s) `

type reembedStatus struct {
	ActiveVersion string      `json:"active_version"`
	HasJob        bool        `json:"has_job"`
	Job           *reembedJob `json:"job,omitempty"`
}
type reembedJob struct {
	TargetVersion   string `json:"target_version"`
	LastID          int64  `json:"last_id"`
	Total           int64  `json:"total"`
	Done            int64  `json:"done"`
	StartedAt       string `json:"started_at"`
	FinishedAt      string `json:"finished_at"`
	Ready           bool   `json:"ready"`
	PendingMetadata int64  `json:"pending_metadata"`
}

type embeddingInput struct {
	PointID, MemoryID                                                         int64
	RecordType, Key, Content, ScopeType, ScopeValue, Kind, UnitType, UnitKind string
	Weight                                                                    float64
	Hash                                                                      string
}

func (in embeddingInput) text() string {
	if in.RecordType == "unit" {
		return unitEmbeddingText(derivedUnit{Type: in.UnitType, Key: in.Key, Text: in.Content, Weight: in.Weight})
	}
	if in.Key != "" {
		return in.Key + "\n" + in.Content
	}
	return in.Content
}

func (s *postgresDataStore) reembedCounts(ctx context.Context, version string) (total, done int64, err error) {
	err = s.db.QueryRow(ctx, embeddingInputs+`SELECT count(*),count(v.point_id) FILTER(WHERE v.input_hash=i.input_hash AND v.embedding IS NOT NULL)
 FROM inputs i LEFT JOIN memory_embedding_versions v ON v.point_id=i.point_id AND v.version=$1`, version).Scan(&total, &done)
	return
}
func (s *postgresDataStore) reembedStatus(ctx context.Context) (reembedStatus, error) {
	result := reembedStatus{}
	err := s.db.QueryRow(ctx, `SELECT COALESCE((SELECT version FROM memory_active_embedder WHERE id=1),'')`).Scan(&result.ActiveVersion)
	if err != nil {
		return result, err
	}
	job := &reembedJob{}
	err = s.db.QueryRow(ctx, `SELECT target_version,last_id,started_at,COALESCE(finished_at,'') FROM memory_reembed_progress WHERE id=1`).Scan(&job.TargetVersion, &job.LastID, &job.StartedAt, &job.FinishedAt)
	if store.IsNoRows(err) {
		return result, nil
	}
	if err != nil {
		return result, err
	}
	job.Total, job.Done, err = s.reembedCounts(ctx, job.TargetVersion)
	if err == nil {
		job.PendingMetadata, err = s.pendingEmbeddingMetadata(ctx)
	}
	var known bool
	if err == nil {
		err = s.db.QueryRow(ctx, `SELECT EXISTS(SELECT 1 FROM memory_embedder_versions WHERE version=$1)`, job.TargetVersion).Scan(&known)
	}
	job.Ready = known && job.Total == job.Done && job.PendingMetadata == 0
	result.HasJob, result.Job = true, job
	return result, err
}
func (s *postgresDataStore) requireAllVectorScopes(ctx context.Context) error {
	if err := s.requireKBDomain(); err != nil {
		return err
	}
	var all bool
	if err := s.db.QueryRow(ctx, `SELECT COALESCE(current_setting('aimee.memory_scope_all',true),'')='1'`).Scan(&all); err != nil {
		return err
	}
	if !all {
		return errors.New("memory: versioned embedding maintenance requires all scopes")
	}
	return nil
}
func (s *postgresDataStore) prepareReembed(ctx context.Context, version, command string) error {
	if version == "" || len(version) > 256 {
		return errors.New("memory: embedder version is required")
	}
	return s.vectorTransaction(ctx, func(bound *postgresDataStore) error {
		if err := bound.requireAllVectorScopes(ctx); err != nil {
			return err
		}
		dim, err := bound.vectorDimension(ctx)
		if err != nil {
			return err
		}
		if _, err = bound.db.Exec(ctx, `INSERT INTO memory_embedder_versions(version,command,dimension) VALUES($1,$2,$3) ON CONFLICT(version) DO NOTHING`, version, command, dim); err != nil {
			return err
		}
		var oldCommand string
		var oldDim int
		if err = bound.db.QueryRow(ctx, `SELECT command,dimension FROM memory_embedder_versions WHERE version=$1`, version).Scan(&oldCommand, &oldDim); err != nil {
			return err
		}
		if oldCommand != command || oldDim != dim {
			return errors.New("memory: version already identifies a different embedder or dimension")
		}
		total, done, err := bound.reembedCounts(ctx, version)
		if err != nil {
			return err
		}
		_, err = bound.db.Exec(ctx, `INSERT INTO memory_reembed_progress(id,target_version,total,done,last_id,started_at) VALUES(1,$1,$2,$3,0,pg_now_text())
 ON CONFLICT(id) DO UPDATE SET target_version=EXCLUDED.target_version,total=EXCLUDED.total,done=EXCLUDED.done,
 last_id=CASE WHEN memory_reembed_progress.target_version=EXCLUDED.target_version THEN memory_reembed_progress.last_id ELSE 0 END,
 started_at=CASE WHEN memory_reembed_progress.target_version=EXCLUDED.target_version THEN memory_reembed_progress.started_at ELSE EXCLUDED.started_at END,finished_at=NULL`, version, total, done)
		return err
	})
}
func (s *postgresDataStore) reembedNext(ctx context.Context, version string, after int64, limit int) ([]int64, error) {
	if err := s.requireAllVectorScopes(ctx); err != nil {
		return nil, err
	}
	rows, err := s.db.Query(ctx, embeddingInputs+`SELECT i.point_id FROM inputs i LEFT JOIN memory_embedding_versions v ON v.version=$1 AND v.point_id=i.point_id
 WHERE i.point_id>$2 AND (v.point_id IS NULL OR v.input_hash<>i.input_hash OR v.embedding IS NULL) ORDER BY i.point_id LIMIT $3`, version, after, limit)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	ids := []int64{}
	for rows.Next() {
		var id int64
		if err = rows.Scan(&id); err != nil {
			return nil, err
		}
		ids = append(ids, id)
	}
	return ids, rows.Err()
}
func (s *postgresDataStore) embeddingInput(ctx context.Context, point int64) (embeddingInput, error) {
	var in embeddingInput
	source := strings.Replace(embeddingInputs, "FROM memories m WHERE m.lifecycle_state='active'", "FROM memories m WHERE m.id=$1 AND m.lifecycle_state='active'", 1)
	source = strings.Replace(source, "WHERE m.lifecycle_state='active' AND m.activation_suppressed=0\n)", "WHERE u.id=$1-1000000000000 AND m.lifecycle_state='active' AND m.activation_suppressed=0\n)", 1)
	err := s.db.QueryRow(ctx, source+`SELECT point_id,memory_id,record_type,input_key,input_content,scope_type,scope_value,kind,unit_type,unit_kind,unit_weight,input_hash FROM inputs WHERE point_id=$1`, point).Scan(&in.PointID, &in.MemoryID, &in.RecordType, &in.Key, &in.Content, &in.ScopeType, &in.ScopeValue, &in.Kind, &in.UnitType, &in.UnitKind, &in.Weight, &in.Hash)
	return in, err
}
func (s *postgresDataStore) stageEmbedding(ctx context.Context, version string, in embeddingInput, vector []float32, detail string) error {
	var literal any
	if len(vector) > 0 {
		var err error
		literal, err = vectorLiteral(vector)
		if err != nil {
			return err
		}
	}
	_, err := s.db.Exec(ctx, `INSERT INTO memory_embedding_versions(version,point_id,memory_id,input_hash,embedding,attempts,last_error)
 VALUES($1,$2,$3,$4,$5::vector,1,$6) ON CONFLICT(version,point_id) DO UPDATE SET memory_id=EXCLUDED.memory_id,
 input_hash=EXCLUDED.input_hash,embedding=EXCLUDED.embedding,attempts=CASE WHEN memory_embedding_versions.input_hash=EXCLUDED.input_hash THEN memory_embedding_versions.attempts+1 ELSE 1 END,
 last_error=EXCLUDED.last_error,updated_at=pg_now_text()`, version, in.PointID, in.MemoryID, in.Hash, literal, detail)
	return err
}
func (s *postgresDataStore) reembedPoint(ctx context.Context, trace uint64, executor egress.Executor, version string, point int64) (EmbedResponse, error) {
	response := EmbedResponse{}
	err := s.vectorTransaction(ctx, func(bound *postgresDataStore) error {
		if err := bound.requireAllVectorScopes(ctx); err != nil {
			return err
		}
		var target, command string
		var dim int
		if err := bound.db.QueryRow(ctx, `SELECT p.target_version,v.command,v.dimension FROM memory_reembed_progress p JOIN memory_embedder_versions v ON v.version=p.target_version WHERE p.id=1 FOR UPDATE OF p`).Scan(&target, &command, &dim); err != nil {
			return err
		}
		if target != version {
			return errors.New("memory: re-embedding job was replaced")
		}
		parent := point
		if point >= unitPointOffset {
			if err := bound.db.QueryRow(ctx, `SELECT memory_id FROM memory_units WHERE id=$1`, point-unitPointOffset).Scan(&parent); err != nil {
				if store.IsNoRows(err) {
					return nil
				}
				return err
			}
		}
		var locked int64
		if err := bound.db.QueryRow(ctx, `SELECT id FROM memories WHERE id=$1 AND lifecycle_state='active' FOR UPDATE`, parent).Scan(&locked); err != nil {
			if store.IsNoRows(err) {
				return nil
			}
			return err
		}
		in, err := bound.embeddingInput(ctx, point)
		if store.IsNoRows(err) {
			return nil
		}
		if err != nil {
			return err
		}
		var exists bool
		if err = bound.db.QueryRow(ctx, `SELECT EXISTS(SELECT 1 FROM memory_embedding_versions WHERE version=$1 AND point_id=$2 AND input_hash=$3 AND embedding IS NOT NULL)`, version, point, in.Hash).Scan(&exists); err != nil {
			return err
		}
		if exists {
			response.Embedded = true
			return nil
		}
		response = bound.embedForVersion(ctx, trace, executor, version, EmbedRequest{BaseURL: command, InputType: "document", Text: in.text(), MaxDim: dim})
		if ctx.Err() != nil {
			return ctx.Err()
		}
		if response.Error == "" && !response.Unavailable && !response.Unauthorized && !response.Truncated && len(response.Vector) == dim {
			if err := bound.checkEmbeddingIdentity(ctx, version, response.ServingID); err != nil {
				response.Error = err.Error()
			}
		}
		detail := response.Error
		vector := response.Vector
		if response.Unavailable || response.Unauthorized || response.Truncated || len(vector) != dim || detail != "" {
			vector = nil
			if detail == "" {
				detail = "embedder unavailable, refused or returned an incompatible vector"
			}
			response.Error = detail
		}
		if err = bound.stageEmbedding(ctx, version, in, vector, detail); err != nil {
			return err
		}
		// This is a durable progress hint. Status and cutover recount current hashes;
		// hashing the entire corpus after every vector would make a run quadratic.
		if _, err = bound.db.Exec(ctx, `UPDATE memory_reembed_progress SET last_id=GREATEST(last_id,$1),done=done+$2 WHERE id=1`, point, map[bool]int{true: 1, false: 0}[len(vector) > 0]); err != nil {
			return err
		}

		response.Vector = nil
		response.Embedded = len(vector) > 0
		return nil
	})
	return response, err
}

// Validate every current input under canonical-table locks. New, edited and
// regenerated units cannot race the check. An incomplete draft never changes
// either the active pointer or its vectors; unrelated semantic vectors survive.
func (s *postgresDataStore) cutoverReembed(ctx context.Context, version string) (int64, error) {
	var count int64
	err := s.vectorTransaction(ctx, func(bound *postgresDataStore) error {
		if err := bound.requireAllVectorScopes(ctx); err != nil {
			return err
		}
		if version == "" {
			if err := bound.db.QueryRow(ctx, `SELECT target_version FROM memory_reembed_progress WHERE id=1`).Scan(&version); err != nil {
				return err
			}
		}
		var dim int
		if err := bound.db.QueryRow(ctx, `SELECT dimension FROM memory_embedder_versions WHERE version=$1`, version).Scan(&dim); err != nil {
			return errors.New("memory: no retained embeddings for requested version")
		}
		exists, err := bound.VectorCollectionExists(ctx)
		if err != nil {
			return err
		}
		if !exists {
			return errors.New("memory: vector index unavailable; schema-owner maintenance is required")
		}
		actual, err := bound.vectorDimension(ctx)
		if err != nil {
			return err
		}
		if dim != actual {
			return errors.New("memory: target dimension requires schema-owner maintenance")
		}
		if _, err = bound.db.Exec(ctx, `LOCK TABLE memories,memory_units IN SHARE MODE`); err != nil {
			return err
		}
		total, done, err := bound.reembedCounts(ctx, version)
		if err != nil {
			return err
		}
		pending, err := bound.pendingEmbeddingMetadata(ctx)
		if err != nil {
			return err
		}
		if pending != 0 {
			return errors.New("memory: derived metadata is still pending; wait for indexing before cutover")
		}
		if total != done {
			return fmt.Errorf("memory: version is incomplete or stale (%d/%d current vectors); resume reembed_start", done, total)
		}
		if _, err = bound.db.Exec(ctx, `DELETE FROM memory_embeddings WHERE record_type IN ('memory','unit')`); err != nil {
			return err
		}
		_, err = bound.db.Exec(ctx, embeddingInputs+`INSERT INTO memory_embeddings(point_id,embedding,record_type,primary_scope,workspace,project,kind,payload_json)
 SELECT i.point_id,v.embedding,i.record_type,i.scope_type,CASE WHEN i.scope_type='workspace' THEN i.scope_value ELSE '' END,
 CASE WHEN i.scope_type='project' THEN i.scope_value ELSE '' END,i.kind,
 (jsonb_build_object('record_type',i.record_type,'memory_id',i.memory_id,'kind',i.kind,'key',i.input_key,'primary_scope',i.scope_type,
 'workspace',CASE WHEN i.scope_type='workspace' THEN i.scope_value ELSE '' END,'project',CASE WHEN i.scope_type='project' THEN i.scope_value ELSE '' END,
 'embedder_version',$1::text) || CASE WHEN i.record_type='unit' THEN jsonb_build_object('unit_id',i.point_id-1000000000000,'unit_type',i.unit_type,'unit_key',i.input_key,'memory_kind',i.unit_kind,'weight',i.unit_weight) ELSE '{}'::jsonb END)::text
 FROM inputs i JOIN memory_embedding_versions v ON v.version=$1 AND v.point_id=i.point_id AND v.input_hash=i.input_hash`, version)
		if err != nil {
			return err
		}
		if _, err = bound.db.Exec(ctx, embeddingInputs+`INSERT INTO vector_index_ops(point_id,collection,memory_id,status,attempts,last_error,indexed_at,updated_at)
 SELECT point_id,'memory',memory_id,'ok',0,'',pg_now_text(),pg_now_text() FROM inputs ON CONFLICT(point_id) DO UPDATE SET status='ok',attempts=0,last_error='',indexed_at=pg_now_text(),updated_at=pg_now_text()`); err != nil {
			return err
		}
		if _, err = bound.db.Exec(ctx, `INSERT INTO memory_active_embedder(id,version,updated_at) VALUES(1,$1,pg_now_text()) ON CONFLICT(id) DO UPDATE SET version=EXCLUDED.version,updated_at=EXCLUDED.updated_at`, version); err != nil {
			return err
		}
		if _, err = bound.db.Exec(ctx, `UPDATE memory_reembed_progress SET total=$2,done=$2,finished_at=pg_now_text() WHERE id=1 AND target_version=$1`, version, total); err != nil {
			return err
		}
		count = total
		return nil
	})
	return count, err
}

func (s *postgresDataStore) reembedData(ctx context.Context, trace uint64, executor egress.Executor, req DataRequest) (json.RawMessage, error) {
	var value any
	var err error
	if err = s.requireAllVectorScopes(ctx); err != nil {
		return nil, err
	}
	switch req.Operation {
	case "reembed-prepare":
		err = s.prepareReembed(ctx, req.Version, req.Command)
		value = map[string]any{"status": "ok"}
	case "reembed-next":
		var ids []int64
		ids, err = s.reembedNext(ctx, req.Version, req.AfterID, req.Limit)
		value = map[string]any{"ids": ids}
	case "reembed-point":
		value, err = s.reembedPoint(ctx, trace, executor, req.Version, req.ID)
	case "reembed-status":
		value, err = s.reembedStatus(ctx)
	case "reembed-cutover":
		var n int64
		n, err = s.cutoverReembed(ctx, req.Version)
		if err == nil {
			err = s.db.QueryRow(ctx, `SELECT version FROM memory_active_embedder WHERE id=1`).Scan(&req.Version)
		}
		value = map[string]any{"status": "ok", "version": req.Version, "rebuilt": n, "done": n, "embedding_count": n, "rebuild_failed": 0}
	default:
		return nil, errors.New("memory: invalid re-embedding operation")
	}
	if err != nil {
		return nil, err
	}
	return json.Marshal(value)
}

// Ordinary indexing shares the cutover lock and verifies the provider selected
// before opening this transaction. If cutover won the race, retry with the new
// active provider rather than writing an old model into the new index.
func (s *postgresDataStore) activeEmbeddingGuard(ctx context.Context, command string, dimension int) error {
	if _, err := s.db.Exec(ctx, `SELECT pg_advisory_xact_lock_shared($1)`, vectorRebuildLock); err != nil {
		return err
	}
	version, active, dim, err := s.activeEmbeddingVersion(ctx)
	if err != nil {
		return err
	}
	if version != "" && (active != command || dim > dimension) {
		return errors.New("memory: embedding provider changed; retry using the active version")
	}
	return nil
}
func (s *postgresDataStore) activeEmbeddingVersion(ctx context.Context) (version, command string, dimension int, err error) {
	// Standalone/private fixtures may not install the KB's version catalog.
	var present bool
	err = s.db.QueryRow(ctx, `SELECT to_regclass('memory_embedder_versions') IS NOT NULL`).Scan(&present)
	if err != nil || !present {
		return
	}
	err = s.db.QueryRow(ctx, `SELECT v.version,v.command,v.dimension FROM memory_active_embedder a JOIN memory_embedder_versions v ON v.version=a.version WHERE a.id=1`).Scan(&version, &command, &dimension)
	if store.IsNoRows(err) {
		err = nil
	}
	return
}
func (s *postgresDataStore) retainActiveEmbedding(ctx context.Context, point int64, vector []float32) error {
	version, _, dimension, err := s.activeEmbeddingVersion(ctx)
	if err != nil || version == "" {
		return err
	}
	if len(vector) != dimension {
		return errors.New("memory: active embedding dimension mismatch")
	}
	in, err := s.embeddingInput(ctx, point)
	if err != nil {
		return err
	}
	return s.stageEmbedding(ctx, version, in, vector, "")
}

func (s *postgresDataStore) checkEmbeddingIdentity(ctx context.Context, version, servingID string) error {
	var identity string
	if err := s.db.QueryRow(ctx, `SELECT serving_id FROM memory_embedder_versions WHERE version=$1`, version).Scan(&identity); err != nil {
		return err
	}
	if identity != "" && identity != servingID {
		return errors.New("memory: embedder serving identity changed within a version")
	}
	if identity == "" && servingID != "" {
		tag, err := s.db.Exec(ctx, `UPDATE memory_embedder_versions SET serving_id=$2 WHERE version=$1 AND serving_id IN ('',$2)`, version, servingID)
		if err != nil {
			return err
		}
		if tag.RowsAffected() != 1 {
			return errors.New("memory: concurrent embedder identity change")
		}
	}
	return nil
}
func (s *postgresDataStore) checkActiveEmbeddingIdentity(ctx context.Context, servingID string) error {
	version, _, _, err := s.activeEmbeddingVersion(ctx)
	if err != nil || version == "" {
		return err
	}
	return s.checkEmbeddingIdentity(ctx, version, servingID)
}

func (s *postgresDataStore) pendingEmbeddingMetadata(ctx context.Context) (int64, error) {
	var n int64
	err := s.db.QueryRow(ctx, `SELECT count(*) FROM kb_async_jobs j JOIN memories m ON m.id=j.document_id
 WHERE j.kind='memory_index' AND j.status<>'done' AND `+indexableMemorySQL("m.")).Scan(&n)
	return n, err
}

func versionServingIdentity(ctx context.Context, trace uint64, executor egress.Executor, command string) (string, error) {
	if !EmbedIsHTTP(command) {
		// Legacy command embedders need not implement a health endpoint. Their
		// version binds the exact configured command, as well as its dimension.
		sum := sha256.Sum256([]byte(command))
		return fmt.Sprintf("command:%x", sum), nil
	}
	probe := EmbedServingID(ctx, trace, executor, command)
	id := strings.TrimSpace(probe.ServingID)
	if probe.Error != "" || id == "" || len(id) > 4096 || strings.ContainsAny(id, "\r\n{}[]") {
		return "", errors.New("memory: embedder serving identity unavailable")
	}
	return id, nil
}

func (s *postgresDataStore) embedForVersion(ctx context.Context, trace uint64, executor egress.Executor, version string, request EmbedRequest) EmbedResponse {
	identity := func() (string, error) { return versionServingIdentity(ctx, trace, executor, request.BaseURL) }
	before, err := identity()
	if err != nil {
		return EmbedResponse{Error: err.Error()}
	}
	var expected string
	if err = s.db.QueryRow(ctx, `SELECT serving_id FROM memory_embedder_versions WHERE version=$1`, version).Scan(&expected); err != nil {
		return EmbedResponse{Error: "memory: embedder version unavailable"}
	}
	if expected != "" && expected != before {
		return EmbedResponse{Error: "memory: embedder serving identity changed within a version"}
	}
	response := Embed(ctx, trace, executor, request)
	if response.Error != "" || response.Unavailable || response.Unauthorized || response.Truncated {
		return response
	}
	after, err := identity()
	if err != nil {
		return EmbedResponse{Error: err.Error()}
	}
	if before != after {
		return EmbedResponse{Error: "memory: embedder changed while producing the vector"}
	}
	response.ServingID = after
	return response
}
func (s *postgresDataStore) embedActiveVersion(ctx context.Context, trace uint64, executor egress.Executor, request EmbedRequest) EmbedResponse {
	version, _, _, err := s.activeEmbeddingVersion(ctx)
	if err != nil {
		return EmbedResponse{Error: "memory: active embedder unavailable"}
	}
	if version == "" {
		return Embed(ctx, trace, executor, request)
	}
	return s.embedForVersion(ctx, trace, executor, version, request)
}
