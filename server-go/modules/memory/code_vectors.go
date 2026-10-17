package memory

import (
	"context"
	"errors"
	"strings"
	"sync"
)

func (p *personalVectors) codeQueryVector(ctx context.Context, query string) (string, string, error) {
	endpoint, err := p.endpointCurrent()
	if err != nil || endpoint == "" {
		return "", "", errors.New("code embedder unavailable")
	}
	serving, err := p.serving(ctx, endpoint)
	if err != nil {
		return "", "", err
	}
	result := Embed(ctx, 0, p.executor, EmbedRequest{BaseURL: endpoint, Text: query, InputType: "query", MaxDim: 4000})
	if result.Error != "" || result.Unavailable || result.Unauthorized || result.Truncated {
		return "", "", errors.New("code query embedding unavailable")
	}
	vector, err := vectorLiteral(result.Vector)
	if err != nil {
		return "", "", err
	}
	after, err := p.serving(ctx, endpoint, len(result.Vector))
	if err != nil || after != serving {
		return "", "", errors.New("embedder changed during query")
	}
	return vector, serving, nil
}

func (p *personalVectors) indexCodeBatch(ctx context.Context) error {
	if p.code == nil {
		return nil
	}
	if err := p.code.ensureCodeIndex(ctx); err != nil {
		return err
	}
	endpoint, err := p.endpointCurrent()
	if err != nil || endpoint == "" {
		return err
	}
	serving, err := p.serving(ctx, endpoint)
	if err != nil {
		return err
	}
	generation, err := p.prepareCodeGeneration(ctx, serving)
	if err != nil {
		return err
	}
	if err := p.reuseCodeVectors(ctx, generation); err != nil {
		return err
	}

	type pending struct{ project, path, content, fingerprint, hash string }
	var items []pending
	// Most rebuild work is identified by cheap version/fingerprint/shape checks.
	// Do not hash every already-current file before selecting each small batch.
	// A disjoint fallback still repairs hash-only corruption when capacity remains.
	for _, hashOnly := range []bool{false, true} {
		if len(items) == 16 {
			break
		}
		shape := "vector_dims(v.embedding)<>(SELECT dimension FROM user_code_embedding_generations g WHERE g.project=f.project AND g.generation=v.generation) OR vector_norm(v.embedding)=0"
		mismatch := "v.path IS NULL OR v.content_fingerprint<>f.fingerprint OR " + shape
		if hashOnly {
			mismatch = "v.path IS NOT NULL AND v.content_fingerprint=f.fingerprint AND NOT (" + shape + ") AND v.input_hash<>" + codeEmbeddingInputHashSQL
		}
		rows, err := p.db.Query(ctx, `SELECT project,path,content,fingerprint,input_hash FROM (
 SELECT f.project,f.path,left(f.content,8192) AS content,f.fingerprint,`+codeEmbeddingInputHashSQL+` AS input_hash,
 row_number() OVER (PARTITION BY f.project ORDER BY f.path) AS project_turn
 FROM user_code_files f LEFT JOIN user_code_embedding_versions v ON v.project=f.project AND v.path=f.path AND v.generation=$1
 LEFT JOIN user_code_embedding_jobs j ON j.project=f.project AND j.path=f.path AND j.generation=$1 AND j.input_hash=`+codeEmbeddingInputHashSQL+`
 WHERE (`+mismatch+`)
 AND (j.path IS NULL OR (j.attempts<8 AND j.next_attempt_at<=clock_timestamp()))
 ) pending ORDER BY project_turn,project,path LIMIT $2`, generation, 16-len(items))
		if err != nil {
			return err
		}
		for rows.Next() {
			var item pending
			if err = rows.Scan(&item.project, &item.path, &item.content, &item.fingerprint, &item.hash); err != nil {
				rows.Close()
				return err
			}
			items = append(items, item)
		}
		err = rows.Err()
		rows.Close()
		if err != nil {
			return err
		}
	}

	var workErr error
	// Two simultaneous document calls let the shipped embedder coalesce work,
	// while bounding model memory and leaving interactive retrieval headroom.
	// Publish each pair before proceeding so a cancelled later pair cannot lose
	// successfully embedded earlier files. Database access stays serial.
	for offset := 0; offset < len(items); offset += 2 {
		pair := items[offset:min(offset+2, len(items))]
		results := make([]EmbedResponse, len(pair))
		var workers sync.WaitGroup
		for i, item := range pair {
			workers.Add(1)
			go func(i int, item pending) {
				defer workers.Done()
				content := strings.ToValidUTF8(item.path+"\n"+item.content, "")
				results[i] = Embed(ctx, 0, p.executor, EmbedRequest{BaseURL: endpoint, Text: content, InputType: "document", MaxDim: 4000})
			}(i, item)
		}
		workers.Wait()
		for i, item := range pair {
			success, err := func() (bool, error) {
				result := results[i]
				if result.Error != "" || result.Unavailable || result.Unauthorized || result.Truncated {
					return false, errors.New("code embedding unavailable")
				}
				vector, err := vectorLiteral(result.Vector)
				if err != nil {
					return false, err
				}
				after, err := p.serving(ctx, endpoint, len(result.Vector))
				if err != nil || after != serving {
					return false, errors.New("code embedding identity changed")
				}
				tag, err := p.db.Exec(ctx, `UPDATE user_code_embedding_generations SET dimension=$3 WHERE project=$1 AND generation=$2 AND dimension IN (0,$3)`, item.project, generation, len(result.Vector))
				if err != nil {
					return false, err
				}
				if tag.RowsAffected() != 1 {
					return false, errors.New("code dimensions changed within generation")
				}
				tag, err = p.db.Exec(ctx, `INSERT INTO user_code_embedding_versions(project,path,generation,content_fingerprint,input_hash,embedding)
 SELECT f.project,f.path,$3,f.fingerprint,$4,$5::vector FROM user_code_files f WHERE f.project=$1 AND f.path=$2 AND `+codeEmbeddingInputHashSQL+`=$4 AND f.fingerprint=$6
 ON CONFLICT(project,path,generation) DO UPDATE SET content_fingerprint=EXCLUDED.content_fingerprint,input_hash=EXCLUDED.input_hash,embedding=EXCLUDED.embedding`, item.project, item.path, generation, item.hash, vector, item.fingerprint)
				return err == nil && tag.RowsAffected() == 1, err
			}()
			workErr = errors.Join(workErr, err, p.recordCodeIndexAttempt(ctx, item.project, item.path, generation, item.hash, success))
		}
	}
	projects, err := p.db.Query(ctx, `SELECT g.project FROM user_code_embedding_generations g JOIN user_code_projects p ON p.name=g.project
 LEFT JOIN user_code_embedding_active a ON a.project=g.project
 WHERE g.generation=$1 AND (a.generation IS DISTINCT FROM g.generation OR g.validated_watermark<>p.generation)
 AND NOT EXISTS (
 SELECT 1 FROM user_code_files f LEFT JOIN user_code_embedding_versions v ON v.project=f.project AND v.path=f.path AND v.generation=g.generation
 WHERE f.project=g.project AND (v.path IS NULL OR v.content_fingerprint<>f.fingerprint OR v.input_hash<>`+codeEmbeddingInputHashSQL+` OR vector_dims(v.embedding)<>g.dimension OR vector_norm(v.embedding)=0))
 ORDER BY g.created_at,g.project LIMIT 16`, generation)
	if err != nil {
		return errors.Join(workErr, err)
	}
	var names []string
	for projects.Next() {
		var name string
		if err := projects.Scan(&name); err != nil {
			projects.Close()
			return errors.Join(workErr, err)
		}
		names = append(names, name)
	}
	err = projects.Err()
	projects.Close()
	if err != nil {
		return errors.Join(workErr, err)
	}
	for _, project := range names {
		workErr = errors.Join(workErr, p.cutoverCodeGeneration(ctx, project, generation))
	}
	return workErr
}

// Repository snapshots often contain exactly the same embedding input. Reuse
// only verified vectors for the identical input hash, serving identity and
// preprocessing policy (all bound into generation). Publication and retrieval
// still validate each target file's own fingerprint and project watermark.
func (p *personalVectors) reuseCodeVectors(ctx context.Context, generation string) error {

	_, err := p.db.Exec(ctx, `WITH pending AS MATERIALIZED (
 SELECT f.project,f.path,f.fingerprint,`+codeEmbeddingInputHashSQL+` AS input_hash,g.dimension
 FROM user_code_files f JOIN user_code_embedding_generations g ON g.project=f.project AND g.generation=$1
 LEFT JOIN user_code_embedding_versions v ON v.project=f.project AND v.path=f.path AND v.generation=$1
 WHERE g.identity_state='verified' AND (v.path IS NULL OR v.content_fingerprint<>f.fingerprint OR vector_dims(v.embedding)<>g.dimension OR vector_norm(v.embedding)=0)
), inputs AS MATERIALIZED (
 SELECT DISTINCT input_hash FROM pending
), cached AS MATERIALIZED (
 SELECT DISTINCT ON (v.input_hash) v.input_hash,v.embedding,vector_dims(v.embedding) AS dim
 FROM user_code_embedding_versions v JOIN user_code_embedding_generations g USING(project,generation)
 JOIN inputs i ON i.input_hash=v.input_hash
 WHERE v.generation=$1 AND g.identity_state='verified' AND vector_dims(v.embedding)=g.dimension AND vector_norm(v.embedding)>0
 ORDER BY v.input_hash,v.project,v.path
), reusable AS MATERIALIZED (
 SELECT p.project,p.path,p.fingerprint,c.input_hash,c.embedding,c.dim
 FROM pending p JOIN cached c USING(input_hash) WHERE p.dimension IN (0,c.dim)
 ORDER BY p.project,p.path LIMIT 4096
), dimensions AS (
 UPDATE user_code_embedding_generations g SET dimension=p.dim
 FROM (SELECT DISTINCT project,dim FROM reusable) p
 WHERE g.project=p.project AND g.generation=$1 AND g.dimension IN (0,p.dim)
 RETURNING g.project
)
INSERT INTO user_code_embedding_versions(project,path,generation,content_fingerprint,input_hash,embedding)
 SELECT p.project,p.path,$1,p.fingerprint,p.input_hash,p.embedding FROM reusable p JOIN dimensions d USING(project)
 ON CONFLICT(project,path,generation) DO UPDATE SET content_fingerprint=EXCLUDED.content_fingerprint,input_hash=EXCLUDED.input_hash,embedding=EXCLUDED.embedding`, generation)

	return err
}
