package memory

import (
	"context"
	"strings"
)

// These predicates run before LIMIT: unrelated documents cannot exhaust the
// candidate window. The same path grammar covers every legacy convention name.
const conventionSourceSQL = `(lower(regexp_replace(d.file_path,'^.*/','')) IN
 ('contributing.md','contributing.rst','agents.md','style.md','styleguide.md','coding.md','code_style.md','coding_standards.md','.aimee-rules','aimee-rules.md')
 OR lower(d.file_path) LIKE '%.aimee/rules.md' OR lower(d.file_path) LIKE '%.aimee/context.md'
 OR lower(d.file_path) LIKE '%/adr/%' OR lower(regexp_replace(d.file_path,'^.*/','')) LIKE 'adr-%')`

type conventionSource struct{ project, path, heading, content string }

func conventionSentence(content string) string {
	content = strings.TrimLeft(content, " \t\n-*#")
	end := len(content)
	for i := 0; i < len(content); i++ {
		if content[i] == '\n' && (i+1 == len(content) || content[i+1] == '\n') {
			end = i
			break
		}
		if content[i] == '.' && (i+1 == len(content) || content[i+1] == ' ' || content[i+1] == '\n') {
			end = i + 1
			break
		}
	}
	return textBound(content[:end], 200)
}

func conventionKey(source conventionSource, sentence string) string {
	base := source.path[strings.LastIndexByte(source.path, '/')+1:]
	suffix := textBound(source.heading, 128)
	if suffix == "" {
		suffix = textBound(sentence, 100)
	}
	key := "convention_" + textBound(source.project, 32) + "_" + textBound(base, 64) + "_" + suffix
	return strings.NewReplacer(" ", "_", "\t", "_", ":", "_", "/", "_", "#", "_").Replace(key)
}

// The caller supplies a transaction and its scope context. There is no automatic
// maintenance scheduling: this preserves the former explicit extraction API.
func (s *postgresDataStore) extractConventions(ctx context.Context, request DataRequest) (int, error) {
	if _, err := s.db.Exec(ctx, `SELECT pg_advisory_xact_lock(hashtextextended('memory:conventions',0))`); err != nil {
		return 0, err
	}
	rows, err := s.db.Query(ctx, `SELECT d.project,d.file_path,d.heading_path,left(d.content,8191)
 FROM kb_documents d JOIN projects p ON p.name=d.project
 WHERE p.lifecycle_state='current' AND d.generation=p.current_generation
 AND lower(d.doc_kind)<>'pdf' AND d.project<>'' AND d.file_path<>'' AND d.content<>''
 AND ($1 OR d.project=$2) AND `+conventionSourceSQL+`
 ORDER BY d.project,d.file_path,d.chunk_index,d.id LIMIT 500`, request.IncludeAll, request.Project)
	if err != nil {
		return 0, err
	}
	sources := []conventionSource{}
	for rows.Next() {
		var source conventionSource
		if err = rows.Scan(&source.project, &source.path, &source.heading, &source.content); err != nil {
			rows.Close()
			return 0, err
		}
		sources = append(sources, source)
	}
	err = rows.Err()
	rows.Close()
	if err != nil {
		return 0, err
	}
	emitted := 0
	for _, source := range sources {
		sentence := conventionSentence(source.content)
		if len(sentence) < 10 {
			continue
		}
		key := conventionKey(source, sentence)
		var exists bool
		// Old native insertions may have normalized their key. Preserve both forms,
		// and never let another project's similarly named convention suppress this one.
		err = s.db.QueryRow(ctx, `SELECT EXISTS(SELECT 1 FROM memories WHERE key=ANY($1::text[])
 AND tier IN ('L3','L4') AND scope_type='project' AND scope_value=$2)`, []string{key, derivedNormalize(key)}, source.project).Scan(&exists)
		if err != nil {
			return 0, err
		}
		if exists {
			continue
		}
		confidence := .6
		record, err := s.InsertEpistemic(ctx, DataRequest{Scope: Scope{Type: ScopeProject, Value: source.project}, Tier: "L3", Kind: "fact", EpistemicKind: "world_fact", Key: key, Content: sentence, Confidence: &confidence, Authority: AuthorityModel})
		if err != nil {
			return 0, err
		}
		if err = s.captureStoredFactActor(ctx, record.ID, AuthorityModel, nil); err != nil {
			return 0, err
		}
		emitted++
	}
	return emitted, nil
}
