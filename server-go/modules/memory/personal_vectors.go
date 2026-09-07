package memory

import (
	"context"
	"crypto/md5"
	"encoding/hex"
	"errors"
	"log"
	"math"
	"sort"
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
	p := &personalVectors{endpoint: endpoint, executor: executor, db: db, wake: make(chan struct{}, 1)}
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
func (p *personalVectors) serving(ctx context.Context, endpoint string) (string, error) {
	result := EmbedServingID(ctx, 0, p.executor, endpoint)
	if result.Error != "" || result.ServingID == "" || len(result.ServingID) > 4096 {
		return "", errors.New("local embedding identity unavailable")
	}
	return result.ServingID, nil
}
func (p *personalVectors) run(ctx context.Context) {
	timer := time.NewTicker(5 * time.Second)
	defer timer.Stop()
	for {
		attempt, cancel := context.WithTimeout(ctx, 60*time.Second)
		err := p.indexBatch(attempt)
		cancel()
		if err != nil && ctx.Err() == nil {
			log.Printf("personal memory embedding pending: %v", err)
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
	endpoint, err := p.endpointCurrent()
	if err != nil {
		return err
	}
	if endpoint == "" {
		return nil
	}
	if !p.ready.Load() {
		statements := []string{personalVectorSchema}
		if err := p.db.Migrate(ctx, store.MigrationRequest{Owner: "memory-personal", Version: 1, Statements: statements, Checksum: store.StoreChecksum(statements)}); err != nil {
			return err
		}
		p.ready.Store(true)
	}
	serving, err := p.serving(ctx, endpoint)
	if err != nil {
		return err
	}
	rows, err := p.db.Query(ctx, `SELECT m.id,m.key,m.content,md5(m.key||chr(31)||m.content)
FROM user_memories m LEFT JOIN user_memory_vectors v ON v.memory_id=m.id
WHERE m.lifecycle_state='active' AND (m.valid_until IS NULL OR m.valid_until>now())
AND (v.memory_id IS NULL OR v.serving_id<>$1 OR v.content_fingerprint<>md5(m.key||chr(31)||m.content))
ORDER BY m.updated_at,m.id LIMIT 16`, serving)
	if err != nil {
		return err
	}
	type pending struct {
		id                        int64
		key, content, fingerprint string
	}
	var items []pending
	for rows.Next() {
		var r pending
		if err = rows.Scan(&r.id, &r.key, &r.content, &r.fingerprint); err != nil {
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
	for _, r := range items {
		if result := p.indexRecord(ctx, endpoint, serving, r.id, r.key, r.content, r.fingerprint); result.Error != "" {
			return errors.New(result.Error)
		}
	}
	return nil
}

func (p *personalVectors) indexRecord(ctx context.Context, endpoint, serving string, id int64, key, content, fingerprint string) EmbedResponse {
	result := Embed(ctx, 0, p.executor, EmbedRequest{BaseURL: endpoint, Text: key + "\n" + content, InputType: "document", MaxDim: 4000})
	if result.Error != "" || result.Unavailable || result.Unauthorized || result.Truncated {
		return EmbedResponse{Error: "local embedder could not index the complete record"}
	}
	vector, err := vectorLiteral(result.Vector)
	if err != nil {
		return EmbedResponse{Error: err.Error()}
	}
	after, err := p.serving(ctx, endpoint)
	if err != nil || after != serving {
		return EmbedResponse{Error: "embedding service changed during indexing"}
	}
	changed, err := p.db.Exec(ctx, `INSERT INTO user_memory_vectors(memory_id,serving_id,content_fingerprint,embedding)
SELECT id,$2,$3,$4::vector FROM user_memories
WHERE id=$1 AND lifecycle_state='active' AND (valid_until IS NULL OR valid_until>now())
AND md5(key||chr(31)||content)=$3
ON CONFLICT(memory_id) DO UPDATE SET serving_id=EXCLUDED.serving_id,content_fingerprint=EXCLUDED.content_fingerprint,embedding=EXCLUDED.embedding,updated_at=now()`, id, serving, fingerprint, vector)
	if err != nil {
		return EmbedResponse{Error: err.Error()}
	}
	result.Embedded = changed.RowsAffected() > 0
	result.ServingID = serving
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
	endpoint, err := p.endpointCurrent()
	if err != nil {
		return nil, err
	}
	if endpoint == "" {
		return nil, errors.New("no local embedding configured")
	}
	if !p.ready.Load() {
		return nil, errors.New("personal vector index is initializing")
	}
	serving, err := p.serving(ctx, endpoint)
	if err != nil {
		return nil, err
	}
	result := Embed(ctx, 0, p.executor, EmbedRequest{BaseURL: endpoint, Text: query, InputType: "query", MaxDim: 4000})
	if result.Error != "" || result.Unavailable || result.Unauthorized || result.Truncated {
		return nil, errors.New("local query embedding unavailable")
	}
	vector, err := vectorLiteral(result.Vector)
	if err != nil {
		return nil, err
	}
	after, err := p.serving(ctx, endpoint)
	if err != nil || after != serving {
		return nil, errors.New("embedding service changed during recall")
	}
	rows, err := p.db.Query(ctx, `SELECT m.id,m.tier,m.kind,m.key,m.content,m.confidence
FROM user_memories m JOIN user_memory_vectors v ON v.memory_id=m.id
WHERE m.lifecycle_state='active' AND (m.valid_until IS NULL OR m.valid_until>now())
AND v.serving_id=$1 AND v.content_fingerprint=md5(m.key||chr(31)||m.content)
AND vector_dims(v.embedding)=vector_dims($2::vector)
AND ($3='' OR m.kind=$3) AND ($4='' OR m.tier=$4)
AND CASE WHEN vector_dims(v.embedding)=vector_dims($2::vector) THEN 1-(v.embedding <=> $2::vector)>0.3 ELSE false END
ORDER BY CASE WHEN vector_dims(v.embedding)=vector_dims($2::vector) THEN v.embedding <=> $2::vector END,m.id LIMIT $5`, serving, vector, kind, tier, limit)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var records []Record
	for rows.Next() {
		r := Record{Scope: Scope{Type: ScopeUser, Value: "_user"}}
		if err = rows.Scan(&r.ID, &r.Tier, &r.Kind, &r.Key, &r.Content, &r.Confidence); err != nil {
			return nil, err
		}
		records = append(records, r)
	}
	return records, rows.Err()
}
func fusePersonal(lexical, semantic []Record, limit int) []Record {
	scores := map[int64]float64{}
	records := map[int64]Record{}
	for _, list := range [][]Record{lexical, semantic} {
		for rank, r := range list {
			scores[r.ID] += 1 / float64(60+rank+1)
			records[r.ID] = r
		}
	}
	out := make([]Record, 0, len(records))
	for _, r := range records {
		out = append(out, r)
	}
	sort.Slice(out, func(i, j int) bool {
		a, b := scores[out[i].ID], scores[out[j].ID]
		if a == b {
			return out[i].ID < out[j].ID
		}
		return a > b
	})
	if len(out) > limit {
		out = out[:limit]
	}
	return out
}
