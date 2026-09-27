package memory

import (
	"bufio"
	"context"
	"encoding/json"
	"github.com/JBailes/aimee/server-go/bus"
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

// Filtered exports use the same authenticated transaction as other inspection
// views. Entity metadata is rebuilt from those exported parents; unversioned
// global profile cards cannot be used as a scoped authority. Exported kinds are data; importing them never imports actor authority.
type filteredExportRequest struct {
	Workspace       string `json:"workspace"`
	Kind            string `json:"kind"`
	Since           string `json:"since"`
	IncludeArchived bool   `json:"include_archived"`
}

func (s *postgresDataStore) exportFiltered(ctx context.Context, q filteredExportRequest) (json.RawMessage, error) {
	if err := s.requireKBDomain(); err != nil {
		return nil, err
	}
	var raw string
	err := s.db.QueryRow(ctx, `WITH selected AS MATERIALIZED (
 SELECT m.id,m.tier,m.kind,m.key,m.content,m.confidence,m.use_count,m.lifecycle_state,
 m.created_at,m.updated_at,m.source_session,m.epistemic_kind,
 COALESCE(CASE WHEN m.scope_type='workspace' THEN m.scope_value END,
 (SELECT ms.scope_value FROM memory_scopes ms WHERE ms.memory_id=m.id AND ms.scope_type='workspace' ORDER BY ms.scope_value LIMIT 1),
 (SELECT mw.workspace FROM memory_workspaces mw WHERE mw.memory_id=m.id ORDER BY mw.workspace LIMIT 1),'') AS workspace
 FROM memories m WHERE ($1='' OR (m.scope_type='workspace' AND m.scope_value=$1)
 OR EXISTS(SELECT 1 FROM memory_scopes ms WHERE ms.memory_id=m.id AND ms.scope_type='workspace' AND ms.scope_value=$1)
 OR EXISTS(SELECT 1 FROM memory_workspaces mw WHERE mw.memory_id=m.id AND mw.workspace=$1))
 AND ($2='' OR $2='all' OR m.kind=$2) AND ($3='' OR `+memoryTimeSQL("m.created_at")+`>=`+memoryTimeSQL("$3::text")+`)
 AND ($4 OR m.lifecycle_state='active')
), profiles AS (
 SELECT e.entity AS entity_id,e.entity AS canonical_name,count(DISTINCT m.id) AS observation_count,
 '{}'::text AS card_json FROM memory_entities e JOIN selected m ON m.id=e.memory_id GROUP BY e.entity
)
SELECT jsonb_build_object('status','ok','schema_version','1','exported_at',to_char(CURRENT_TIMESTAMP AT TIME ZONE 'UTC','YYYY-MM-DD"T"HH24:MI:SS"Z"'),
 'count',(SELECT count(*) FROM selected),'memories',COALESCE((SELECT jsonb_agg(to_jsonb(m) ORDER BY m.id) FROM selected m),'[]'::jsonb),
 'entity_profiles',COALESCE((SELECT jsonb_agg(to_jsonb(p) ORDER BY p.entity_id) FROM profiles p),'[]'::jsonb))::text`, q.Workspace, q.Kind, q.Since, q.IncludeArchived).Scan(&raw)
	if err != nil {
		return nil, err
	}
	var result map[string]json.RawMessage
	if err = json.Unmarshal([]byte(raw), &result); err != nil {
		return nil, err
	}
	if q.Workspace != "" {
		result["workspace"], _ = json.Marshal(q.Workspace)
	}
	if q.Kind != "" {
		result["kind"], _ = json.Marshal(q.Kind)
	}
	if q.Since != "" {
		result["since"], _ = json.Marshal(q.Since)
	}
	return json.Marshal(result)
}

func handleFilteredExport(options handlerOptions, invocation bus.ModuleInvocation, args commandArgs) ([]byte, bus.ModuleStatus) {
	for _, field := range []string{"workspace", "kind", "since"} {
		if raw, exists := args[field]; exists {
			var value string
			if string(raw) == "null" || json.Unmarshal(raw, &value) != nil {
				return commandResult(commandError("invalid_argument", field+" must be a string"))
			}
		}
	}
	q := filteredExportRequest{Workspace: args.stringOr("workspace", ""), Kind: args.stringOr("kind", ""), Since: args.stringOr("since", "")}
	if raw, ok := args["include_archived"]; ok && (string(raw) == "null" || json.Unmarshal(raw, &q.IncludeArchived) != nil) {
		return commandResult(commandError("invalid_argument", "include_archived must be boolean"))
	}
	if q.Since != "" {
		if _, err := parseMemoryTime(q.Since); err != nil {
			return commandResult(commandError("invalid_argument", err.Error()))
		}
	}
	r := DataRequest{Operation: "export-filtered", IncludeAll: true, Workspace: q.Workspace, FilteredExport: &q}
	commandScope(args, &r)
	encoded, err := json.Marshal(r)
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	raw, status := handleData(options, invocation, encoded)
	if status != bus.ModuleStatusOK {
		return nil, status
	}
	var response DataResponse
	if json.Unmarshal(raw, &response) != nil || len(response.Payload) == 0 {
		return nil, bus.ModuleStatusInternal
	}
	return commandResult(response.Payload)
}
