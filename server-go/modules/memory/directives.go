package memory

import (
	"context"
	"errors"
	"time"

	store "github.com/JBailes/aimee/server-go/modules/aimee"
)

type Directive struct {
	ID                 int64  `json:"id"`
	Question           string `json:"question"`
	Topic              string `json:"topic"`
	AnchorEntity       string `json:"anchor_entity"`
	AnchorFile         string `json:"anchor_file"`
	Cause              string `json:"cause"`
	Priority           int    `json:"priority"`
	State              string `json:"state"`
	MemoryAID          int64  `json:"memory_a_id"`
	MemoryBID          int64  `json:"memory_b_id"`
	ResolutionMemoryID int64  `json:"resolution_memory_id"`
	Evidence           string `json:"evidence"`
	SourceSession      string `json:"source_session"`
	SurfacedCount      int    `json:"surfaced_count"`
	LastSurfacedAt     string `json:"last_surfaced_at"`
	ResolvedAt         string `json:"resolved_at"`
	ValidUntil         string `json:"valid_until"`
	CreatedAt          string `json:"created_at"`
	UpdatedAt          string `json:"updated_at"`
}

type DirectiveCounts struct {
	Open       int `json:"open"`
	Suppressed int `json:"suppressed"`
	Resolved   int `json:"resolved"`
	Expired    int `json:"expired"`
}

const directiveColumns = `id, question, topic, anchor_entity, anchor_file, cause, priority,
state, memory_a_id, memory_b_id, resolution_memory_id, evidence, source_session, surfaced_count,
last_surfaced_at, resolved_at, valid_until, created_at, updated_at`

func scanDirective(row store.Row, out *Directive) error {
	return row.Scan(&out.ID, &out.Question, &out.Topic, &out.AnchorEntity, &out.AnchorFile,
		&out.Cause, &out.Priority, &out.State, &out.MemoryAID, &out.MemoryBID,
		&out.ResolutionMemoryID, &out.Evidence, &out.SourceSession, &out.SurfacedCount,
		&out.LastSurfacedAt, &out.ResolvedAt, &out.ValidUntil, &out.CreatedAt, &out.UpdatedAt)
}

func scanDirectiveRows(rows store.Rows) ([]Directive, error) {
	defer rows.Close()
	items := make([]Directive, 0)
	for rows.Next() {
		var item Directive
		if err := rows.Scan(&item.ID, &item.Question, &item.Topic, &item.AnchorEntity,
			&item.AnchorFile, &item.Cause, &item.Priority, &item.State, &item.MemoryAID,
			&item.MemoryBID, &item.ResolutionMemoryID, &item.Evidence, &item.SourceSession,
			&item.SurfacedCount, &item.LastSurfacedAt, &item.ResolvedAt, &item.ValidUntil,
			&item.CreatedAt, &item.UpdatedAt); err != nil {
			return nil, err
		}
		items = append(items, item)
	}
	return items, rows.Err()
}

func (s *postgresDataStore) requireKBDirective() error {
	if s.placement != PlacementKB {
		return errors.New("memory: directives belong to kb placement")
	}
	return nil
}

func validDirectiveCause(cause string) bool {
	switch cause {
	case "contradiction", "retrieval_failure", "missing_config", "user_follow_up":
		return true
	}
	return false
}

func validDirectiveState(state string) bool {
	switch state {
	case "", "open", "suppressed", "resolved", "expired":
		return true
	}
	return false
}

func (s *postgresDataStore) DirectiveCreate(ctx context.Context, request DataRequest) (Directive, error) {
	if err := s.requireKBDirective(); err != nil {
		return Directive{}, err
	}
	if !validDirectiveCause(request.Cause) || request.Priority < 0 || request.Priority > 100 {
		return Directive{}, errors.New("memory: invalid directive")
	}
	priority := request.Priority
	if priority == 0 {
		priority = 50
	}
	var item Directive
	err := scanDirective(s.db.QueryRow(ctx, `INSERT INTO epistemic_directives
(question,topic,anchor_entity,anchor_file,cause,priority,memory_a_id,memory_b_id,evidence,
 source_session,valid_until)
VALUES ($1,$2,$3,$4,$5,$6,$7,$8,$9,$10,$11)
ON CONFLICT DO NOTHING RETURNING `+directiveColumns,
		request.Question, request.Topic, request.AnchorEntity, request.AnchorFile, request.Cause,
		priority, request.MemoryAID, request.MemoryBID, request.Evidence, request.SessionID,
		request.ValidUntil), &item)
	if store.IsNoRows(err) {
		err = scanDirective(s.db.QueryRow(ctx, `SELECT `+directiveColumns+` FROM epistemic_directives
WHERE (cause='contradiction' AND memory_a_id=$1 AND memory_b_id=$2) OR
      (cause=$3 AND topic=$4 AND $3 IN ('retrieval_failure','missing_config'))
ORDER BY id DESC LIMIT 1`, request.MemoryAID, request.MemoryBID, request.Cause, request.Topic), &item)
	}
	if err == nil {
		runtimeMetricState.directiveCreated.Add(1)
	}
	return item, err
}

func (s *postgresDataStore) DirectiveList(ctx context.Context, state, cause string, limit int) ([]Directive, error) {
	if err := s.requireKBDirective(); err != nil {
		return nil, err
	}
	if !validDirectiveState(state) || (cause != "" && !validDirectiveCause(cause)) {
		return nil, errors.New("memory: invalid directive filter")
	}
	rows, err := s.db.Query(ctx, `SELECT `+directiveColumns+` FROM epistemic_directives
WHERE ($1='' OR state=$1) AND ($2='' OR cause=$2)
ORDER BY priority DESC, created_at DESC, id DESC LIMIT $3`, state, cause, limit)
	if err != nil {
		return nil, err
	}
	return scanDirectiveRows(rows)
}

func (s *postgresDataStore) DirectiveGet(ctx context.Context, id int64) (Directive, error) {
	if err := s.requireKBDirective(); err != nil {
		return Directive{}, err
	}
	var item Directive
	err := scanDirective(s.db.QueryRow(ctx, `SELECT `+directiveColumns+`
FROM epistemic_directives WHERE id=$1`, id), &item)
	if store.IsNoRows(err) {
		return Directive{}, ErrMemoryNotFound
	}
	return item, err
}

func (s *postgresDataStore) DirectiveResolve(ctx context.Context, id, resolutionID int64, note string) (bool, error) {
	if err := s.requireKBDirective(); err != nil {
		return false, err
	}
	tag, err := s.db.Exec(ctx, `UPDATE epistemic_directives SET state='resolved',
resolution_memory_id=$2, evidence=CASE WHEN $3='' THEN evidence ELSE $3 END,
resolved_at=pg_now_text(), updated_at=pg_now_text() WHERE id=$1 AND state='open'`, id, resolutionID, note)
	changed := err == nil && tag.RowsAffected() > 0
	if changed {
		runtimeMetricState.directiveResolved.Add(1)
	}
	return changed, err
}

func (s *postgresDataStore) DirectiveSuppress(ctx context.Context, id int64) (bool, error) {
	if err := s.requireKBDirective(); err != nil {
		return false, err
	}
	tag, err := s.db.Exec(ctx, `UPDATE epistemic_directives SET state='suppressed',
updated_at=pg_now_text() WHERE id=$1 AND state='open'`, id)
	return err == nil && tag.RowsAffected() > 0, err
}

func (s *postgresDataStore) DirectiveSweep(ctx context.Context) (int, error) {
	if err := s.requireKBDirective(); err != nil {
		return 0, err
	}
	tag, err := s.db.Exec(ctx, `UPDATE epistemic_directives SET state='expired', updated_at=pg_now_text()
WHERE state='open' AND valid_until<>''
  AND rtrim(replace(valid_until,'T',' '),'Z') < rtrim(replace(pg_now_text(),'T',' '),'Z')`)
	if err != nil {
		return 0, err
	}
	count := int(tag.RowsAffected())
	runtimeMetricState.directiveExpired.Add(int64(count))
	return count, nil
}

func (s *postgresDataStore) DirectiveMatch(ctx context.Context, turn, entity, file string, limit int) ([]Directive, error) {
	started := time.Now()
	defer runtimeMetricState.directiveCalls.observe(started)
	if err := s.requireKBDirective(); err != nil {
		return nil, err
	}
	rows, err := s.db.Query(ctx, `SELECT `+directiveColumns+` FROM epistemic_directives
WHERE state='open' AND (valid_until='' OR
 rtrim(replace(valid_until,'T',' '),'Z') >= rtrim(replace(pg_now_text(),'T',' '),'Z'))
AND (($2<>'' AND lower(anchor_entity)=lower($2)) OR
     ($3<>'' AND lower(anchor_file)=lower($3)) OR
     ($1<>'' AND (lower(question) LIKE '%'||lower($1)||'%' OR
                  lower(topic) LIKE '%'||lower($1)||'%' OR
                  (topic<>'' AND lower($1) LIKE '%'||lower(topic)||'%'))))
ORDER BY CASE WHEN $2<>'' AND lower(anchor_entity)=lower($2) THEN 3
              WHEN $3<>'' AND lower(anchor_file)=lower($3) THEN 2 ELSE 1 END DESC,
priority DESC, created_at DESC LIMIT $4`, turn, entity, file, limit)
	if err != nil {
		return nil, err
	}
	return scanDirectiveRows(rows)
}

func (s *postgresDataStore) DirectiveMarkSurfaced(ctx context.Context, id int64) (bool, error) {
	if err := s.requireKBDirective(); err != nil {
		return false, err
	}
	tag, err := s.db.Exec(ctx, `UPDATE epistemic_directives SET surfaced_count=surfaced_count+1,
last_surfaced_at=pg_now_text(), updated_at=pg_now_text() WHERE id=$1 AND state='open'`, id)
	changed := err == nil && tag.RowsAffected() > 0
	if changed {
		runtimeMetricState.directiveSurfaced.Add(1)
	}
	return changed, err
}

func (s *postgresDataStore) DirectiveCounts(ctx context.Context) (DirectiveCounts, error) {
	var counts DirectiveCounts
	if err := s.requireKBDirective(); err != nil {
		return counts, err
	}
	err := s.db.QueryRow(ctx, `SELECT
COUNT(*) FILTER (WHERE state='open'), COUNT(*) FILTER (WHERE state='suppressed'),
COUNT(*) FILTER (WHERE state='resolved'), COUNT(*) FILTER (WHERE state='expired')
FROM epistemic_directives`).Scan(&counts.Open, &counts.Suppressed, &counts.Resolved, &counts.Expired)
	return counts, err
}
