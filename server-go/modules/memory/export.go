package memory

import (
	"bufio"
	"context"
	"encoding/json"
	"os"
	"path/filepath"
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

func (s *postgresDataStore) ExportDecisionsJSONL(ctx context.Context, path string) (int, error) {
	return s.ExportJSONL(ctx, path, true)
}

// Stage output beside its destination, then publish only after every read and
// write succeeds. Failed exports leave an existing destination intact.
func (s *postgresDataStore) ExportJSONL(ctx context.Context, path string, decisions bool) (count int, err error) {
	if err = s.requireKBDomain(); err != nil {
		return 0, err
	}
	file, err := os.CreateTemp(filepath.Dir(path), ".aimee-memory-export-*")
	if err != nil {
		return 0, err
	}
	defer func() { file.Close(); os.Remove(file.Name()) }()
	writer := bufio.NewWriter(file)
	encoder := json.NewEncoder(writer)
	var afterID int64
	for {
		if err = ctx.Err(); err != nil {
			return count, err
		}
		records, readErr := s.ExportRecords(ctx, afterID, 128)
		if readErr != nil {
			return count, readErr
		}
		if len(records) == 0 {
			break
		}
		for _, record := range records {
			afterID = record.ID
			if decisions && record.Kind != "decision" {
				continue
			}
			var value any = record
			if !decisions {
				scopes, scopeErr := s.ScopeCollect(ctx, record.ID)
				if scopeErr != nil {
					return count, scopeErr
				}
				primary, scopeErr := s.PrimaryScope(ctx, record.ID)
				if scopeErr != nil {
					return count, scopeErr
				}
				value = struct {
					ExportRecord
					Scopes       []ScopeTag `json:"scopes"`
					PrimaryScope ScopeTag   `json:"primary_scope"`
				}{record, scopes, primary}
			}
			if err = encoder.Encode(value); err != nil {
				return count, err
			}
			count++
		}
	}
	if err = writer.Flush(); err != nil {
		return count, err
	}
	if err = file.Sync(); err != nil {
		return count, err
	}
	if err = file.Close(); err != nil {
		return count, err
	}
	if err = ctx.Err(); err != nil {
		return count, err
	}
	err = os.Rename(file.Name(), path)
	return count, err
}
