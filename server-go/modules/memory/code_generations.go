package memory

import (
	"context"
	"crypto/sha256"
	"errors"
	"fmt"

	store "github.com/JBailes/aimee/server-go/db"
)

const codeGenerationPolicy = "code-path-prefix8192-cosine-v1"
const codeEmbeddingInputHashSQL = `encode(sha256(convert_to(f.path||chr(10)||left(f.content,8192),'UTF8')),'hex')`

var codeGenerationSchema = []string{
	`CREATE TABLE IF NOT EXISTS user_code_embedding_generations (
 project text NOT NULL REFERENCES user_code_projects(name) ON DELETE CASCADE,
 generation text NOT NULL,serving_id text NOT NULL,policy text NOT NULL,identity_state text NOT NULL,
 state text NOT NULL,dimension integer NOT NULL DEFAULT 0,source_watermark bigint NOT NULL,validated_watermark bigint NOT NULL DEFAULT -1,
 created_at timestamptz NOT NULL DEFAULT now(),PRIMARY KEY(project,generation))`,
	`CREATE TABLE IF NOT EXISTS user_code_embedding_active (
 project text PRIMARY KEY REFERENCES user_code_projects(name) ON DELETE CASCADE,
 generation text NOT NULL,FOREIGN KEY(project,generation) REFERENCES user_code_embedding_generations(project,generation))`,
	`CREATE TABLE IF NOT EXISTS user_code_embedding_versions (
 project text NOT NULL,path text NOT NULL,generation text NOT NULL,content_fingerprint text NOT NULL,input_hash text NOT NULL,embedding vector NOT NULL,
 PRIMARY KEY(project,path,generation),FOREIGN KEY(project,path) REFERENCES user_code_files(project,path) ON DELETE CASCADE,
 FOREIGN KEY(project,generation) REFERENCES user_code_embedding_generations(project,generation))`,
	`CREATE TABLE IF NOT EXISTS user_code_embedding_jobs (
 project text NOT NULL,path text NOT NULL,generation text NOT NULL,input_hash text NOT NULL,
 state text NOT NULL,attempts integer NOT NULL DEFAULT 0,next_attempt_at timestamptz NOT NULL DEFAULT '-infinity',
 created_at timestamptz NOT NULL DEFAULT now(),PRIMARY KEY(project,path,generation,input_hash),
 FOREIGN KEY(project,path) REFERENCES user_code_files(project,path) ON DELETE CASCADE,
 FOREIGN KEY(project,generation) REFERENCES user_code_embedding_generations(project,generation))`,
}

func codeGenerationID(serving string) string {
	return fmt.Sprintf("code:%x", sha256.Sum256([]byte(serving+"\x00"+codeGenerationPolicy)))
}
func (p *personalVectors) prepareCodeGeneration(ctx context.Context, serving string) (string, error) {
	generation := codeGenerationID(serving)
	_, err := p.db.Exec(ctx, `INSERT INTO user_code_embedding_generations(project,generation,serving_id,policy,identity_state,state,source_watermark)
 SELECT name,$1,$2,$3,$4,'backfilling',generation FROM user_code_projects ON CONFLICT(project,generation) DO NOTHING`, generation, serving, codeGenerationPolicy, embeddingIdentityState(serving))
	return generation, err
}
func (p *personalVectors) cutoverCodeGeneration(ctx context.Context, project, generation string) error {
	return p.generationTransaction(ctx, func(tx store.Tx) error {
		// Publication already locks this same project row before changing its files.
		var watermark int64
		if err := tx.QueryRow(ctx, `SELECT generation FROM user_code_projects WHERE name=$1 FOR SHARE NOWAIT`, project).Scan(&watermark); err != nil {
			return err
		}
		if _, err := tx.Exec(ctx, `UPDATE user_code_embedding_generations SET state='catching_up' WHERE project=$1 AND generation=$2 AND state<>'active'`, project, generation); err != nil {
			return err
		}
		var pending int64
		if err := tx.QueryRow(ctx, `SELECT count(*) FROM user_code_files f LEFT JOIN user_code_embedding_versions v ON v.project=f.project AND v.path=f.path AND v.generation=$2
 WHERE f.project=$1 AND (v.path IS NULL OR v.content_fingerprint<>f.fingerprint OR v.input_hash<>`+codeEmbeddingInputHashSQL+` OR vector_dims(v.embedding)<>(SELECT dimension FROM user_code_embedding_generations g WHERE g.project=f.project AND g.generation=v.generation) OR vector_norm(v.embedding)=0)`, project, generation).Scan(&pending); err != nil {
			return err
		}
		if pending != 0 {
			return nil
		}
		if _, err := tx.Exec(ctx, `UPDATE user_code_embedding_generations SET state='validating' WHERE project=$1 AND generation=$2 AND state<>'active'`, project, generation); err != nil {
			return err
		}
		if _, err := tx.Exec(ctx, `UPDATE user_code_embedding_generations SET state='retired' WHERE project=$1 AND state='active' AND generation<>$2`, project, generation); err != nil {
			return err
		}
		if _, err := tx.Exec(ctx, `UPDATE user_code_embedding_generations SET state='active',validated_watermark=$3 WHERE project=$1 AND generation=$2`, project, generation, watermark); err != nil {
			return err
		}
		if _, err := tx.Exec(ctx, `INSERT INTO user_code_embedding_active(project,generation) VALUES($1,$2) ON CONFLICT(project) DO UPDATE SET generation=EXCLUDED.generation`, project, generation); err != nil {
			return err
		}
		// The legacy adapter is replaced only with a complete compatible project.
		_, err := tx.Exec(ctx, `UPDATE user_code_files f SET embedding=v.embedding,embedding_serving=g.serving_id,embedding_fingerprint=v.content_fingerprint
 FROM user_code_embedding_versions v JOIN user_code_embedding_generations g ON g.project=v.project AND g.generation=v.generation
 WHERE f.project=$1 AND v.generation=$2 AND v.project=f.project AND v.path=f.path AND v.content_fingerprint=f.fingerprint`, project, generation)
		return err
	})
}
func (p *personalVectors) recordCodeIndexAttempt(ctx context.Context, project, path, generation, hash string, success bool) error {
	state := "failed"
	if success {
		state = "done"
	}
	_, err := p.db.Exec(ctx, `INSERT INTO user_code_embedding_jobs(project,path,generation,input_hash,state,attempts,next_attempt_at)
 SELECT project,path,$3,$4,$5,1,CASE WHEN $5='failed' THEN clock_timestamp()+interval '5 seconds' ELSE '-infinity'::timestamptz END FROM user_code_files WHERE project=$1 AND path=$2
 ON CONFLICT(project,path,generation,input_hash) DO UPDATE SET state=EXCLUDED.state,attempts=user_code_embedding_jobs.attempts+1,
 next_attempt_at=CASE WHEN EXCLUDED.state='failed' THEN clock_timestamp()+interval '30 seconds' ELSE '-infinity'::timestamptz END`, project, path, generation, hash, state)
	return err
}
func migrateCodeGenerations(ctx context.Context, db store.Store) error {
	version, checksum, err := db.CurrentSchemaVersion(ctx, "memory-code")
	if err != nil {
		return err
	}
	original := []string{codeIndexSchema}
	if version > 2 {
		return errors.New("code vector schema requires a newer owner")
	}
	if version == 1 && checksum != store.StoreChecksum(original) {
		return errors.New("code schema checksum mismatch")
	}
	if version < 1 {
		if err = db.Migrate(ctx, store.MigrationRequest{Owner: "memory-code", Version: 1, Statements: original, Checksum: store.StoreChecksum(original)}); err != nil {
			return err
		}
	}
	if version == 2 && checksum != store.StoreChecksum(codeGenerationSchema) {
		return errors.New("code generation schema checksum mismatch")
	}
	if version < 2 {
		return db.Migrate(ctx, store.MigrationRequest{Owner: "memory-code", Version: 2, Statements: codeGenerationSchema, Checksum: store.StoreChecksum(codeGenerationSchema)})
	}
	return nil
}
