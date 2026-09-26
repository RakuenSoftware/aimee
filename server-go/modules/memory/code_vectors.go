package memory

import (
	"context"
	"errors"
	"strings"
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
	after, err := p.serving(ctx, endpoint)
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
	rows, err := p.db.Query(ctx, `SELECT f.project,f.path,left(f.content,8192),f.fingerprint,`+codeEmbeddingInputHashSQL+`
 FROM user_code_files f LEFT JOIN user_code_embedding_versions v ON v.project=f.project AND v.path=f.path AND v.generation=$1
 LEFT JOIN user_code_embedding_jobs j ON j.project=f.project AND j.path=f.path AND j.generation=$1 AND j.input_hash=`+codeEmbeddingInputHashSQL+`
 WHERE (v.path IS NULL OR v.content_fingerprint<>f.fingerprint OR v.input_hash<>`+codeEmbeddingInputHashSQL+` OR vector_dims(v.embedding)<>(SELECT dimension FROM user_code_embedding_generations g WHERE g.project=f.project AND g.generation=v.generation) OR vector_norm(v.embedding)=0)
 AND (j.path IS NULL OR (j.attempts<8 AND j.next_attempt_at<=clock_timestamp())) ORDER BY f.project,f.path LIMIT 16`, generation)
	if err != nil {
		return err
	}
	type pending struct{ project, path, content, fingerprint, hash string }
	var items []pending
	for rows.Next() {
		var item pending
		if err := rows.Scan(&item.project, &item.path, &item.content, &item.fingerprint, &item.hash); err != nil {
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
	var workErr error
	for _, item := range items {
		success, err := func() (bool, error) {
			content := strings.ToValidUTF8(item.path+"\n"+item.content, "")
			result := Embed(ctx, 0, p.executor, EmbedRequest{BaseURL: endpoint, Text: content, InputType: "document", MaxDim: 4000})
			if result.Error != "" || result.Unavailable || result.Unauthorized || result.Truncated {
				return false, errors.New("code embedding unavailable")
			}
			vector, err := vectorLiteral(result.Vector)
			if err != nil {
				return false, err
			}
			after, err := p.serving(ctx, endpoint)
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
	projects, err := p.db.Query(ctx, `SELECT g.project FROM user_code_embedding_generations g JOIN user_code_projects p ON p.name=g.project
 LEFT JOIN user_code_embedding_active a ON a.project=g.project
 WHERE g.generation=$1 AND (a.generation IS DISTINCT FROM g.generation OR g.validated_watermark<>p.generation) ORDER BY g.created_at,g.project LIMIT 16`, generation)
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
