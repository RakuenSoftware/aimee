package memory

import (
	"context"
	"crypto/md5"
	"encoding/hex"
	"errors"
	"fmt"
	"log"
	"math"
	"strconv"
	"strings"
	"sync/atomic"
	"time"

	store "github.com/JBailes/aimee/server-go/db"
	"github.com/JBailes/aimee/server-go/modules/egress"
)

// Personal vectors are a storage adapter inside the same memory module. They
// never reference shared KB tables. Content fingerprints prevent an in-flight
// embedding from becoming current after a concurrent edit or retirement.
const personalVectorSchema = `CREATE TABLE IF NOT EXISTS user_memory_vectors (
 memory_id bigint PRIMARY KEY REFERENCES user_memories(id) ON DELETE CASCADE,
 serving_id text NOT NULL,
 content_fingerprint text NOT NULL,
 embedding vector NOT NULL,
 updated_at timestamptz NOT NULL DEFAULT now()
)`

type personalVectors struct {
	code     *postgresDataStore
	settings func() (string, error)
	endpoint string
	executor egress.Executor
	db       store.Store
	ready    atomic.Bool
	wake     chan struct{}
}

// StartPersonalIndex attaches embedding to the local owner. Empty endpoint is
// supported for a native record-only deployment; the standard container supplies
// its local model service. Model failure never redirects personal data to a KB.
func StartPersonalIndex(ctx context.Context, data DataStore, executor egress.Executor, endpoint string) {
	s, ok := data.(*postgresDataStore)
	if !ok || s.placement != PlacementServer || executor == nil || ctx == nil {
		return
	}
	db, ok := s.db.(store.Store)
	if !ok {
		return
	}
	p := &personalVectors{code: s, endpoint: endpoint, executor: executor, db: db, wake: make(chan struct{}, 1)}
	if configured, ok := s.db.(interface{ EmbeddingEndpoint() (string, error) }); ok {
		p.settings = configured.EmbeddingEndpoint
	}
	s.personal = p
	go p.run(ctx)
}
func vectorLiteral(vector []float32) (string, error) {
	if len(vector) == 0 || len(vector) > 4000 {
		return "", errors.New("invalid embedding dimensions")
	}
	parts := make([]string, len(vector))
	norm := float64(0)
	for i, v := range vector {
		if math.IsNaN(float64(v)) || math.IsInf(float64(v), 0) {
			return "", errors.New("nonfinite embedding")
		}
		parts[i] = strconv.FormatFloat(float64(v), 'g', -1, 32)
		norm += float64(v) * float64(v)
	}
	if norm == 0 {
		return "", errors.New("zero embedding")
	}
	return "[" + strings.Join(parts, ",") + "]", nil
}
func (p *personalVectors) serving(ctx context.Context, endpoint string, expectedDimensions ...int) (string, error) {
	result := EmbedServingID(ctx, 0, p.executor, endpoint)
	if result.Error != "" || result.ServingID == "" || len(result.ServingID) > 4096 {
		return "", errors.New("local embedding identity unavailable")
	}
	if result.IdentityState == "verified" && len(expectedDimensions) > 0 && expectedDimensions[0] > 0 && result.Dim != expectedDimensions[0] {
		return "", errors.New("local vector dimensions disagree with embedding identity")
	}
	return result.ServingID, nil
}
func (p *personalVectors) run(ctx context.Context) {
	timer := time.NewTicker(5 * time.Second)
	defer timer.Stop()
	for {
		attempt, cancel := context.WithTimeout(ctx, 60*time.Second)
		err := errors.Join(p.indexBatch(attempt), p.indexCodeBatch(attempt))
		cancel()
		if err != nil && ctx.Err() == nil {
			log.Printf("local memory/code embedding pending: %v", err)
		}
		select {
		case <-ctx.Done():
			return
		case <-timer.C:
		case <-p.wake:
		}
	}
}
func (p *personalVectors) endpointCurrent() (string, error) {
	if p.settings != nil {
		return p.settings()
	}
	return p.endpoint, nil
}
func (p *personalVectors) indexBatch(ctx context.Context) error {
	// Apply tombstones even while the model route is offline. Every retained
	// generation observes processing revocation before more model work is tried.
	if err := p.ensureGenerations(ctx); err != nil {
		return err
	}
	if _, err := p.db.Exec(ctx, `DELETE FROM user_memory_embedding_versions v USING user_memories m WHERE m.id=v.memory_id AND m.lifecycle_state NOT IN ('active','retired')`); err != nil {
		return err
	}
	if _, err := p.db.Exec(ctx, `DELETE FROM user_memory_vectors v USING user_memories m WHERE m.id=v.memory_id AND m.lifecycle_state NOT IN ('active','retired')`); err != nil {
		return err
	}

	endpoint, err := p.endpointCurrent()
	if err != nil {
		return err
	}
	if endpoint == "" {
		return nil
	}
	serving, err := p.serving(ctx, endpoint)
	if err != nil {
		return err
	}
	generation, err := p.prepareGeneration(ctx, "memory", serving)
	if err != nil {
		return err
	}
	var current bool
	if err = p.db.QueryRow(ctx, `SELECT EXISTS(SELECT 1 FROM user_embedding_active a JOIN user_embedding_generations g ON g.generation=a.generation WHERE a.record_class='memory' AND a.generation=$1 AND g.validated_watermark=(SELECT generation FROM user_memory_collection_generation WHERE id=1))`, generation).Scan(&current); err != nil {
		return err
	}
	if current {
		return nil
	}
	rows, err := p.db.Query(ctx, personalIndexInputsSQL+`SELECT i.memory_id,i.record_revision,i.input_key,i.input_content,i.input_hash
 FROM inputs i LEFT JOIN user_memory_embedding_versions v ON v.generation=$1 AND v.memory_id=i.memory_id AND v.record_revision=i.record_revision
 LEFT JOIN user_memory_embedding_jobs j ON j.generation=$1 AND j.memory_id=i.memory_id AND j.record_revision=i.record_revision AND j.input_hash=i.input_hash
 WHERE (v.memory_id IS NULL OR v.content_fingerprint<>i.input_hash OR vector_dims(v.embedding)<>(SELECT dimension FROM user_embedding_generations WHERE generation=$1) OR vector_norm(v.embedding)=0) AND (j.memory_id IS NULL OR (j.attempts<8 AND j.next_attempt_at<=clock_timestamp())) ORDER BY i.memory_id,i.record_revision LIMIT 16`, generation)
	if err != nil {
		return err
	}
	type pending struct {
		id, revision              int64
		key, content, fingerprint string
	}
	var items []pending
	for rows.Next() {
		var r pending
		if err = rows.Scan(&r.id, &r.revision, &r.key, &r.content, &r.fingerprint); err != nil {
			rows.Close()
			return err
		}
		items = append(items, r)
	}
	err = rows.Err()
	rows.Close()
	if err != nil {
		return err
	}
	var workErr error
	for _, r := range items {
		if result := p.indexRetainedVersion(ctx, endpoint, serving, generation, r.id, r.revision, r.key, r.content, r.fingerprint); result.Error != "" {
			workErr = errors.Join(workErr, errors.New(result.Error))
		}
	}
	return errors.Join(workErr, p.cutoverMemoryGeneration(ctx, generation))
}

func (p *personalVectors) indexRecord(ctx context.Context, endpoint, serving string, id int64, key, content, fingerprint string) EmbedResponse {
	generation, err := p.prepareGeneration(ctx, "memory", serving)
	if err != nil {
		return EmbedResponse{Error: err.Error()}
	}
	var revision int64
	var committed string
	err = p.db.QueryRow(ctx, personalIndexInputsSQL+`SELECT i.record_revision,i.input_hash FROM inputs i JOIN user_memories m ON m.id=i.memory_id AND m.record_revision=i.record_revision
 WHERE i.memory_id=$1 AND i.input_key=$2 AND i.input_content=$3 AND md5(m.key||chr(31)||m.content)=$4`, id, key, content, fingerprint).Scan(&revision, &committed)
	if store.IsNoRows(err) {
		return EmbedResponse{Error: "personal indexing source changed"}
	}
	if err != nil {
		return EmbedResponse{Error: err.Error()}
	}
	result := p.indexRetainedVersion(ctx, endpoint, serving, generation, id, revision, key, content, committed)
	if result.Error == "" && result.Embedded {
		if err := p.cutoverMemoryGeneration(ctx, generation); err != nil {
			return EmbedResponse{Error: err.Error()}
		}
	}
	return result
}

// Explicit embedding uses the same owner-selected model and stale-write guard
// as background indexing. It cannot write a colliding ID in the KB namespace.
func (s *postgresDataStore) embedPersonalRecord(ctx context.Context, record Record) EmbedResponse {
	p := s.personal
	if p == nil || !p.ready.Load() {
		return EmbedResponse{Error: "personal vector index is unavailable"}
	}
	endpoint, err := p.endpointCurrent()
	if err != nil || endpoint == "" {
		return EmbedResponse{Error: "personal embedder is unavailable"}
	}
	serving, err := p.serving(ctx, endpoint)
	if err != nil {
		return EmbedResponse{Error: err.Error()}
	}
	fingerprint := md5.Sum([]byte(record.Key + string(rune(31)) + record.Content))
	return p.indexRecord(ctx, endpoint, serving, record.ID, record.Key, record.Content, hex.EncodeToString(fingerprint[:]))
}

func (p *personalVectors) search(ctx context.Context, query, kind, tier string, limit int) ([]Record, error) {
	return p.searchWithStore(ctx, p.db, query, kind, tier, limit)
}

// Candidate reads share the calling owner transaction and its request clock.
func (p *personalVectors) searchWithStore(ctx context.Context, db store.Queryer, query, kind, tier string, limit int) ([]Record, error) {
	observation := retrievalArmObservation{State: "unavailable", Reason: "bounded_local_vector_fallback", Quota: limit, IndexReadiness: "unavailable"}
	defer func() { recordRetrievalArm(ctx, "dense", observation) }()

	endpoint, err := p.endpointCurrent()
	if err != nil {
		return nil, err
	}
	if endpoint == "" {
		return nil, errors.New("no local embedding configured")
	}
	if !p.ready.Load() {
		observation.IndexReadiness = "rebuilding"
		return nil, errors.New("personal vector index is initializing")
	}
	var generation, expected, policy string
	var current, validated int64
	if err := db.QueryRow(ctx, `SELECT a.generation,g.serving_id,g.policy,g.validated_watermark,c.generation FROM user_embedding_active a JOIN user_embedding_generations g ON g.generation=a.generation CROSS JOIN user_memory_collection_generation c WHERE a.record_class='memory' AND c.id=1`).Scan(&generation, &expected, &policy, &validated, &current); err != nil {
		observation.IndexReadiness = "rebuilding"
		return nil, errors.New("personal generation unavailable")
	}
	observation.IndexVersion = generation
	observation.IdentityState = embeddingIdentityState(expected)
	observation.CurrentWatermark = fmt.Sprint(current)
	observation.ValidatedWatermark = fmt.Sprint(validated)
	if policy != personalGenerationPolicy {
		observation.IndexReadiness = "identity_mismatch"
		return nil, errors.New("personal generation policy mismatch")
	}
	serving, err := p.serving(ctx, endpoint)
	if err != nil {
		return nil, err
	}
	if serving != expected {
		observation.IndexReadiness = "identity_mismatch"
		return nil, errors.New("personal embedding identity mismatch")
	}
	result := Embed(ctx, 0, p.executor, EmbedRequest{BaseURL: endpoint, Text: query, InputType: "query", MaxDim: 4000})
	if result.Error != "" || result.Unavailable || result.Unauthorized || result.Truncated {
		return nil, errors.New("local query embedding unavailable")
	}
	vector, err := vectorLiteral(result.Vector)
	if err != nil {
		return nil, err
	}
	after, err := p.serving(ctx, endpoint, len(result.Vector))
	if err != nil || after != serving {
		observation.IndexReadiness = "identity_mismatch"
		return nil, errors.New("embedding service changed during recall")
	}
	rows, err := db.Query(ctx, `SELECT m.id,m.tier,m.kind,m.key,m.content,m.confidence,1-(v.embedding <=> $2::vector),(SELECT owner_id::text FROM user_memory_collection_generation WHERE id=1),m.record_revision::text
FROM user_memories m JOIN user_memory_embedding_versions v ON v.memory_id=m.id AND v.record_revision=m.record_revision
WHERE `+personalCurrentMemorySQL("m.")+`
AND v.generation=$1 AND v.content_fingerprint=encode(sha256(convert_to(jsonb_build_array(m.id,m.record_revision,m.key,m.content)::text,'UTF8')),'hex')
AND vector_dims(v.embedding)=vector_dims($2::vector)
AND ($3='' OR m.kind=$3) AND ($4='' OR m.tier=$4)
AND CASE WHEN vector_dims(v.embedding)=vector_dims($2::vector) THEN 1-(v.embedding <=> $2::vector)>0.3 ELSE false END
ORDER BY CASE WHEN vector_dims(v.embedding)=vector_dims($2::vector) THEN v.embedding <=> $2::vector END,m.id LIMIT $5`, generation, vector, kind, tier, limit)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var records []Record
	for rows.Next() {
		var similarity float64
		r := Record{observedVersion: &MemoryRecordVersion{SchemaVersion: 1}, Scope: Scope{Type: ScopeUser, Value: "_user"}}
		if err = rows.Scan(&r.ID, &r.Tier, &r.Kind, &r.Key, &r.Content, &r.Confidence, &similarity, &r.observedVersion.OwnerID, &r.observedVersion.RecordRevision); err != nil {
			return nil, err
		}
		r.observedVersion.RecordID = fmt.Sprint(r.ID)
		recordNativeRank(ctx, &r, "semantic", len(records)+1, similarity, "cosine_similarity")
		records = append(records, r)
	}
	if rows.Err() == nil {
		observation.State = "available"
		observation.Reason = "private_current_revision_generation"
		observation.Candidates = len(records)
		observation.IndexReadiness = "ready"
		if current != validated {
			observation.IndexReadiness = "lagging"
		}
	}
	return records, rows.Err()
}
func fusePersonal(lexical, semantic []Record, limit int) []Record {
	return fuseRanked(context.Background(), lexical, semantic, limit, "lexical", "semantic")
}
