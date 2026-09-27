package memory

import (
	"context"
	"crypto/sha256"
	"errors"
	"fmt"

	store "github.com/JBailes/aimee/server-go/db"
)

const personalGenerationPolicy = "private-retained-revisions-cosine-v1"

var personalGenerationSchema = []string{
	`CREATE TABLE IF NOT EXISTS user_embedding_generations (
 generation text PRIMARY KEY, record_class text NOT NULL, serving_id text NOT NULL,
 policy text NOT NULL, identity_state text NOT NULL, dimension integer NOT NULL DEFAULT 0,
 state text NOT NULL CHECK(state IN ('created','backfilling','catching_up','validating','active','retired','failed','cancelled')),
 source_watermark bigint NOT NULL DEFAULT 0, validated_watermark bigint NOT NULL DEFAULT 0,
 created_at timestamptz NOT NULL DEFAULT now(),activated_at timestamptz)`,
	`CREATE TABLE IF NOT EXISTS user_embedding_active (
 record_class text PRIMARY KEY,generation text NOT NULL REFERENCES user_embedding_generations(generation))`,
	`CREATE TABLE IF NOT EXISTS user_memory_embedding_versions (
 generation text NOT NULL REFERENCES user_embedding_generations(generation),
 memory_id bigint NOT NULL REFERENCES user_memories(id) ON DELETE CASCADE,
 record_revision bigint NOT NULL,content_fingerprint text NOT NULL,embedding vector NOT NULL,
 created_at timestamptz NOT NULL DEFAULT now(),PRIMARY KEY(generation,memory_id,record_revision))`,
	`CREATE TABLE IF NOT EXISTS user_memory_embedding_jobs (
 generation text NOT NULL REFERENCES user_embedding_generations(generation),
 memory_id bigint NOT NULL REFERENCES user_memories(id) ON DELETE CASCADE,
 record_revision bigint NOT NULL,input_hash text NOT NULL,state text NOT NULL,
 attempts integer NOT NULL DEFAULT 0,last_error text NOT NULL DEFAULT '',
 next_attempt_at timestamptz NOT NULL DEFAULT '-infinity',created_at timestamptz NOT NULL DEFAULT now(),updated_at timestamptz NOT NULL DEFAULT now(),
 PRIMARY KEY(generation,memory_id,record_revision,input_hash))`,
}

func personalGenerationID(recordClass, serving string) string {
	sum := sha256.Sum256([]byte(recordClass + "\x00" + serving + "\x00" + personalGenerationPolicy))
	return fmt.Sprintf("private-%s:%x", recordClass, sum)
}
func (p *personalVectors) ensureGenerations(ctx context.Context) error {
	if p.ready.Load() {
		return nil
	}
	version, checksum, err := p.db.CurrentSchemaVersion(ctx, "memory-personal")
	if err != nil {
		return err
	}
	legacy := []string{personalVectorSchema}
	if version > 2 {
		return errors.New("private vector schema requires a newer owner")
	}
	if version == 1 && checksum != store.StoreChecksum(legacy) {
		return errors.New("private vector schema checksum mismatch")
	}
	if version < 1 {
		if err = p.db.Migrate(ctx, store.MigrationRequest{Owner: "memory-personal", Version: 1, Statements: legacy, Checksum: store.StoreChecksum(legacy)}); err != nil {
			return err
		}
	}
	if version == 2 && checksum != store.StoreChecksum(personalGenerationSchema) {
		return errors.New("private generation schema checksum mismatch")
	}
	if version < 2 {
		if err = p.db.Migrate(ctx, store.MigrationRequest{Owner: "memory-personal", Version: 2, Statements: personalGenerationSchema, Checksum: store.StoreChecksum(personalGenerationSchema)}); err != nil {
			return err
		}
	}
	p.ready.Store(true)
	return nil
}
func (p *personalVectors) prepareGeneration(ctx context.Context, recordClass, serving string) (string, error) {
	if err := p.ensureGenerations(ctx); err != nil {
		return "", err
	}
	generation := personalGenerationID(recordClass, serving)
	_, err := p.db.Exec(ctx, `INSERT INTO user_embedding_generations(generation,record_class,serving_id,policy,identity_state,state,source_watermark)
 VALUES($1,$2,$3,$4,$5,'backfilling',(SELECT generation FROM user_memory_collection_generation WHERE id=1)) ON CONFLICT(generation) DO NOTHING`, generation, recordClass, serving, personalGenerationPolicy, embeddingIdentityState(serving))
	return generation, err
}

// Retained versions have independent keys. Index membership cannot make them
// current; ordinary serving joins the parent's exact current revision below.
const personalIndexInputsSQL = `WITH retained AS (
 SELECT m.id AS memory_id,m.record_revision,to_jsonb(m) AS record FROM user_memories m
 WHERE m.lifecycle_state IN ('active','retired')
 UNION ALL
 SELECT v.memory_id,v.record_revision,v.record FROM user_memory_versions v JOIN user_memories m ON m.id=v.memory_id
 WHERE m.lifecycle_state IN ('active','retired') AND v.record_revision<m.record_revision AND v.record->>'lifecycle_state' IN ('active','retired')
), inputs AS (SELECT memory_id,record_revision,record->>'key' AS input_key,record->>'content' AS input_content,
 encode(sha256(convert_to(jsonb_build_array(memory_id,record_revision,record->>'key',record->>'content')::text,'UTF8')),'hex') AS input_hash FROM retained) `

func (p *personalVectors) indexRetainedVersion(ctx context.Context, endpoint, serving, generation string, id, revision int64, key, content, fingerprint string) (out EmbedResponse) {
	defer func() {
		if out.Error == "" && !out.Embedded {
			return
		}
		state := "done"
		reason := ""
		if out.Error != "" {
			state = "failed"
			reason = "embedding_attempt_failed"
		}
		_, err := p.db.Exec(ctx, `INSERT INTO user_memory_embedding_jobs(generation,memory_id,record_revision,input_hash,state,attempts,last_error,next_attempt_at)
  SELECT $1,id,$3,$4,$5,1,$6,CASE WHEN $5='failed' THEN clock_timestamp()+interval '5 seconds' ELSE '-infinity'::timestamptz END FROM user_memories WHERE id=$2
  ON CONFLICT(generation,memory_id,record_revision,input_hash) DO UPDATE SET state=EXCLUDED.state,attempts=user_memory_embedding_jobs.attempts+1,last_error=EXCLUDED.last_error,
  next_attempt_at=CASE WHEN EXCLUDED.state='failed' THEN clock_timestamp()+make_interval(secs=>LEAST(300,5*power(2,LEAST(user_memory_embedding_jobs.attempts,6)))::int) ELSE '-infinity'::timestamptz END,updated_at=clock_timestamp()`, generation, id, revision, fingerprint, state, reason)
		if err != nil && out.Error == "" {
			out.Error = err.Error()
			out.Embedded = false
		}
	}()
	result := Embed(ctx, 0, p.executor, EmbedRequest{BaseURL: endpoint, Text: key + "\n" + content, InputType: "document", MaxDim: 4000})
	if result.Error != "" || result.Unavailable || result.Unauthorized || result.Truncated {
		return EmbedResponse{Error: "local embedder could not index the complete version"}
	}
	vector, err := vectorLiteral(result.Vector)
	if err != nil {
		return EmbedResponse{Error: err.Error()}
	}
	after, err := p.serving(ctx, endpoint, len(result.Vector))
	if err != nil || after != serving {
		return EmbedResponse{Error: "embedding service changed during indexing"}
	}
	tag, err := p.db.Exec(ctx, `UPDATE user_embedding_generations SET dimension=$2 WHERE generation=$1 AND dimension IN (0,$2)`, generation, len(result.Vector))
	if err != nil {
		return EmbedResponse{Error: err.Error()}
	}
	if tag.RowsAffected() != 1 {
		return EmbedResponse{Error: "embedding dimensions changed within generation"}
	}
	tag, err = p.db.Exec(ctx, personalIndexInputsSQL+`INSERT INTO user_memory_embedding_versions(generation,memory_id,record_revision,content_fingerprint,embedding)
 SELECT $1,memory_id,record_revision,input_hash,$5::vector FROM inputs WHERE memory_id=$2 AND record_revision=$3 AND input_hash=$4
 ON CONFLICT(generation,memory_id,record_revision) DO UPDATE SET content_fingerprint=EXCLUDED.content_fingerprint,embedding=EXCLUDED.embedding`, generation, id, revision, fingerprint, vector)
	if err != nil {
		return EmbedResponse{Error: err.Error()}
	}
	result.Embedded = tag.RowsAffected() == 1
	result.ServingID = serving
	return result
}
func (p *personalVectors) generationTransaction(ctx context.Context, run func(store.Tx) error) error {
	// A caller may already own a fixture/request transaction. Never commit it.
	if tx, ok := p.db.(store.Tx); ok {
		return run(tx)
	}
	tx, err := p.db.Begin(ctx)
	if err != nil {
		return err
	}
	defer tx.Rollback(context.Background())
	if err = run(tx); err != nil {
		return err
	}
	return tx.Commit(ctx)
}
func (p *personalVectors) cutoverMemoryGeneration(ctx context.Context, generation string) error {
	return p.generationTransaction(ctx, func(tx store.Tx) error {
		// The schema-owner adapter serializes canonical writes without exposing
		// the protected collection journal or granting canonical write privileges.
		if _, err := tx.Exec(ctx, `SELECT memory_index_cutover_lock()`); err != nil {
			return err
		}
		if _, err := tx.Exec(ctx, `UPDATE user_embedding_generations SET state='catching_up' WHERE generation=$1 AND state IN ('created','backfilling','retired','failed','cancelled')`, generation); err != nil {
			return err
		}
		var pending int64
		if err := tx.QueryRow(ctx, personalIndexInputsSQL+`SELECT count(*) FROM inputs i LEFT JOIN user_memory_embedding_versions v ON v.generation=$1 AND v.memory_id=i.memory_id AND v.record_revision=i.record_revision
  WHERE v.memory_id IS NULL OR v.content_fingerprint<>i.input_hash OR vector_dims(v.embedding)<>(SELECT dimension FROM user_embedding_generations WHERE generation=$1) OR vector_norm(v.embedding)=0`, generation).Scan(&pending); err != nil {
			return err
		}
		if pending != 0 {
			return nil
		}
		if _, err := tx.Exec(ctx, personalIndexInputsSQL+`DELETE FROM user_memory_embedding_versions v WHERE NOT EXISTS(SELECT 1 FROM inputs i WHERE i.memory_id=v.memory_id AND i.record_revision=v.record_revision)`); err != nil {
			return err
		}
		if _, err := tx.Exec(ctx, `UPDATE user_embedding_generations SET state='validating' WHERE generation=$1 AND state<>'active'`, generation); err != nil {
			return err
		}
		if _, err := tx.Exec(ctx, `UPDATE user_embedding_generations SET state='retired' WHERE record_class='memory' AND state='active' AND generation<>$1`, generation); err != nil {
			return err
		}
		if _, err := tx.Exec(ctx, `UPDATE user_embedding_generations SET state='active',validated_watermark=(SELECT generation FROM user_memory_collection_generation WHERE id=1),activated_at=now() WHERE generation=$1`, generation); err != nil {
			return err
		}
		if _, err := tx.Exec(ctx, `INSERT INTO user_embedding_active(record_class,generation) VALUES('memory',$1) ON CONFLICT(record_class) DO UPDATE SET generation=EXCLUDED.generation`, generation); err != nil {
			return err
		}
		// Keep the old storage adapter readable only after atomic activation. Old
		// binaries still filter provider identity/fingerprint and current eligibility.
		if _, err := tx.Exec(ctx, `DELETE FROM user_memory_vectors`); err != nil {
			return err
		}
		_, err := tx.Exec(ctx, `INSERT INTO user_memory_vectors(memory_id,serving_id,content_fingerprint,embedding)
 SELECT m.id,g.serving_id,md5(m.key||chr(31)||m.content),v.embedding FROM user_memories m JOIN user_memory_embedding_versions v ON v.memory_id=m.id AND v.record_revision=m.record_revision
 JOIN user_embedding_generations g ON g.generation=v.generation WHERE v.generation=$1`, generation)
		return err
	})
}
