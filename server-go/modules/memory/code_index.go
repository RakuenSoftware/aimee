package memory

import (
	"context"
	"encoding/json"
	"errors"
	"net/url"
	"path"
	"strings"
	"sync"

	store "github.com/JBailes/aimee/server-go/db"
)

// Private repository data uses the server's existing PostgreSQL capability.
// It never enters the shared KB's tables or personal fact namespace.
const codeIndexSchema = `CREATE TABLE IF NOT EXISTS user_code_projects (
 name text PRIMARY KEY, root text NOT NULL, scanned_at timestamptz NOT NULL DEFAULT now(),
 generation bigint NOT NULL DEFAULT 0, scan_id text NOT NULL DEFAULT '', scan_started timestamptz
);
CREATE TABLE IF NOT EXISTS user_code_files (
 project text NOT NULL REFERENCES user_code_projects(name) ON DELETE CASCADE,
 path text NOT NULL, content text NOT NULL, module_identity text NOT NULL DEFAULT '', definitions jsonb NOT NULL DEFAULT '[]',
 calls jsonb NOT NULL DEFAULT '[]', imports jsonb NOT NULL DEFAULT '[]',
 fingerprint text NOT NULL, embedding vector, embedding_serving text NOT NULL DEFAULT '',
 embedding_fingerprint text NOT NULL DEFAULT '', PRIMARY KEY(project,path)
);
CREATE INDEX IF NOT EXISTS user_code_files_search ON user_code_files
 USING gin(to_tsvector('simple',path||' '||content));
CREATE TABLE IF NOT EXISTS user_code_stage (
 project text NOT NULL REFERENCES user_code_projects(name) ON DELETE CASCADE,
 scan_id text NOT NULL, path text NOT NULL, data jsonb NOT NULL,
 PRIMARY KEY(project,scan_id,path)
)`

type codeIndexState struct {
	mu    sync.Mutex
	ready bool
}

type CodeDefinition struct {
	Name    string `json:"name"`
	Kind    string `json:"kind"`
	Line    int    `json:"line"`
	LineEnd int    `json:"line_end"`
}
type CodeCall struct {
	Caller string `json:"caller"`
	Callee string `json:"callee"`
	Line   int    `json:"line"`
}
type CodeFile struct {
	ModuleIdentity string           `json:"module_identity"`
	Path           string           `json:"rel_path"`
	Content        string           `json:"content"`
	Definitions    []CodeDefinition `json:"definitions"`
	Calls          []CodeCall       `json:"calls"`
	Imports        []string         `json:"imports"`
}
type CodeIndexRequest struct {
	Route         string     `json:"route"`
	Project       string     `json:"project"`
	Root          string     `json:"root_path"`
	Phase         string     `json:"phase"`
	ScanID        string     `json:"scan_id"`
	ExpectedFiles int        `json:"expected_files"`
	Files         []CodeFile `json:"files"`
}

func (s *postgresDataStore) ensureCodeIndex(ctx context.Context) error {
	s.code.mu.Lock()
	defer s.code.mu.Unlock()
	if s.code.ready {
		return nil
	}
	db, ok := s.db.(store.Store)
	if !ok {
		return errors.New("code index migration capability unavailable")
	}
	statements := []string{codeIndexSchema}
	if err := db.Migrate(ctx, store.MigrationRequest{Owner: "memory-code", Version: 1, Statements: statements, Checksum: store.StoreChecksum(statements)}); err != nil {
		return err
	}
	s.code.ready = true
	return nil
}

func validCodePath(p string) bool {
	if p == "" || len(p) > 4096 || strings.ContainsAny(p, "\\\x00\r\n") || path.IsAbs(p) || path.Clean(p) != p || p == ".." || strings.HasPrefix(p, "../") {
		return false
	}
	for _, c := range strings.Split(p, "/") {
		if strings.HasPrefix(c, ".") {
			return false
		}
	}
	return true
}

func (s *postgresDataStore) CodeIndex(ctx context.Context, req CodeIndexRequest) (json.RawMessage, error) {
	if s.placement != PlacementServer {
		return nil, errors.New("private code index requires server placement")
	}
	if err := s.ensureCodeIndex(ctx); err != nil {
		return nil, err
	}
	route, err := url.ParseRequestURI(req.Route)
	if err != nil {
		return nil, errors.New("invalid code route")
	}
	if route.Path != "/v1/code/scan" {
		return s.queryCode(ctx, route, req)
	}
	if strings.TrimSpace(req.Project) == "" || len(req.Project) > 127 || len(req.Root) > 4096 || req.Root == "" || len(req.ScanID) > 128 || req.ExpectedFiles < 0 {
		return nil, errors.New("invalid code scan identity")
	}
	for _, f := range req.Files {
		if !validCodePath(f.Path) || len(f.ModuleIdentity) > 4096 || len(f.Content) > 512*1024 || strings.ContainsRune(f.Content, 0) || len(f.Definitions) > 256 || len(f.Calls) > 512 || len(f.Imports) > 128 {
			return nil, errors.New("invalid code file")
		}
	}
	db, ok := s.db.(store.DB)
	if !ok {
		return nil, errors.New("code transaction capability unavailable")
	}
	tx, err := db.Begin(ctx)
	if err != nil {
		return nil, err
	}
	defer tx.Rollback(ctx)
	if req.Phase == "begin" || req.Phase == "" {
		_, err = tx.Exec(ctx, `INSERT INTO user_code_projects(name,root,scan_id,scan_started) VALUES($1,$2,$3,now())
ON CONFLICT(name) DO UPDATE SET scan_id=EXCLUDED.scan_id,scan_started=now()`, req.Project, req.Root, req.ScanID)
		if err != nil {
			return nil, err
		}
	}
	var active string
	err = tx.QueryRow(ctx, `SELECT scan_id FROM user_code_projects WHERE name=$1 FOR UPDATE`, req.Project).Scan(&active)
	if err != nil {
		return nil, err
	}
	if req.Phase != "" && (req.ScanID == "" || req.ScanID != active) {
		return nil, errors.New("stale or missing scan session")
	}
	count := 0
	switch req.Phase {
	case "begin":
		_, err = tx.Exec(ctx, `DELETE FROM user_code_stage WHERE project=$1`, req.Project)
	case "stage", "":
		for _, f := range req.Files {
			if f.Definitions == nil {
				f.Definitions = []CodeDefinition{}
			}
			if f.Calls == nil {
				f.Calls = []CodeCall{}
			}
			if f.Imports == nil {
				f.Imports = []string{}
			}
			raw, e := json.Marshal(f)
			if e != nil {
				return nil, e
			}
			if req.Phase == "stage" {
				_, err = tx.Exec(ctx, `INSERT INTO user_code_stage(project,scan_id,path,data) VALUES($1,$2,$3,$4::jsonb)
ON CONFLICT(project,scan_id,path) DO UPDATE SET data=EXCLUDED.data`, req.Project, req.ScanID, f.Path, string(raw))
			} else {
				err = writeCodeFile(ctx, tx, req.Project, f.Path, string(raw))
			}
			if err != nil {
				return nil, err
			}
			count++
		}
		if req.Phase == "" {
			_, err = tx.Exec(ctx, `UPDATE user_code_projects SET root=$2,scanned_at=now(),generation=generation+1 WHERE name=$1`, req.Project, req.Root)
		}

	case "seal":
		if err = tx.QueryRow(ctx, `SELECT count(*) FROM user_code_stage WHERE project=$1 AND scan_id=$2`, req.Project, req.ScanID).Scan(&count); err != nil {
			return nil, err
		}
		if count != req.ExpectedFiles {
			return nil, errors.New("code manifest count mismatch")
		}
		_, err = tx.Exec(ctx, `DELETE FROM user_code_files WHERE project=$1 AND path NOT IN
(SELECT path FROM user_code_stage WHERE project=$1 AND scan_id=$2)`, req.Project, req.ScanID)
		if err != nil {
			return nil, err
		}
		_, err = tx.Exec(ctx, `INSERT INTO user_code_files(project,path,content,definitions,calls,imports,fingerprint,module_identity)
SELECT project,path,data->>'content',data->'definitions',data->'calls',data->'imports',md5(data->>'content'),COALESCE(data->>'module_identity','')
FROM user_code_stage WHERE project=$1 AND scan_id=$2
ON CONFLICT(project,path) DO UPDATE SET content=EXCLUDED.content,module_identity=EXCLUDED.module_identity,definitions=EXCLUDED.definitions,
calls=EXCLUDED.calls,imports=EXCLUDED.imports,fingerprint=EXCLUDED.fingerprint`, req.Project, req.ScanID)
		if err != nil {
			return nil, err
		}
		_, err = tx.Exec(ctx, `UPDATE user_code_projects SET root=$2,scanned_at=now(),generation=generation+1,scan_id='' WHERE name=$1`, req.Project, req.Root)
		if err != nil {
			return nil, err
		}
		_, err = tx.Exec(ctx, `DELETE FROM user_code_stage WHERE project=$1`, req.Project)
	case "abort":
		_, err = tx.Exec(ctx, `DELETE FROM user_code_stage WHERE project=$1 AND scan_id=$2`, req.Project, req.ScanID)
		if err == nil {
			_, err = tx.Exec(ctx, `UPDATE user_code_projects SET scan_id='' WHERE name=$1`, req.Project)
		}
	default:
		return nil, errors.New("unsupported code scan phase")
	}
	if err != nil {
		return nil, err
	}
	if err = tx.Commit(ctx); err != nil {
		return nil, err
	}
	return json.Marshal(map[string]any{"status": "ok", "project": req.Project, "files_scanned": count, "files": count, "inspected": count, "skipped": false, "reason": "indexed"})
}
func writeCodeFile(ctx context.Context, q store.Queryer, project, p, raw string) error {
	_, err := q.Exec(ctx, `INSERT INTO user_code_files(project,path,content,definitions,calls,imports,fingerprint,module_identity)
VALUES($1,$2,$3::jsonb->>'content',$3::jsonb->'definitions',$3::jsonb->'calls',$3::jsonb->'imports',md5($3::jsonb->>'content'),COALESCE($3::jsonb->>'module_identity',''))
ON CONFLICT(project,path) DO UPDATE SET content=EXCLUDED.content,module_identity=EXCLUDED.module_identity,definitions=EXCLUDED.definitions,
calls=EXCLUDED.calls,imports=EXCLUDED.imports,fingerprint=EXCLUDED.fingerprint`, project, p, raw)
	return err
}
