package families

// A repository is runtime business data, not personal memory. Each staged scan
// owns an immutable snapshot; publication changes only the current project head.
import (
	"context"
	"crypto/rand"
	"crypto/sha256"
	"encoding/json"
	"errors"
	"fmt"
	"math"
	"net/url"
	"path"
	"strings"
	"unicode/utf8"

	store "github.com/JBailes/aimee/server-go/modules/aimee"
)

const codeGenerationPolicy = "runtime-code-path-prefix8192-bytes-cosine/v1"

type codeDefinition struct {
	Name    string `json:"name"`
	Kind    string `json:"kind"`
	Line    int    `json:"line"`
	LineEnd int    `json:"line_end"`
}
type codeCall struct {
	Caller string `json:"caller"`
	Callee string `json:"callee"`
	Line   int    `json:"line"`
}
type codeFile struct {
	ModuleIdentity   string           `json:"module_identity"`
	Path             string           `json:"rel_path"`
	Content          string           `json:"content"`
	Definitions      []codeDefinition `json:"definitions"`
	Calls            []codeCall       `json:"calls"`
	Imports          []string         `json:"imports"`
	Embedding        []float64        `json:"embedding,omitempty"`
	EmbeddingServing string           `json:"embedding_serving,omitempty"`
	EmbeddingPolicy  string           `json:"embedding_policy,omitempty"`
}
type codeIndexRequest struct {
	FilePath         string     `json:"file_path"`
	LineStart        int        `json:"line_start"`
	LineEnd          int        `json:"line_end"`
	MaxLines         int        `json:"max_lines"`
	Route            string     `json:"route"`
	Project          string     `json:"project"`
	Root             string     `json:"root_path"`
	Phase            string     `json:"phase"`
	ScanID           string     `json:"scan_id"`
	ExpectedFiles    int        `json:"expected_files"`
	Files            []codeFile `json:"files"`
	Embedding        []float64  `json:"embedding"`
	EmbeddingServing string     `json:"embedding_serving"`
	EmbeddingPolicy  string     `json:"embedding_policy"`
}
type codeStore struct{ db store.Queryer }

func validCodePath(p string) bool {
	if p == "" || len(p) > 4096 || !utf8.ValidString(p) || strings.ContainsAny(p, "\\\x00\r\n") || path.IsAbs(p) || path.Clean(p) != p || p == ".." || strings.HasPrefix(p, "../") {
		return false
	}
	for i, c := range strings.Split(p, "/") {
		if strings.HasPrefix(c, ".") && !(i == len(strings.Split(p, "/"))-1 && c == ".gitmodules") {
			return false
		}
	}
	return true
}
func codeVector(v []float64) (string, error) {
	if len(v) < 1 || len(v) > 4000 {
		return "", errors.New("invalid code vector dimension")
	}
	norm := 0.0
	for _, x := range v {
		if math.IsNaN(x) || math.IsInf(x, 0) {
			return "", errors.New("nonfinite code vector")
		}
		norm += x * x
	}
	if norm == 0 || math.IsInf(norm, 0) {
		return "", errors.New("invalid code vector norm")
	}
	b, e := json.Marshal(v)
	return string(b), e
}
func runtimeCodeIndex(ctx context.Context, db store.DB, fields []string) (uint32, []string, error) {
	var r codeIndexRequest
	if len(fields[0]) > 16*1024*1024 || json.Unmarshal([]byte(fields[0]), &r) != nil {
		return store.StatusInvalid, nil, nil
	}
	route, err := url.ParseRequestURI(r.Route)
	if err != nil {
		return store.StatusInvalid, nil, nil
	}
	s := codeStore{db: db}
	var raw json.RawMessage
	if route.Path == "/v1/code/scan" {
		raw, err = s.publish(ctx, db, r)
	} else {
		raw, err = s.queryCode(ctx, route, r)
	}
	if err != nil {
		return store.StatusFailed, nil, err
	}
	return store.StatusOK, []string{string(raw)}, nil
}
func (s *codeStore) publish(ctx context.Context, db store.DB, r codeIndexRequest) (json.RawMessage, error) {
	if strings.TrimSpace(r.Project) == "" || strings.TrimSpace(r.Project) != r.Project || len(r.Project) > 127 || r.Root == "" || len(r.Root) > 4096 || len(r.ScanID) > 128 || r.ExpectedFiles < 0 || r.ExpectedFiles > 100000 {
		return nil, errors.New("invalid code scan identity")
	}
	for i := range r.Files {
		f := &r.Files[i]
		if !validCodePath(f.Path) || len(f.ModuleIdentity) > 4096 || len(f.Content) > 512*1024 || !utf8.ValidString(f.Content) || strings.ContainsRune(f.Content, 0) || len(f.Definitions) > 256 || len(f.Calls) > 512 || len(f.Imports) > 128 || len(f.EmbeddingServing) > 160 {
			return nil, errors.New("invalid code file")
		}
		if f.Definitions == nil {
			f.Definitions = []codeDefinition{}
		}
		if f.Calls == nil {
			f.Calls = []codeCall{}
		}
		if f.Imports == nil {
			f.Imports = []string{}
		}
		if len(f.Embedding) > 0 {
			if _, err := codeVector(f.Embedding); err != nil || f.EmbeddingServing == "" || f.EmbeddingPolicy != codeGenerationPolicy {
				return nil, errors.New("unqualified code embedding")
			}
		} else if f.EmbeddingServing != "" {
			return nil, errors.New("code embedding identity has no vector")
		}
	}
	if r.Phase == "" {
		r.ScanID = rand.Text()
	} else if r.ScanID == "" {
		return nil, errors.New("missing scan session")
	}
	tx, err := db.Begin(ctx)
	if err != nil {
		return nil, err
	}
	defer tx.Rollback(ctx)
	if r.Phase == "begin" || r.Phase == "" {
		_, err = tx.Exec(ctx, `INSERT INTO server_code_projects(name,root) VALUES($1,$2) ON CONFLICT(name) DO NOTHING`, r.Project, r.Root)
		if err != nil {
			return nil, err
		}
	}
	var active string
	var retired bool
	if err = tx.QueryRow(ctx, `SELECT scan_id,retired FROM server_code_projects WHERE name=$1 FOR UPDATE`, r.Project).Scan(&active, &retired); err != nil {
		return nil, err
	}
	if retired {
		return nil, errors.New("retired code project is unavailable")
	}
	if r.Phase == "begin" || r.Phase == "" {
		// A scan UUID is never reused or allowed to resurrect an old publication.
		if _, err = tx.Exec(ctx, `INSERT INTO server_code_scans(project,scan_id,state) VALUES($1,$2,'building')`, r.Project, r.ScanID); err != nil {
			return nil, err
		}
		if active != "" {
			if _, err = tx.Exec(ctx, `UPDATE server_code_scans SET state='superseded' WHERE project=$1 AND scan_id=$2 AND state='building'`, r.Project, active); err != nil {
				return nil, err
			}
		}
		if _, err = tx.Exec(ctx, `UPDATE server_code_projects SET scan_id=$2 WHERE name=$1`, r.Project, r.ScanID); err != nil {
			return nil, err
		}
		active = r.ScanID
	}
	if active != r.ScanID {
		return nil, errors.New("stale code scan session")
	}
	count := 0
	switch r.Phase {
	case "begin":
	case "stage", "":
		for _, f := range r.Files {
			body, e := json.Marshal(f)
			if e != nil {
				return nil, e
			}
			var vector any
			if len(f.Embedding) > 0 {
				vector, _ = codeVector(f.Embedding)
			}
			fingerprint := fmt.Sprintf("%x", sha256.Sum256([]byte(f.Content)))
			var accepted bool
			err = tx.QueryRow(ctx, `WITH accepted AS (INSERT INTO server_code_file_versions(project,scan_id,path,data,content,fingerprint,embedding,embedding_serving)
    VALUES($1,$2,$3,$4::jsonb,$5,$6,$7::vector,$8) ON CONFLICT(project,scan_id,path) DO NOTHING RETURNING 1)
    SELECT EXISTS(SELECT 1 FROM accepted) OR EXISTS(SELECT 1 FROM server_code_file_versions WHERE project=$1 AND scan_id=$2 AND path=$3 AND data=$4::jsonb)`, r.Project, r.ScanID, f.Path, string(body), f.Content, fingerprint, vector, f.EmbeddingServing).Scan(&accepted)
			if err != nil {
				return nil, err
			}
			if !accepted {
				return nil, errors.New("changed retry cannot overwrite staged source")
			}
			count++
		}
	case "seal":
		if err = tx.QueryRow(ctx, `SELECT count(*) FROM server_code_file_versions WHERE project=$1 AND scan_id=$2`, r.Project, r.ScanID).Scan(&count); err != nil {
			return nil, err
		}
		if count != r.ExpectedFiles {
			return nil, errors.New("code manifest count mismatch")
		}
	case "abort":
		if _, err = tx.Exec(ctx, `UPDATE server_code_scans SET state='aborted' WHERE project=$1 AND scan_id=$2`, r.Project, r.ScanID); err != nil {
			return nil, err
		}
	default:
		return nil, errors.New("unsupported code scan phase")
	}
	if r.Phase == "seal" || r.Phase == "" {
		if _, err = tx.Exec(ctx, `UPDATE server_code_projects SET root=$2,scanned_at=now(),generation=generation+1,current_scan=$3,scan_id='' WHERE name=$1`, r.Project, r.Root, r.ScanID); err != nil {
			return nil, err
		}
		if _, err = tx.Exec(ctx, `UPDATE server_code_scans SET state='ready' WHERE project=$1 AND scan_id=$2`, r.Project, r.ScanID); err != nil {
			return nil, err
		}
	} else if r.Phase == "abort" {
		if _, err = tx.Exec(ctx, `UPDATE server_code_projects SET scan_id='' WHERE name=$1`, r.Project); err != nil {
			return nil, err
		}
	}
	if err = tx.Commit(ctx); err != nil {
		return nil, err
	}
	return json.Marshal(map[string]any{"status": "ok", "project": r.Project, "files_scanned": count, "files": count, "inspected": count, "skipped": false, "reason": "indexed"})
}
func (*codeStore) graphFusionEnabled() bool { return codeFusionEnabled() }
