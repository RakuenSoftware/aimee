package memory

import (
	"bufio"
	"context"
	"encoding/json"
	"os"
)

type ExportRecord struct {
	ID            int64   `json:"id"`
	Tier          string  `json:"tier"`
	Kind          string  `json:"kind"`
	Key           string  `json:"key"`
	Content       string  `json:"content"`
	Confidence    float64 `json:"confidence"`
	UseCount      int     `json:"use_count"`
	SourceSession string  `json:"source_session"`
	CreatedAt     string  `json:"created_at"`
	UpdatedAt     string  `json:"updated_at"`
}

func (s *postgresDataStore) ExportRecords(ctx context.Context, afterID int64, limit int) ([]ExportRecord, error) {
	if err := s.requireKBDomain(); err != nil {
		return nil, err
	}
	rows, err := s.db.Query(ctx, `SELECT id,tier,kind,key,content,confidence,use_count,
COALESCE(source_session,''),created_at,updated_at FROM memories WHERE id>$1 ORDER BY id LIMIT $2`, afterID, limit)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	items := make([]ExportRecord, 0)
	for rows.Next() {
		var item ExportRecord
		if err := rows.Scan(&item.ID, &item.Tier, &item.Kind, &item.Key, &item.Content,
			&item.Confidence, &item.UseCount, &item.SourceSession, &item.CreatedAt, &item.UpdatedAt); err != nil {
			return nil, err
		}
		items = append(items, item)
	}
	return items, rows.Err()
}

func (s *postgresDataStore) ExportDecisionsJSONL(ctx context.Context, path string) (count int, err error) {
	if err = s.requireKBDomain(); err != nil {
		return 0, err
	}
	rows, err := s.db.Query(ctx, `SELECT id,tier,kind,key,content,confidence,use_count,
COALESCE(source_session,''),created_at,updated_at FROM memories WHERE kind='decision' ORDER BY id`)
	if err != nil {
		return 0, err
	}
	defer rows.Close()
	file, err := os.OpenFile(path, os.O_CREATE|os.O_WRONLY|os.O_TRUNC, 0600)
	if err != nil {
		return 0, err
	}
	writer := bufio.NewWriter(file)
	defer func() {
		if flushErr := writer.Flush(); err == nil {
			err = flushErr
		}
		if closeErr := file.Close(); err == nil {
			err = closeErr
		}
	}()
	encoder := json.NewEncoder(writer)
	for rows.Next() {
		var item ExportRecord
		if err = rows.Scan(&item.ID, &item.Tier, &item.Kind, &item.Key, &item.Content,
			&item.Confidence, &item.UseCount, &item.SourceSession, &item.CreatedAt, &item.UpdatedAt); err != nil {
			return count, err
		}
		if err = encoder.Encode(item); err != nil {
			return count, err
		}
		count++
	}
	return count, rows.Err()
}
