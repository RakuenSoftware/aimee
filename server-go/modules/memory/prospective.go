package memory

import (
	"context"
	"errors"
	"strings"
	"time"

	store "github.com/JBailes/aimee/server-go/modules/aimee"
)

type Prospective struct {
	ID              int64  `json:"id"`
	TriggerText     string `json:"trigger_text"`
	ActionText      string `json:"action_text"`
	AnchorEntity    string `json:"anchor_entity"`
	AnchorFile      string `json:"anchor_file"`
	Recurrence      string `json:"recurrence"`
	State           string `json:"state"`
	ValidUntil      string `json:"valid_until"`
	SourceSession   string `json:"source_session"`
	TriggerCount    int    `json:"trigger_count"`
	LastTriggeredAt string `json:"last_triggered_at"`
	CreatedAt       string `json:"created_at"`
	UpdatedAt       string `json:"updated_at"`
}

const prospectiveColumns = `id, trigger_text, action_text, anchor_entity, anchor_file,
recurrence, state, valid_until, source_session, trigger_count, last_triggered_at,
created_at, updated_at`

func scanProspective(row store.Row, out *Prospective) error {
	return row.Scan(&out.ID, &out.TriggerText, &out.ActionText, &out.AnchorEntity, &out.AnchorFile,
		&out.Recurrence, &out.State, &out.ValidUntil, &out.SourceSession, &out.TriggerCount,
		&out.LastTriggeredAt, &out.CreatedAt, &out.UpdatedAt)
}

func scanProspectiveRows(rows store.Rows) ([]Prospective, error) {
	defer rows.Close()
	items := make([]Prospective, 0)
	for rows.Next() {
		var item Prospective
		if err := rows.Scan(&item.ID, &item.TriggerText, &item.ActionText, &item.AnchorEntity,
			&item.AnchorFile, &item.Recurrence, &item.State, &item.ValidUntil, &item.SourceSession,
			&item.TriggerCount, &item.LastTriggeredAt, &item.CreatedAt, &item.UpdatedAt); err != nil {
			return nil, err
		}
		items = append(items, item)
	}
	return items, rows.Err()
}

func (s *postgresDataStore) requireKBProspective() error {
	if s.placement != PlacementKB {
		return errors.New("memory: prospective memory belongs to kb placement")
	}
	return nil
}

func (s *postgresDataStore) ProspectiveCreate(ctx context.Context, request DataRequest) (Prospective, error) {
	if err := s.requireKBProspective(); err != nil {
		return Prospective{}, err
	}
	recurrence := request.Recurrence
	if recurrence == "" {
		recurrence = "once"
	}
	if recurrence != "once" && recurrence != "repeat" {
		return Prospective{}, errors.New("memory: invalid prospective recurrence")
	}
	var item Prospective
	err := scanProspective(s.db.QueryRow(ctx, `INSERT INTO prospective_memories
(trigger_text, action_text, anchor_entity, anchor_file, recurrence, valid_until, source_session)
VALUES ($1,$2,$3,$4,$5,$6,$7) RETURNING `+prospectiveColumns,
		request.TriggerText, request.ActionText, request.AnchorEntity, request.AnchorFile,
		recurrence, request.ValidUntil, request.SessionID), &item)
	return item, err
}

func (s *postgresDataStore) ProspectiveList(ctx context.Context, state string, limit int) ([]Prospective, error) {
	if err := s.requireKBProspective(); err != nil {
		return nil, err
	}
	if state != "" && state != "armed" && state != "triggered" && state != "completed" && state != "expired" {
		return nil, errors.New("memory: invalid prospective state")
	}
	rows, err := s.db.Query(ctx, `SELECT `+prospectiveColumns+` FROM prospective_memories
WHERE ($1 = '' OR state = $1) ORDER BY created_at DESC, id DESC LIMIT $2`, state, limit)
	if err != nil {
		return nil, err
	}
	return scanProspectiveRows(rows)
}

func (s *postgresDataStore) ProspectiveGet(ctx context.Context, id int64) (Prospective, error) {
	if err := s.requireKBProspective(); err != nil {
		return Prospective{}, err
	}
	var item Prospective
	err := scanProspective(s.db.QueryRow(ctx, `SELECT `+prospectiveColumns+`
FROM prospective_memories WHERE id=$1`, id), &item)
	if store.IsNoRows(err) {
		return Prospective{}, ErrMemoryNotFound
	}
	return item, err
}

func (s *postgresDataStore) ProspectiveComplete(ctx context.Context, id int64) (bool, error) {
	if err := s.requireKBProspective(); err != nil {
		return false, err
	}
	tag, err := s.db.Exec(ctx, `UPDATE prospective_memories SET state='completed', updated_at=pg_now_text()
WHERE id=$1 AND state IN ('armed','triggered')`, id)
	changed := err == nil && tag.RowsAffected() > 0
	if changed {
		runtimeMetricState.prospectiveDone.Add(1)
	}
	return changed, err
}

func (s *postgresDataStore) ProspectiveSweep(ctx context.Context) (int, error) {
	if err := s.requireKBProspective(); err != nil {
		return 0, err
	}
	tag, err := s.db.Exec(ctx, `UPDATE prospective_memories SET state='expired', updated_at=pg_now_text()
WHERE state='armed' AND valid_until<>''
  AND rtrim(replace(valid_until,'T',' '),'Z') < rtrim(replace(pg_now_text(),'T',' '),'Z')`)
	if err != nil {
		return 0, err
	}
	count := int(tag.RowsAffected())
	runtimeMetricState.prospectiveExpired.Add(int64(count))
	return count, nil
}

func (s *postgresDataStore) ProspectiveMatch(ctx context.Context, turn, entity, file string,
	limit int) ([]Prospective, error) {
	started := time.Now()
	defer runtimeMetricState.prospectiveCalls.observe(started)
	if err := s.requireKBProspective(); err != nil {
		return nil, err
	}
	turn = strings.TrimSpace(turn)
	rows, err := s.db.Query(ctx, `SELECT `+prospectiveColumns+` FROM prospective_memories
WHERE state='armed' AND (valid_until='' OR
  rtrim(replace(valid_until,'T',' '),'Z') >= rtrim(replace(pg_now_text(),'T',' '),'Z'))
AND (($2<>'' AND lower(anchor_entity)=lower($2)) OR
     ($3<>'' AND lower(anchor_file)=lower($3)) OR
     ($1<>'' AND (lower($1) LIKE '%'||lower(trigger_text)||'%' OR
                  lower(trigger_text) LIKE '%'||lower($1)||'%' OR
                  lower(action_text) LIKE '%'||lower($1)||'%')))
ORDER BY CASE WHEN $2<>'' AND lower(anchor_entity)=lower($2) THEN 3
              WHEN $3<>'' AND lower(anchor_file)=lower($3) THEN 2 ELSE 1 END DESC,
         trigger_count ASC, created_at DESC LIMIT $4`, turn, entity, file, limit)
	if err != nil {
		return nil, err
	}
	return scanProspectiveRows(rows)
}

func (s *postgresDataStore) ProspectiveMarkTriggered(ctx context.Context, id int64) (bool, error) {
	if err := s.requireKBProspective(); err != nil {
		return false, err
	}
	tag, err := s.db.Exec(ctx, `UPDATE prospective_memories SET
trigger_count=trigger_count+1, last_triggered_at=pg_now_text(), updated_at=pg_now_text(),
state=CASE WHEN recurrence='once' THEN 'triggered' ELSE 'armed' END
WHERE id=$1 AND state='armed'`, id)
	changed := err == nil && tag.RowsAffected() > 0
	if changed {
		runtimeMetricState.prospectiveTrigger.Add(1)
	}
	return changed, err
}
