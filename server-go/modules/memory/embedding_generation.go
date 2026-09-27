package memory

import (
	"context"
	"crypto/sha256"
	"encoding/json"
	"errors"
	"fmt"
	"strings"
)

// These are owner policies, not caller-controlled provider parameters.
const memoryGenerationChunking = "memory-whole-and-unit-v2"
const memoryGenerationExtractor = "canonical-revision-retained-v1"

type embeddingGenerationStatus struct {
	Version            string                    `json:"version"`
	State              string                    `json:"state"`
	IdentityState      string                    `json:"identity_state"`
	GenerationDigest   string                    `json:"generation_digest,omitempty"`
	SnapshotWatermark  json.RawMessage           `json:"snapshot_watermark"`
	ValidatedWatermark json.RawMessage           `json:"validated_watermark"`
	Coverage           json.RawMessage           `json:"coverage"`
	CurrentWatermark   json.RawMessage           `json:"current_watermark,omitempty"`
	StorageReadiness   string                    `json:"storage_readiness,omitempty"`
	Classes            []embeddingClassReadiness `json:"classes,omitempty"`
	Queue              embeddingQueueReadiness   `json:"queue"`
}

// Watermarks contain commitments and aggregate generations, never record IDs.
// Only the all-scope maintenance owner calls this function. Per-request class
// diagnostics must not expose this global inventory as scoped completeness.
const embeddingWatermarkSQL = `SELECT jsonb_build_object('schema_version',1,'digest',
 'sha256:'||encode(sha256(convert_to(jsonb_build_object(
 'canonical',COALESCE((SELECT jsonb_agg(jsonb_build_array(scope_type,scope_value,generation::text) ORDER BY scope_type,scope_value) FROM memory_collection_generations),'[]'::jsonb),
 'projections',COALESCE((SELECT jsonb_agg(jsonb_build_array(scope_type,scope_value,generation) ORDER BY scope_type,scope_value) FROM (SELECT scope_type,scope_value,sum(generation)::text AS generation FROM memory_projection_generations GROUP BY scope_type,scope_value) p),'[]'::jsonb),
 'assertions',COALESCE((SELECT sum(generation)::text FROM kb_async_jobs WHERE kind='memory_assertion_index'),'0'))::text,'UTF8')),'hex'))::text`

func (s *postgresDataStore) embeddingWatermark(ctx context.Context) (string, error) {
	var watermark string
	err := s.db.QueryRow(ctx, embeddingWatermarkSQL).Scan(&watermark)
	return watermark, err
}
func (s *postgresDataStore) embeddingGeneration(ctx context.Context, version string) (*embeddingGenerationStatus, error) {
	result := &embeddingGenerationStatus{Version: version}
	var snapshot, validated, coverage string
	err := s.db.QueryRow(ctx, `SELECT generation_state,identity_state,generation_digest,snapshot_watermark::text,validated_watermark::text,coverage::text FROM memory_embedder_versions WHERE version=$1`, version).Scan(&result.State, &result.IdentityState, &result.GenerationDigest, &snapshot, &validated, &coverage)
	if err != nil {
		return nil, err
	}
	result.SnapshotWatermark = json.RawMessage(snapshot)
	result.ValidatedWatermark = json.RawMessage(validated)
	result.Coverage = json.RawMessage(coverage)
	return result, nil
}
func embeddingGenerationTransition(from, to string) bool {
	if from == to {
		return true
	}
	switch from {
	case "created":
		return to == "backfilling" || to == "cancelled" || to == "failed"
	case "backfilling":
		return to == "catching_up" || to == "failed" || to == "cancelled"
	case "catching_up":
		return to == "backfilling" || to == "validating" || to == "failed" || to == "cancelled"
	case "validating":
		return to == "backfilling" || to == "active" || to == "failed" || to == "cancelled"
	case "active":
		return to == "retired"
	// Retained rollback still requires the same complete validation as cutover.
	case "retired":
		return to == "backfilling" || to == "validating"
	case "failed", "cancelled":
		return to == "backfilling"
	}
	return false
}
func (s *postgresDataStore) transitionEmbeddingGeneration(ctx context.Context, version, to string) error {
	var from string
	if err := s.db.QueryRow(ctx, `SELECT generation_state FROM memory_embedder_versions WHERE version=$1 FOR UPDATE`, version).Scan(&from); err != nil {
		return err
	}
	if !embeddingGenerationTransition(from, to) {
		return errors.New("memory: invalid embedding generation transition")
	}
	_, err := s.db.Exec(ctx, `UPDATE memory_embedder_versions SET generation_state=$2 WHERE version=$1`, version, to)
	return err
}
func (s *postgresDataStore) bindEmbeddingGenerationIdentity(ctx context.Context, version, servingID string) error {
	state := embeddingIdentityState(servingID)
	embeddingDigest := strings.TrimPrefix(servingID, "embedding-v1:")
	if state != "verified" {
		// Keep legacy identity explicitly unknown, while still binding the index
		// policy. This commitment never upgrades an unverified alias into a model ID.
		embeddingDigest = fmt.Sprintf("sha256:%x", sha256.Sum256([]byte("legacy-unknown\x00"+servingID)))
	}
	digest, err := (IndexGenerationIdentity{EmbeddingDigest: embeddingDigest, ChunkingPolicy: memoryGenerationChunking, ExtractorPolicy: memoryGenerationExtractor, DistancePolicy: "cosine-v1", IndexSettings: "exact-version-pinned-v1"}).Digest()
	if err != nil {
		return err
	}
	tag, err := s.db.Exec(ctx, `UPDATE memory_embedder_versions SET identity_state=$2,generation_digest=$3 WHERE version=$1 AND (generation_digest='' OR generation_digest=$3)`, version, state, digest)
	if err != nil {
		return err
	}
	if tag.RowsAffected() != 1 {
		return errors.New("memory: index policy changed within embedding generation")
	}
	return nil
}

type embeddingClassReadiness struct {
	RecordClass   string    `json:"record_class"`
	Placement     Placement `json:"placement"`
	TemporalModes []string  `json:"temporal_modes"`
	Total         int64     `json:"total"`
	Indexed       int64     `json:"indexed"`
	Pending       int64     `json:"pending"`
}
type embeddingQueueReadiness struct {
	Pending          int64   `json:"pending"`
	Retrying         int64   `json:"retrying"`
	Failed           int64   `json:"failed"`
	OldestAgeSeconds float64 `json:"oldest_age_seconds"`
}

func (s *postgresDataStore) generationReadiness(ctx context.Context, result *embeddingGenerationStatus) error {
	if result == nil {
		return nil
	}
	if err := s.requireAllVectorScopes(ctx); err != nil {
		return err
	}
	current, err := s.embeddingWatermark(ctx)
	if err != nil {
		return err
	}
	result.CurrentWatermark = json.RawMessage(current)
	var now, validated struct {
		Digest string `json:"digest"`
	}
	if err = json.Unmarshal([]byte(current), &now); err != nil {
		return err
	}
	if err = json.Unmarshal(result.ValidatedWatermark, &validated); err != nil {
		return err
	}
	rows, err := s.db.Query(ctx, embeddingInputs()+`SELECT classes.record_type,count(i.point_id),count(v.point_id) FILTER(WHERE v.input_hash=i.input_hash AND v.source_revision=i.record_revision AND v.embedding IS NOT NULL AND vector_dims(v.embedding)=(SELECT dimension FROM memory_embedder_versions WHERE version=$1) AND vector_norm(v.embedding)>0)
 FROM (VALUES('memory'),('unit')) classes(record_type) LEFT JOIN inputs i ON i.record_type=classes.record_type LEFT JOIN memory_embedding_versions v ON v.point_id=i.point_id AND v.version=$1 GROUP BY classes.record_type ORDER BY classes.record_type`, result.Version)
	if err != nil {
		return err
	}
	result.Classes = []embeddingClassReadiness{}
	for rows.Next() {
		c := embeddingClassReadiness{Placement: PlacementKB, TemporalModes: []string{"current"}}
		if err := rows.Scan(&c.RecordClass, &c.Total, &c.Indexed); err != nil {
			rows.Close()
			return err
		}
		c.Pending = c.Total - c.Indexed
		result.Classes = append(result.Classes, c)
	}
	err = rows.Err()
	rows.Close()
	if err != nil {
		return err
	}
	assertion := embeddingClassReadiness{RecordClass: "semantic_assertion", Placement: PlacementKB, TemporalModes: []string{"current", "historical", "valid_at", "believed_at"}}
	assertion.Total, assertion.Indexed, err = s.assertionReembedCounts(ctx, result.Version)
	if err != nil {
		return err
	}
	assertion.Pending = assertion.Total - assertion.Indexed
	result.Classes = append(result.Classes, assertion)
	err = s.db.QueryRow(ctx, `SELECT count(*) FILTER(WHERE status<>'done'),count(*) FILTER(WHERE status='failed' AND attempts<8),count(*) FILTER(WHERE status='failed' AND attempts>=8),COALESCE(max(GREATEST(0,extract(epoch FROM clock_timestamp()-NULLIF(created_at,'')::timestamptz))) FILTER(WHERE status<>'done'),0)::double precision
 FROM kb_async_jobs WHERE kind IN ('memory_index','memory_assertion_index')`).Scan(&result.Queue.Pending, &result.Queue.Retrying, &result.Queue.Failed, &result.Queue.OldestAgeSeconds)
	if err != nil {
		return err
	}
	switch result.State {
	case "created", "backfilling", "catching_up", "validating":
		result.StorageReadiness = "rebuilding"
	case "failed", "cancelled":
		result.StorageReadiness = "unavailable"
	default:
		result.StorageReadiness = "ready"
		if now.Digest == "" || now.Digest != validated.Digest || result.Queue.Pending != 0 {
			result.StorageReadiness = "lagging"
		}
		for _, c := range result.Classes {
			if c.Pending != 0 {
				result.StorageReadiness = "lagging"
			}
		}
	}
	return nil
}
