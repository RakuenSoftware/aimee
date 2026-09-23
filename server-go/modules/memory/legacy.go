package memory

import (
	"bufio"
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"regexp"
	"strconv"
	"strings"
)

// LegacySearchResult is the compatibility shape for the retired DB1
// conversation-window endpoint. Its backing data is now the shared memory
// corpus, so callers keep a useful endpoint without reviving a second store.
type LegacySearchResult struct {
	SessionID string   `json:"session_id"`
	Seq       int      `json:"seq"`
	FilePath  string   `json:"file_path"`
	StartLine int      `json:"start_line"`
	EndLine   int      `json:"end_line"`
	Summary   string   `json:"summary"`
	Score     float64  `json:"score"`
	Files     []string `json:"files"`
}

type VectorHit struct {
	ID    int64   `json:"id"`
	Score float64 `json:"score"`
}

type DriftResult struct {
	Drifted   bool   `json:"drifted"`
	TaskID    int64  `json:"task_id"`
	TaskTitle string `json:"task_title"`
	Message   string `json:"message"`
}

func (s *postgresDataStore) LegacySearch(ctx context.Context, clusters []string, limit int) ([]LegacySearchResult, error) {
	query := strings.TrimSpace(strings.Join(clusters, " "))
	if limit <= 0 || limit > 64 {
		limit = 10
	}
	rows, err := s.db.Query(ctx, `SELECT COALESCE(source_session,''),0,COALESCE(artifact_ref,''),0,0,
 content,confidence FROM memories
WHERE `+currentMemorySQL("")+` AND ($1='' OR
 to_tsvector('simple',key||' '||content) @@ plainto_tsquery('simple',$1))
ORDER BY CASE WHEN $1='' THEN 0 ELSE ts_rank_cd(to_tsvector('simple',key||' '||content),
 plainto_tsquery('simple',$1)) END DESC, confidence DESC, id DESC LIMIT $2`, query, limit)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var out []LegacySearchResult
	for rows.Next() {
		var item LegacySearchResult
		if err := rows.Scan(&item.SessionID, &item.Seq, &item.FilePath, &item.StartLine,
			&item.EndLine, &item.Summary, &item.Score); err != nil {
			return nil, err
		}
		if item.FilePath != "" {
			item.Files = []string{item.FilePath}
		}
		out = append(out, item)
	}
	return out, rows.Err()
}

func (s *postgresDataStore) CompactLegacy(ctx context.Context) (int, int, error) {
	if s.placement != PlacementKB {
		return 0, 0, errors.New("memory: compaction belongs to KB placement")
	}
	rows, err := s.db.Query(ctx, `SELECT source_session FROM memories
WHERE tier='L0' AND lifecycle_state='active' AND COALESCE(source_session,'')<>''
  AND aimee_utc_text_timestamptz(created_at) < now()-interval '30 days'
GROUP BY source_session HAVING count(*) BETWEEN 1 AND 64 ORDER BY min(id) LIMIT 64`)
	if err != nil {
		return 0, 0, err
	}
	var sessions []string
	for rows.Next() {
		var session string
		if err := rows.Scan(&session); err != nil {
			rows.Close()
			return 0, 0, err
		}
		sessions = append(sessions, session)
	}
	err = rows.Err()
	rows.Close()
	if err != nil {
		return 0, 0, err
	}
	summaries, facts := 0, 0
	for _, session := range sessions {
		folded, _, foldErr := s.FoldSession(ctx, session)
		if foldErr != nil {
			return summaries, facts, foldErr
		}
		if folded > 0 {
			summaries++
			facts += folded
		}
	}
	return summaries, facts, nil
}

func conversationText(value any) (session, text string) {
	object, ok := value.(map[string]any)
	if !ok {
		return "", ""
	}
	for _, key := range []string{"session_id", "session", "conversation_id", "id"} {
		if v, ok := object[key].(string); ok && v != "" {
			session = v
			break
		}
	}
	for _, key := range []string{"content", "text", "message", "prompt", "response"} {
		switch v := object[key].(type) {
		case string:
			if strings.TrimSpace(v) != "" {
				return session, v
			}
		case map[string]any:
			if nested, ok := v["content"].(string); ok && strings.TrimSpace(nested) != "" {
				return session, nested
			}
		}
	}
	return session, ""
}

func (s *postgresDataStore) ScanConversations(ctx context.Context, directories []string) (int, error) {
	if s.placement != PlacementKB {
		return 0, errors.New("memory: conversation scan belongs to KB placement")
	}
	inserted := 0
	for _, directory := range directories {
		if inserted >= 10000 {
			break
		}
		err := filepath.WalkDir(directory, func(path string, entry os.DirEntry, walkErr error) error {
			if walkErr != nil || entry.IsDir() || inserted >= 10000 {
				return walkErr
			}
			ext := strings.ToLower(filepath.Ext(path))
			if ext != ".jsonl" && ext != ".json" {
				return nil
			}
			file, err := os.Open(path)
			if err != nil {
				return err
			}
			scanErr := func() error {
				defer file.Close()
				reader := bufio.NewReader(io.LimitReader(file, 16<<20))
				lineNo := 0
				for inserted < 10000 {
					line, readErr := reader.ReadBytes('\n')
					if len(line) > 0 {
						lineNo++
						var value any
						if json.Unmarshal(line, &value) == nil {
							session, content := conversationText(value)
							if content != "" {
								digest := sha256.Sum256([]byte(path + ":" + strconv.Itoa(lineNo)))
								key := "conversation:" + hex.EncodeToString(digest[:12])
								_, err = s.Put(ctx, Scope{Type: ScopeGlobal, Value: "_global"}, Record{
									Tier: "L0", Kind: "episode", Key: key, Content: content, Confidence: 0.7,
								})
								if err != nil {
									return err
								}
								if session != "" {
									_, err = s.db.Exec(ctx, `UPDATE memories SET source_session=$1,artifact_type='conversation',artifact_ref=$2
WHERE key=$3 AND scope_type='global' AND scope_value='_global'`, session, path, key)
									if err != nil {
										return err
									}
								}
								inserted++
							}
						}
					}
					if readErr == io.EOF {
						break
					}
					if readErr != nil {
						return readErr
					}
				}
				return nil
			}()
			return scanErr
		})
		if err != nil {
			return inserted, err
		}
	}
	return inserted, nil
}

var driftWords = regexp.MustCompile(`[a-z0-9_./-]+`)

func driftTokens(text string) []string {
	words := driftWords.FindAllString(strings.ToLower(text), -1)
	out := words[:0]
	for _, word := range words {
		if len(word) >= 3 && !strings.Contains(" the and for with from into task this that ", " "+word+" ") {
			out = append(out, word)
		}
	}
	return out
}

func (s *postgresDataStore) CheckDrift(ctx context.Context, taskID int64, filePath, command string) (DriftResult, error) {
	var title string
	if err := s.db.QueryRow(ctx, `SELECT title FROM tasks WHERE id=$1`, taskID).Scan(&title); err != nil {
		return DriftResult{}, err
	}
	rows, err := s.db.Query(ctx, `SELECT title FROM tasks WHERE parent_id=$1 ORDER BY id LIMIT 32`, taskID)
	if err != nil {
		return DriftResult{}, err
	}
	terms := driftTokens(title)
	for rows.Next() {
		var sub string
		if err := rows.Scan(&sub); err != nil {
			rows.Close()
			return DriftResult{}, err
		}
		terms = append(terms, driftTokens(sub)...)
	}
	err = rows.Err()
	rows.Close()
	if err != nil {
		return DriftResult{}, err
	}
	target := strings.ToLower(filePath + " " + command)
	inScope := filePath == "" && command == ""
	for _, term := range terms {
		if strings.Contains(target, term) {
			inScope = true
			break
		}
	}
	result := DriftResult{TaskID: taskID, TaskTitle: title, Drifted: !inScope}
	if result.Drifted {
		result.Message = "The requested file or command does not overlap the active task scope: " + title
	} else {
		result.Message = "Action overlaps the active task scope"
	}
	return result, nil
}

func (s *postgresDataStore) ExtractAntiPatterns(ctx context.Context, source string) (int, error) {
	var count int
	var query string
	switch source {
	case "feedback":
		query = `WITH inserted AS (
 INSERT INTO anti_patterns(pattern,description,source,source_ref,confidence)
 SELECT r.title,r.description,'feedback','rule:'||r.id::text,0.8 FROM rules r
 WHERE r.polarity='negative' AND r.title<>'' AND ` + memoryUnexpiredAtSQL("r.expires_at", "CURRENT_TIMESTAMP") + ` AND NOT EXISTS
  (SELECT 1 FROM anti_patterns a WHERE a.source_ref='rule:'||r.id::text)
 RETURNING 1) SELECT count(*) FROM inserted`
	case "failure":
		query = `WITH inserted AS (
 INSERT INTO anti_patterns(pattern,description,source,source_ref,confidence)
 SELECT d.chosen,COALESCE(NULLIF(d.rationale,''),d.options),'failure','decision:'||d.id::text,0.75
 FROM decision_log d WHERE d.outcome='failure' AND d.chosen<>'' AND NOT EXISTS
  (SELECT 1 FROM anti_patterns a WHERE a.source_ref='decision:'||d.id::text)
 RETURNING 1) SELECT count(*) FROM inserted`
	default:
		return 0, errors.New("memory: unknown anti-pattern source")
	}
	err := s.db.QueryRow(ctx, query).Scan(&count)
	return count, err
}

func (s *postgresDataStore) EscalateAntiPatterns(ctx context.Context, threshold int) (int, error) {
	if threshold <= 0 {
		threshold = 5
	}
	var count int
	err := s.db.QueryRow(ctx, `WITH inserted AS (
 INSERT INTO rules(polarity,title,description,weight,domain,directive_type,created_at,updated_at)
 SELECT 'negative',a.pattern,a.description,10,'anti-pattern','hard',pg_now_text(),pg_now_text()
 FROM anti_patterns a WHERE a.hit_count >= $1 AND NOT EXISTS
  (SELECT 1 FROM rules r WHERE r.polarity='negative' AND r.title=a.pattern AND r.directive_type='hard')
 RETURNING 1) SELECT count(*) FROM inserted`, threshold).Scan(&count)
	return count, err
}

type styleDimension struct {
	key, negativePreference, positivePreference string
	negative, positive                          []string
}

var styleDimensions = []styleDimension{
	{"style_verbosity", "User prefers concise, non-verbose output", "User explicitly prefers brief responses",
		[]string{"verbose", "wordy", "lengthy", "too long", "wall of text"}, []string{"concise", "brief", "terse", "succinct", "compact"}},
	{"style_explanations", "User prefers minimal explanations; avoid stating the obvious", "User values detailed explanations with reasoning",
		[]string{"obvious", "unnecessary", "over-explain", "stop explaining"}, []string{"explain", "reasoning", "context", "more detail", "elaborate"}},
	{"style_commit_style", "User wants clear, descriptive commit messages", "User follows conventional commit conventions",
		[]string{"vague commit", "bad message", "unclear commit"}, []string{"conventional", "semantic commit", "good commit"}},
}

func containsAny(text string, values []string) bool {
	for _, value := range values {
		if strings.Contains(text, value) {
			return true
		}
	}
	return false
}

func (s *postgresDataStore) LearnStyle(ctx context.Context) (int, error) {
	rows, err := s.db.Query(ctx, `SELECT polarity,description FROM rules
WHERE polarity IN ('positive','negative') AND `+memoryUnexpiredAtSQL("expires_at", "CURRENT_TIMESTAMP")+` ORDER BY id DESC LIMIT 256`)
	if err != nil {
		return 0, err
	}
	positive := make([]int, len(styleDimensions))
	negative := make([]int, len(styleDimensions))
	for rows.Next() {
		var polarity, description string
		if err := rows.Scan(&polarity, &description); err != nil {
			rows.Close()
			return 0, err
		}
		description = strings.ToLower(description)
		for i, dimension := range styleDimensions {
			if polarity == "positive" && containsAny(description, dimension.positive) {
				positive[i]++
			}
			if polarity == "negative" && containsAny(description, dimension.negative) {
				negative[i]++
			}
		}
	}
	if err := rows.Err(); err != nil {
		rows.Close()
		return 0, err
	}
	rows.Close()
	learned := 0
	for i, dimension := range styleDimensions {
		content := ""
		if negative[i] >= 2 && negative[i] >= positive[i] {
			content = dimension.negativePreference
		} else if positive[i] >= 2 {
			content = dimension.positivePreference
		}
		if content == "" {
			continue
		}
		if _, err := s.Put(ctx, Scope{Type: ScopeGlobal, Value: "_global"}, Record{
			Tier: "L1", Kind: "preference", Key: dimension.key, Content: content, Confidence: 0.8,
		}); err != nil {
			return learned, err
		}
		learned++
	}
	return learned, nil
}

var errEpisodeMixedScope = errors.New("memory: episode sources have incompatible scopes; select one scope")

func (s *postgresDataStore) GenerateEpisodeCard(ctx context.Context, session string) (int64, error) {
	if strings.TrimSpace(session) == "" {
		return 0, errors.New("memory: episode card needs a session")
	}
	command, err := s.episodeCognifier()
	if err != nil {
		return 0, err
	}
	rows, err := s.db.Query(ctx, `SELECT id,key,content,scope_type,scope_value FROM memories m
WHERE source_session=$1 AND lifecycle_state='active'
 AND NOT EXISTS (SELECT 1 FROM memory_units u WHERE u.memory_id=m.id AND u.is_episode_card=1)
ORDER BY id LIMIT 201`, session)
	if err != nil {
		return 0, err
	}
	type source struct {
		id           int64
		key, content string
		scope        Scope
	}
	var sources []source
	for rows.Next() {
		var item source
		if err := rows.Scan(&item.id, &item.key, &item.content, &item.scope.Type, &item.scope.Value); err != nil {
			rows.Close()
			return 0, err
		}
		sources = append(sources, item)
	}
	err = rows.Err()
	rows.Close()
	if err != nil {
		return 0, err
	}
	if len(sources) > episodeMaxSources {
		return 0, errEpisodeCapacity
	}
	if len(sources) == 0 {
		return 0, ErrMemoryNotFound
	}
	// Shared/global inputs may contribute to a private card. Different private
	// scopes cannot be combined into one card without widening disclosure.
	scope := Scope{Type: ScopeGlobal, Value: "_global"}
	for _, item := range sources {
		if item.scope.Type == ScopeGlobal {
			continue
		}
		if scope.Type != ScopeGlobal && scope != item.scope {
			return 0, errEpisodeMixedScope
		}
		scope = item.scope
	}
	turns := make([]episodeTurn, 0, len(sources))
	size := 0
	for _, item := range sources {
		size += len(item.content)
		if size > episodeMaxInput {
			return 0, errEpisodeCapacity
		}
		turns = append(turns, episodeTurn{ID: item.id, Text: item.content})
	}
	input, err := json.Marshal(map[string]any{"task": "episode_card", "session_id": session, "turns": turns})
	if err != nil || len(input) > episodeMaxInput {
		return 0, errEpisodeCapacity
	}
	run := s.episodeCommand
	if run == nil {
		run = runEpisodeCommand
	}
	raw, err := run(ctx, command, input)
	if err != nil {
		return 0, err
	}
	card, err := parseEpisodeCard(raw)
	if err != nil {
		return 0, err
	}
	var unitID int64
	var created bool
	err = s.db.QueryRow(ctx, `WITH existing AS (
 SELECT u.id FROM memory_units u JOIN memories m ON m.id=u.memory_id
 WHERE m.key=$1 AND m.source_session=$3 AND m.lifecycle_state='active'
   AND u.unit_type='episode_card' AND u.unit_key=$3 AND u.is_episode_card=1
 AND m.scope_type=$4 AND m.scope_value=$5
 ORDER BY u.id LIMIT 1
), parent AS (
 INSERT INTO memories(tier,kind,epistemic_kind,key,content,confidence,source_session,scope_type,scope_value,lifecycle_state)
 SELECT 'L1','episode','episode',$1,$2,0.8,$3,$4,$5,'active'
 WHERE NOT EXISTS(SELECT 1 FROM existing) RETURNING id
), created AS (
 INSERT INTO memory_units(memory_id,unit_type,unit_key,unit_text,weight,memory_kind,is_episode_card)
 SELECT id,'episode_card',$3,$2,1.0,'episodic',1 FROM parent RETURNING id
)
SELECT id,false FROM existing UNION ALL SELECT id,true FROM created LIMIT 1`,
		"episode-card:"+session, card.text(), session, scope.Type, scope.Value).Scan(&unitID, &created)
	if err != nil {
		return 0, err
	}
	if !created {
		return unitID, nil
	}
	for _, item := range sources {
		_, err = s.db.Exec(ctx, `INSERT INTO memory_lineage(object_type,object_id,source_kind,source_ref,confidence)
SELECT 'memory_unit',$1,'memory',$2,0.8 WHERE NOT EXISTS(
 SELECT 1 FROM memory_lineage WHERE object_type='memory_unit' AND object_id=$1
   AND source_kind='memory' AND source_ref=$2)`, unitID, fmt.Sprintf("memory:%d", item.id))
		if err != nil {
			return 0, err
		}
		if _, err := s.db.Exec(ctx, `INSERT INTO memory_lineage(object_type,object_id,source_kind,source_ref,confidence)
 SELECT 'memory',u.memory_id,'memory',$2,0.8 FROM memory_units u WHERE u.id=$1
 AND NOT EXISTS(SELECT 1 FROM memory_lineage l WHERE l.object_type='memory' AND l.object_id=u.memory_id AND l.source_kind='memory' AND l.source_ref=$2)`, unitID, fmt.Sprintf("memory:%d", item.id)); err != nil {
			return 0, err
		}
		if _, err := s.db.Exec(ctx, `INSERT INTO memory_relations(memory_id,src_entity,relation,dst_entity,fact_text)
 SELECT m.id,m.key,'REL_SUMMARISES',$2,$3 FROM memories m JOIN memory_units u ON u.memory_id=m.id WHERE u.id=$1
 AND NOT EXISTS(SELECT 1 FROM memory_relations r WHERE r.memory_id=m.id AND r.relation='REL_SUMMARISES' AND r.dst_entity=$2)`, unitID, fmt.Sprintf("memory:%d", item.id), card.Title); err != nil {
			return 0, err
		}
	}
	return unitID, nil
}

func vectorText(vector []float64) (string, error) {
	if len(vector) == 0 || len(vector) > 4000 {
		return "", errors.New("memory: invalid vector dimensions")
	}
	parts := make([]string, len(vector))
	for i, value := range vector {
		parts[i] = strconv.FormatFloat(value, 'g', -1, 64)
	}
	return "[" + strings.Join(parts, ",") + "]", nil
}

func (s *postgresDataStore) SearchVectors(ctx context.Context, vector []float64, recordType,
	workspace, project string, includeAll bool, limit int) ([]VectorHit, error) {
	return s.searchVectors(ctx, vector, recordType, workspace, project, includeAll, limit, Scope{})
}

func (s *postgresDataStore) searchVectors(ctx context.Context, vector []float64, recordType,
	workspace, project string, includeAll bool, limit int, exact Scope) ([]VectorHit, error) {
	encoded, err := vectorText(vector)
	if err != nil {
		return nil, err
	}
	if recordType == "" || limit < 1 || limit > 256 {
		return nil, errors.New("memory: invalid vector search")
	}
	rows, err := s.db.Query(ctx, `SELECT e.point_id,1-(e.embedding <=> $1::vector) AS score
FROM memory_embeddings e WHERE e.record_type=$2
 AND (e.record_type NOT IN ('memory','unit') OR EXISTS(SELECT 1 FROM memories m WHERE m.id=CASE e.record_type
   WHEN 'memory' THEN e.point_id WHEN 'unit' THEN
    (SELECT u.memory_id FROM memory_units u WHERE u.id=e.point_id-1000000000000 AND `+currentUnitInputsSQL("u")+`) END
   AND `+currentMemorySQL("m.")+`
   AND m.scope_type=e.primary_scope AND
    ((m.scope_type='global' AND m.scope_value='_global') OR
     (m.scope_type='workspace' AND m.scope_value=e.workspace) OR
     (m.scope_type='project' AND m.scope_value=e.project))))
 AND ($5 OR
 e.primary_scope='global' OR e.workspace='_shared' OR ($3<>'' AND e.workspace=$3) OR
 ($4<>'' AND e.project=$4))
 AND ($7='' OR (e.primary_scope=$7 AND
 (($7='global' AND $8='_global') OR ($7='workspace' AND e.workspace=$8) OR ($7='project' AND e.project=$8))))
 ORDER BY e.embedding <=> $1::vector,e.point_id LIMIT $6`,
		encoded, recordType, workspace, project, includeAll, limit, exact.Type, exact.Value)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var hits []VectorHit
	for rows.Next() {
		var hit VectorHit
		if err := rows.Scan(&hit.ID, &hit.Score); err != nil {
			return nil, err
		}
		hits = append(hits, hit)
	}
	return hits, rows.Err()
}

func (s *postgresDataStore) FailedEmbeddingIDs(ctx context.Context, limit int) ([]int64, error) {
	if limit <= 0 || limit > 256 {
		limit = 256
	}
	rows, err := s.db.Query(ctx, `SELECT v.point_id FROM vector_index_ops v JOIN memories m ON m.id=v.memory_id
WHERE v.status='failed' AND v.attempts < $2 AND m.lifecycle_state='active' ORDER BY v.point_id LIMIT $1`, limit, vectorRetryLimit())
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var ids []int64
	for rows.Next() {
		var id int64
		if err := rows.Scan(&id); err != nil {
			return nil, err
		}
		ids = append(ids, id)
	}
	return ids, rows.Err()
}

func (s *postgresDataStore) MarkEmbeddingFailure(ctx context.Context, id int64, detail string) error {
	if s.placement != PlacementKB {
		return errors.New("memory: vector maintenance belongs to KB placement")
	}
	if len(detail) > 1024 {
		detail = textBound(detail, 1024)
	}
	// Backoff starts when the attempt fails, even after a long transaction.
	_, err := s.db.Exec(ctx, `INSERT INTO vector_index_ops(point_id,collection,memory_id,status,attempts,last_error,updated_at)
SELECT $1,'memory',id,'failed',1,$2,clock_timestamp()::text FROM memories WHERE id=CASE WHEN $1::bigint >= $3::bigint
 THEN (SELECT memory_id FROM memory_units WHERE id=$1::bigint-$3::bigint) ELSE $1::bigint END AND lifecycle_state='active'
 ON CONFLICT(point_id) DO UPDATE SET
status='failed',attempts=vector_index_ops.attempts+1,last_error=EXCLUDED.last_error,updated_at=clock_timestamp()::text`, id, detail, unitPointOffset)
	return err
}
