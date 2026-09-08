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
	rows, err := p.db.Query(ctx, `SELECT project,path,left(content,8192),fingerprint FROM user_code_files
WHERE embedding IS NULL OR embedding_serving<>$1 OR embedding_fingerprint<>fingerprint
ORDER BY project,path LIMIT 16`, serving)
	if err != nil {
		return err
	}
	type pending struct{ project, path, content, fingerprint string }
	var items []pending
	for rows.Next() {
		var v pending
		if err := rows.Scan(&v.project, &v.path, &v.content, &v.fingerprint); err != nil {
			rows.Close()
			return err
		}
		items = append(items, v)
	}
	err = rows.Err()
	rows.Close()
	if err != nil {
		return err
	}
	for _, v := range items {
		content := strings.ToValidUTF8(v.path+"\n"+v.content, "")
		result := Embed(ctx, 0, p.executor, EmbedRequest{BaseURL: endpoint, Text: content, InputType: "document", MaxDim: 4000})
		if result.Error != "" || result.Unavailable || result.Unauthorized || result.Truncated {
			return errors.New("code embedding unavailable")
		}
		vector, err := vectorLiteral(result.Vector)
		if err != nil {
			return err
		}
		after, err := p.serving(ctx, endpoint)
		if err != nil || after != serving {
			return errors.New("embedder changed during code indexing")
		}
		_, err = p.db.Exec(ctx, `UPDATE user_code_files SET embedding=$5::vector,embedding_serving=$4,embedding_fingerprint=$3
WHERE project=$1 AND path=$2 AND fingerprint=$3`, v.project, v.path, v.fingerprint, serving, vector)
		if err != nil {
			return err
		}
	}
	return nil
}
