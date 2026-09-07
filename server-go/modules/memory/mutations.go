package memory

import (
	"context"
	"errors"

	store "github.com/JBailes/aimee/server-go/db"
)

const (
	AuthorityModel = 0
	AuthorityUser  = 1

	MutationOK                  = 0
	MutationImmutableExperience = -2
	MutationRequiresReplacement = -3
)

var validEpistemicKinds = map[string]bool{
	"world_fact": true, "episode": true, "experience": true, "mental_model": true,
	"preference": true, "instruction": true, "policy": true, "hypothesis": true,
}

// InsertEpistemic is the canonical KB memory write. Authority is derived by the
// authenticated caller and becomes durable provenance; it is never inferred
// from the memory text. Active rejection tombstones fail the write closed.
func (s *postgresDataStore) InsertEpistemic(ctx context.Context, request DataRequest) (Record, error) {
	if err := s.requireKBDomain(); err != nil {
		return Record{}, err
	}
	epistemic := request.EpistemicKind
	if epistemic == "" {
		epistemic = "world_fact"
	}
	if !validEpistemicKinds[epistemic] || (request.Authority != AuthorityModel && request.Authority != AuthorityUser) {
		return Record{}, errors.New("memory: invalid epistemic write policy")
	}
	provenance := "agent_message"
	ceiling := 0.8
	if request.Authority == AuthorityUser {
		provenance, ceiling = "user_stated", 1.0
	}
	if request.Tier == "L5" && ceiling > 0.5 {
		ceiling = 0.5
	}
	confidence := 1.0
	if request.Confidence != nil {
		confidence = *request.Confidence
	}
	if confidence > ceiling {
		confidence = ceiling
	}
	scope := request.Scope
	var record Record
	record.Scope = scope
	err := s.db.QueryRow(ctx, `WITH allowed AS (
 SELECT 1 WHERE NOT EXISTS (
  SELECT 1 FROM memory_rejection_tombstones WHERE object_kind='memory' AND active=1
   AND memory_key=$3 AND memory_content=$4 AND scope_type=$10 AND scope_value=$11
 )
), updated AS (
 UPDATE memories SET tier=$1,content=$4,use_cases=$5,confidence=$6,confidence_ceiling=$7,
 source_session=$8,provenance_category=$9,epistemic_kind=$2,lifecycle_state='active',
 activation_suppressed=0,valid_until='',updated_at=pg_now_text()
 WHERE kind=$12 AND key=$3 AND scope_type=$10 AND scope_value=$11
   AND lifecycle_state='active' AND EXISTS(SELECT 1 FROM allowed)
 RETURNING id
), inserted AS (
 INSERT INTO memories(tier,kind,epistemic_kind,key,content,use_cases,confidence,confidence_ceiling,
 source_session,provenance_category,scope_type,scope_value,lifecycle_state)
 SELECT $1,$12,$2,$3,$4,$5,$6,$7,$8,$9,$10,$11,'active' FROM allowed
 WHERE NOT EXISTS(SELECT 1 FROM updated) RETURNING id
) SELECT id FROM updated UNION ALL SELECT id FROM inserted LIMIT 1`, request.Tier, epistemic,
		request.Key, request.Content, request.UseCases, confidence, ceiling, request.SessionID,
		provenance, scope.Type, scope.Value, request.Kind).Scan(&record.ID)
	if store.IsNoRows(err) {
		return Record{}, errors.New("memory: write blocked by rejection tombstone")
	}
	record.Tier, record.Kind, record.Key, record.Content, record.Confidence =
		request.Tier, request.Kind, request.Key, request.Content, confidence
	return record, err
}

// UpdateAs preserves model-authored history while allowing an authenticated
// operator to make the explicitly destructive in-place edit.
func (s *postgresDataStore) UpdateAs(ctx context.Context, id int64, content string, authority int) (int, int64, error) {
	if err := s.requireKBDomain(); err != nil {
		return -1, 0, err
	}
	var epistemic string
	if err := s.db.QueryRow(ctx, `SELECT epistemic_kind FROM memories WHERE id=$1`, id).Scan(&epistemic); err != nil {
		if store.IsNoRows(err) {
			return -1, 0, ErrMemoryNotFound
		}
		return -1, 0, err
	}
	switch epistemic {
	case "episode", "experience":
		return MutationImmutableExperience, id, nil
	case "instruction", "policy":
		return MutationRequiresReplacement, id, nil
	}
	if authority == AuthorityUser {
		updated, err := s.UpdateContent(ctx, id, content)
		if !updated && err == nil {
			return -1, 0, ErrMemoryNotFound
		}
		return MutationOK, id, err
	}
	if authority != AuthorityModel {
		return -1, 0, errors.New("memory: invalid authority")
	}
	var newID int64
	err := s.db.QueryRow(ctx, `WITH old AS (
 UPDATE memories SET key=key||'#v'||id::text,lifecycle_state='superseded',valid_until=pg_now_text(),
 archive_reason='superseded by model edit',activation_suppressed=1,updated_at=pg_now_text()
 WHERE id=$1 AND lifecycle_state='active'
 RETURNING tier,kind,epistemic_kind,regexp_replace(key,'#v[0-9]+$','') AS key,use_cases,
 confidence,confidence_ceiling,source_session,provenance_category,scope_type,scope_value
), fresh AS (
 INSERT INTO memories(tier,kind,epistemic_kind,key,content,use_cases,confidence,confidence_ceiling,
 source_session,provenance_category,scope_type,scope_value,lifecycle_state)
 SELECT tier,kind,epistemic_kind,key,$2,use_cases,confidence,confidence_ceiling,source_session,
 provenance_category,scope_type,scope_value,'active' FROM old RETURNING id
) SELECT id FROM fresh`, id, content).Scan(&newID)
	if store.IsNoRows(err) {
		return -1, 0, ErrMemoryNotFound
	}
	return MutationOK, newID, err
}

// DeleteAs hard-deletes only under explicit user authority. Model authority
// retires and versions the row so historical retrieval remains possible.
func (s *postgresDataStore) DeleteAs(ctx context.Context, id int64, authority int) (bool, error) {
	if err := s.requireKBDomain(); err != nil {
		return false, err
	}
	var query string
	switch authority {
	case AuthorityUser:
		query = `DELETE FROM memories WHERE id=$1`
	case AuthorityModel:
		query = `UPDATE memories SET key=key||'#v'||id::text,lifecycle_state='superseded',
valid_until=pg_now_text(),archive_reason='retired by model',activation_suppressed=1,
updated_at=pg_now_text() WHERE id=$1 AND lifecycle_state='active'`
	default:
		return false, errors.New("memory: invalid authority")
	}
	tag, err := s.db.Exec(ctx, query, id)
	return err == nil && tag.RowsAffected() > 0, err
}
