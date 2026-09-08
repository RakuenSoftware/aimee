package memory

import (
	"context"
	"encoding/json"
	"errors"
	"net/url"
	"strconv"
	"time"
)

func (s *postgresDataStore) codeJSON(ctx context.Context, sql string, args ...any) (json.RawMessage, error) {
	var raw string
	if err := s.db.QueryRow(ctx, sql, args...).Scan(&raw); err != nil {
		return nil, err
	}
	return json.RawMessage(raw), nil
}

func (s *postgresDataStore) queryCode(ctx context.Context, route *url.URL, req CodeIndexRequest) (json.RawMessage, error) {
	q := route.Query()
	project := q.Get("project")
	if project == "" {
		project = req.Project
	}
	if q.Get("scope") == "all" {
		project = ""
	}
	limit := 20
	if v := q.Get("max_results"); v != "" {
		var err error
		limit, err = strconv.Atoi(v)
		if err != nil || limit < 1 || limit > 1000 {
			return nil, errors.New("invalid code limit")
		}
	}
	if limit > 128 {
		limit = 128
	}
	switch route.Path {
	case "/v1/code/projects":
		return s.codeJSON(ctx, `SELECT json_build_object('projects',COALESCE(json_agg(p),'[]'::json))::text FROM
(SELECT name,root,scanned_at,generation FROM user_code_projects WHERE generation>0 ORDER BY name LIMIT $1) p`, limit)
	case "/v1/code/project/delete":
		_, err := s.db.Exec(ctx, `DELETE FROM user_code_projects WHERE name=$1`, project)
		return json.RawMessage(`{"status":"ok"}`), err
	case "/v1/code/project-stats":
		return s.codeJSON(ctx, `SELECT json_build_object('files',count(*),'definitions',COALESCE(sum(jsonb_array_length(definitions)),0),
'calls',COALESCE(sum(jsonb_array_length(calls)),0),'embeddings',count(*) FILTER(WHERE embedding IS NOT NULL AND embedding_fingerprint=fingerprint),'projects',count(DISTINCT project))::text
FROM user_code_files WHERE ($1='' OR project=$1)`, project)
	case "/v1/code/find":
		return s.codeJSON(ctx, `SELECT json_build_object('hits',COALESCE(json_agg(h),'[]'::json))::text FROM
(SELECT f.project,f.path AS file_path,(d->>'line')::int AS line,(d->>'line_end')::int AS line_end,
d->>'kind' AS kind,d->>'name' AS name FROM user_code_files f CROSS JOIN LATERAL jsonb_array_elements(f.definitions) d
WHERE ($1='' OR f.project=$1) AND lower(d->>'name')=lower($2) ORDER BY f.project,f.path,line LIMIT $3) h`, project, q.Get("identifier"), limit)
	case "/v1/code/callers":
		return s.codeJSON(ctx, `SELECT json_build_object('hits',COALESCE(json_agg(h),'[]'::json))::text FROM
(SELECT f.project,f.path AS file_path,c->>'caller' AS caller,(c->>'line')::int AS line
FROM user_code_files f CROSS JOIN LATERAL jsonb_array_elements(f.calls) c
WHERE ($1='' OR f.project=$1) AND c->>'callee'=$2 ORDER BY f.project,f.path,line LIMIT $3) h`, project, q.Get("symbol"), limit)
	case "/v1/code/structure":
		return s.codeJSON(ctx, `SELECT json_build_object('definitions',definitions,'imports',imports,'calls',calls)::text
FROM user_code_files WHERE project=$1 AND path=$2`, project, q.Get("file_path"))
	case "/v1/code/search", "/v1/code/hybrid", "/v1/code/context":
		return s.searchCode(ctx, project, q.Get("query"), limit)
	case "/v1/code/blast-radius":
		return s.blastCode(ctx, project, q.Get("file_path"))
	default:
		return nil, errors.New("unsupported private code query")
	}
}

// The same extracted definitions and calls serve callers, blast radius and
// retrieval expansion. Edges never join symbols across projects, and all hits
// carry the source file rather than inventing personal memories from code.
const codeCallEdges = `SELECT DISTINCT caller.project,caller.path AS source,callee.path AS target
FROM user_code_files caller CROSS JOIN LATERAL jsonb_array_elements(caller.calls) c
JOIN user_code_files callee ON callee.project=caller.project AND callee.path<>caller.path
CROSS JOIN LATERAL jsonb_array_elements(callee.definitions) d
WHERE c->>'callee'=d->>'name'
UNION
SELECT caller.project,caller.path,callee.path FROM user_code_files caller
CROSS JOIN LATERAL jsonb_array_elements_text(caller.imports) imp
JOIN user_code_files callee ON callee.project=caller.project AND callee.path<>caller.path
WHERE callee.module_identity<>'' AND (imp=callee.module_identity OR imp=callee.module_identity||'.__init__')`

func (s *postgresDataStore) searchCode(ctx context.Context, project, query string, limit int) (json.RawMessage, error) {
	vector, serving := "", ""
	if s.personal != nil && query != "" {
		bounded, cancel := context.WithTimeout(ctx, 1500*time.Millisecond)
		vector, serving, _ = s.personal.codeQueryVector(bounded, query)
		cancel()
	}

	return s.codeJSON(ctx, `WITH lexical AS MATERIALIZED (
 SELECT project,path,ts_rank_cd(to_tsvector('simple',path||' '||content),plainto_tsquery('simple',$2)) AS rank
 FROM user_code_files WHERE ($1='' OR project=$1) AND $2<>'' AND
 (to_tsvector('simple',path||' '||content) @@ plainto_tsquery('simple',$2)
 OR strpos(lower(path||' '||content),lower($2))>0)
 ORDER BY rank DESC,project,path LIMIT 32
), dense AS MATERIALIZED (
 SELECT project,path,1-(embedding <=> NULLIF($5,'')::vector) AS rank FROM user_code_files
 WHERE $5<>'' AND ($1='' OR project=$1) AND embedding_serving=$6 AND embedding_fingerprint=fingerprint
 AND CASE WHEN vector_dims(embedding)=vector_dims(NULLIF($5,'')::vector)
 THEN 1-(embedding <=> NULLIF($5,'')::vector)>0.3 ELSE false END
 ORDER BY rank DESC,project,path LIMIT 32
), seeds AS (SELECT * FROM lexical UNION ALL SELECT * FROM dense),
 edges AS (`+codeCallEdges+`), expanded AS (
 SELECT e.project,CASE WHEN e.source=l.path THEN e.target ELSE e.source END AS path,
 max(l.rank)*0.5 AS rank FROM seeds l JOIN edges e ON e.project=l.project AND (e.source=l.path OR e.target=l.path)
 WHERE $4 GROUP BY e.project,CASE WHEN e.source=l.path THEN e.target ELSE e.source END
), ranked AS (
 SELECT project,path,max(rank) AS rank,bool_or(direct) AS direct FROM
 (SELECT *,true AS direct FROM seeds UNION ALL SELECT *,false AS direct FROM expanded) c GROUP BY project,path
), hits AS (
 SELECT f.project,f.path AS file_path,left(f.content,2048) AS snippet,
 1 AS line,json_build_object('kind','file','line_start',1,'line_end',1) AS span,
 p.generation,'current' AS freshness,f.fingerprint AS content_hash,
 r.rank,r.rank AS score,1 AS signal_hits,
 json_build_array(CASE WHEN EXISTS(SELECT 1 FROM lexical l WHERE l.project=f.project AND l.path=f.path) THEN 'code'
 WHEN EXISTS(SELECT 1 FROM dense d WHERE d.project=f.project AND d.path=f.path) THEN 'vector' ELSE 'graph' END) AS signals,
 CASE WHEN EXISTS(SELECT 1 FROM lexical l WHERE l.project=f.project AND l.path=f.path) THEN 'code'
 WHEN EXISTS(SELECT 1 FROM dense d WHERE d.project=f.project AND d.path=f.path) THEN 'vector' ELSE 'graph' END AS source
 FROM ranked r JOIN user_code_files f USING(project,path) JOIN user_code_projects p ON p.name=f.project
 ORDER BY r.direct DESC,r.rank DESC,f.project,f.path LIMIT $3
) SELECT json_build_object('hits',COALESCE(json_agg(hits),'[]'::json),'results',COALESCE(json_agg(hits),'[]'::json),'status','ok','graph_code_fusion_state',CASE WHEN $4 THEN 'on' ELSE 'off' END)::text FROM hits`, project, query, limit, s.graphFusionEnabled(), vector, serving)
}

func (s *postgresDataStore) blastCode(ctx context.Context, project, file string) (json.RawMessage, error) {
	return s.codeJSON(ctx, `WITH edges AS (`+codeCallEdges+`)
SELECT json_build_object('file',$2::text,'project',p.name,'generation',p.generation,'freshness','current','resolved',true,
'dependency_edges',COALESCE((SELECT json_agg(json_build_object('identity',e.target,'provenance','code_structure','confidence','structural','project',p.name,'generation',p.generation,'freshness','current')) FROM edges e WHERE e.project=p.name AND e.source=$2),'[]'::json),
'dependent_edges',COALESCE((SELECT json_agg(json_build_object('path',e.source,'provenance','code_structure','confidence','structural','project',p.name,'generation',p.generation,'freshness','current')) FROM edges e WHERE e.project=p.name AND e.target=$2),'[]'::json),
'dependents',COALESCE((SELECT json_agg(e.source) FROM edges e WHERE e.project=p.name AND e.target=$2),'[]'::json),
'dependencies',COALESCE((SELECT json_agg(e.target) FROM edges e WHERE e.project=p.name AND e.source=$2),'[]'::json))::text
FROM user_code_projects p JOIN user_code_files f ON f.project=p.name WHERE p.name=$1 AND f.path=$2`, project, file)
}
