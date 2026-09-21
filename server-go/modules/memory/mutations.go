package memory

import (
	"context"
	"encoding/json"
	"errors"
	"math"
	"strings"

	store "github.com/JBailes/aimee/server-go/db"
)

const (
	AuthorityModel = 0
	AuthorityUser  = 1

	MutationOK                  = 0
	MutationImmutableExperience = -2
	MutationRequiresReplacement = -3
	MutationReviewRequired      = -4
	MutationVersionConflict     = -5
	MutationIdempotencyConflict = -6
	MutationReplayUnavailable   = -7
)

var validEpistemicKinds = map[string]bool{
	"world_fact": true, "episode": true, "experience": true, "mental_model": true,
	"preference": true, "instruction": true, "policy": true, "hypothesis": true,
}

// InsertEpistemic is the canonical KB memory write. Authority is derived by the
// authenticated caller and becomes durable provenance; it is never inferred
// from the memory text. Active rejection tombstones fail the write closed.
func (s *postgresDataStore) InsertEpistemic(ctx context.Context, request DataRequest) (record Record, err error) {
	replaced := false
	defer func() {
		if replaced {
			return
		}
		s.recordMutation(DataRequest{Operation: "insert-epistemic", SessionID: request.SessionID, Authority: request.Authority}, DataResponse{Records: []Record{record}}, err, "")
	}()
	if _, ok := s.db.(store.Tx); !ok {
		return Record{}, errors.New("memory: canonical writes require a transaction")
	}
	if err := s.requireKBDomain(); err != nil {
		return Record{}, err
	}
	if strings.TrimSpace(request.Scope.Value) == missingScopeValue {
		return Record{}, errors.New("memory: cannot store without active scope context")
	}
	var screenErr error
	request.Content, screenErr = screenMemoryWrite(request.Key, request.Content)
	if screenErr != nil {
		return Record{}, screenErr
	}
	request.UseCases, screenErr = screenMemoryText(request.UseCases)
	if screenErr != nil {
		return Record{}, screenErr
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
	if math.IsNaN(confidence) || math.IsInf(confidence, 0) || confidence < 0 || confidence > 1 {
		return Record{}, errors.New("memory: invalid confidence")
	}
	if confidence > ceiling {
		confidence = ceiling
	}
	scope := request.Scope
	record.Scope = scope
	// Serialize the conflict identity even when there is no row to lock yet.
	identity, _ := json.Marshal([4]string{string(scope.Type), scope.Value, request.Kind, request.Key})
	if _, err = s.db.Exec(ctx, `SELECT pg_advisory_xact_lock(hashtextextended($1,5747))`, string(identity)); err != nil {
		return Record{}, err
	}
	var count int
	var existing int64
	if err = s.db.QueryRow(ctx, `SELECT count(*),COALESCE(min(id),0) FROM memories
 WHERE kind=$1 AND key=$2 AND scope_type=$3 AND scope_value=$4 AND lifecycle_state='active'`,
		request.Kind, request.Key, scope.Type, scope.Value).Scan(&count, &existing); err != nil {
		return Record{}, err
	}
	if count > 1 {
		return Record{}, errors.New("memory: ambiguous active key requires review")
	}
	if existing != 0 {
		replaced = true
		request.Confidence = &confidence
		// All replacements use the same admission rules as edit and supersede.
		return s.replaceKBAs(ctx, existing, request.Content, confidence, request.SessionID, request.Authority, &request)
	}
	err = s.db.QueryRow(ctx, `INSERT INTO memories(tier,kind,epistemic_kind,key,content,use_cases,confidence,confidence_ceiling,
 source_session,provenance_category,scope_type,scope_value,lifecycle_state)
 SELECT $1,$12,$2,$3,$4,$5,$6,$7,$8,$9,$10,$11,'active'
 WHERE NOT EXISTS (
  SELECT 1 FROM memory_rejection_tombstones WHERE object_kind='memory' AND active=1
   AND memory_key=$3 AND memory_content=$4 AND scope_type=$10 AND scope_value=$11
 ) RETURNING id`, request.Tier, epistemic, request.Key, request.Content, request.UseCases, confidence,
		ceiling, request.SessionID, provenance, scope.Type, scope.Value, request.Kind).Scan(&record.ID)
	if store.IsNoRows(err) {
		return Record{}, errors.New("memory: write blocked by rejection tombstone")
	}

	record.Tier, record.Kind, record.Key, record.Content, record.Confidence =
		request.Tier, request.Kind, request.Key, request.Content, confidence
	return record, err
}

// UpdateAs is always a versioned correction. User authority determines the new
// author; it is not implicit permission to erase the previous version.
func (s *postgresDataStore) UpdateAs(ctx context.Context, id int64, content string, authority int) (int, int64, error) {
	record, err := s.replaceKBCorrection(ctx, id, content, nil, "", authority, nil, nil)
	if errors.Is(err, ErrMemoryNotFound) {
		return -1, 0, err
	}
	if code := mutationRefusal(err); code != 0 {
		if proposedCorrection(err) != nil {
			return code, id, err
		}
		return code, id, nil
	}
	return MutationOK, record.ID, err
}

func mutationRefusal(err error) int {
	switch {
	case errors.Is(err, errIdempotencyConflict):
		return MutationIdempotencyConflict
	case errors.Is(err, errReplayUnavailable):
		return MutationReplayUnavailable
	case errors.Is(err, errMutationVersionConflict):
		return MutationVersionConflict
	case errors.Is(err, errImmutableExperience):
		return MutationImmutableExperience
	case errors.Is(err, errRequiresRevocation):
		return MutationRequiresReplacement
	case errors.Is(err, errMutationReviewRequired):
		return MutationReviewRequired
	}
	return 0
}

// DeleteAs hard-deletes only under explicit user authority. Model authority
// retires and versions the row so historical retrieval remains possible.
func (s *postgresDataStore) DeleteAs(ctx context.Context, id int64, authority int) (changed bool, err error) {
	defer func() {
		s.recordMutation(DataRequest{Operation: "delete-as", ID: id, Authority: authority}, DataResponse{Deleted: changed}, err, "")
	}()
	if err := s.requireKBDomain(); err != nil {
		return false, err
	}
	var query string
	switch authority {
	case AuthorityUser:
		query = `DELETE FROM memories WHERE id=$1`
	case AuthorityModel:
		var epistemic, origin string
		if err := s.db.QueryRow(ctx, `SELECT epistemic_kind,provenance_category FROM memories WHERE id=$1 AND lifecycle_state='active' FOR UPDATE`, id).Scan(&epistemic, &origin); err != nil {
			if store.IsNoRows(err) {
				return false, nil
			}
			return false, err
		}
		if err := admitMemoryReplacement(epistemic, origin, authority); err != nil {
			return false, err
		}
		query = `UPDATE memories SET key=key||'#v'||id::text,lifecycle_state='superseded',
valid_until=pg_now_text(),archive_reason='retired by model',activation_suppressed=1,
updated_at=pg_now_text() WHERE id=$1 AND lifecycle_state='active'`
	default:
		return false, errors.New("memory: invalid authority")
	}
	tag, err := s.db.Exec(ctx, query, id)
	return err == nil && tag.RowsAffected() > 0, err
}

var (
	errMutationReviewRequired = errors.New("memory: replacing authoritative or unknown-origin content requires user review")
	errImmutableExperience    = errors.New("memory: episode and experience memories require annotation")
	errRequiresRevocation     = errors.New("memory: instruction and policy memories require revocation")
)

func admitMemoryReplacement(epistemic, origin string, authority int) error {
	switch epistemic {
	case "episode", "experience":
		return errImmutableExperience
	case "instruction", "policy":
		return errRequiresRevocation
	}
	if authority == AuthorityModel && origin != "agent_message" {
		return errMutationReviewRequired
	}
	return nil
}

// All replacement entry points preserve the old row and apply the same
// authority/epistemic admission before writing anything.
func (s *postgresDataStore) supersedeKB(ctx context.Context, id int64, content string, confidence float64, session string) (Record, error) {
	return s.replaceKBAs(ctx, id, content, confidence, session, AuthorityModel, nil)
}
func (s *postgresDataStore) replaceKBAs(ctx context.Context, id int64, content string, confidence float64, session string, authority int, metadata *DataRequest) (Record, error) {
	return s.replaceKBVersion(ctx, id, content, confidence, session, authority, metadata, nil)
}
func (s *postgresDataStore) replaceKBVersion(ctx context.Context, id int64, content string, confidence float64, session string, authority int, metadata *DataRequest, condition *MemoryRecordVersion) (Record, error) {
	return s.replaceKBCorrection(ctx, id, content, &confidence, session, authority, metadata, condition)
}

// A nil confidence preserves the locked original's value for update. Capture it
// in the same row read as admission and version comparison, without a second lock query.
func (s *postgresDataStore) replaceKBCorrection(ctx context.Context, id int64, content string, requestedConfidence *float64, session string, authority int, metadata *DataRequest, condition *MemoryRecordVersion) (r Record, err error) {
	defer func() {
		s.recordMutation(DataRequest{Operation: "supersede", ID: id, SessionID: session, Authority: authority}, DataResponse{Records: []Record{r}}, err, "")
	}()
	correction, err := s.prepareKBCorrection(ctx, id, content, requestedConfidence, session, authority, metadata, condition)
	if err != nil {
		return Record{}, err
	}
	return s.applyKBCorrection(ctx, correction)
}

// This value is private to the Go owner and valid only while the preparing
// transaction holds the target's row lock. Admission performs no durable writes;
// keyed callers can reject an edit before opening its canonical audit commit.
type preparedKBCorrection struct {
	id              int64
	content         string
	confidence      float64
	session         string
	provenance      string
	epistemic       string
	ceiling         float64
	tier, useCases  string
	replaceMetadata bool
	unchanged       *Record
}

func (s *postgresDataStore) prepareKBCorrection(ctx context.Context, id int64, content string, requestedConfidence *float64, session string, authority int, metadata *DataRequest, condition *MemoryRecordVersion) (preparedKBCorrection, error) {
	if err := s.requireKBDomain(); err != nil {
		return preparedKBCorrection{}, err
	}
	if _, ok := s.db.(store.Tx); !ok {
		return preparedKBCorrection{}, errors.New("memory: replacement requires a transaction")
	}
	if authority != AuthorityModel && authority != AuthorityUser {
		return preparedKBCorrection{}, errors.New("memory: invalid authority")
	}
	confidence := 0.0
	if requestedConfidence != nil {
		confidence = *requestedConfidence
		if math.IsNaN(confidence) || math.IsInf(confidence, 0) || confidence < 0 || confidence > 1 {
			return preparedKBCorrection{}, errors.New("memory: invalid confidence")
		}
	}
	var screenErr error
	content, screenErr = screenMemoryText(content)
	if screenErr != nil {
		return preparedKBCorrection{}, screenErr
	}
	var epistemic, origin, oldUseCases string
	var previous Record
	columns := "epistemic_kind,provenance_category,tier,kind,key,content,use_cases,confidence,scope_type,scope_value"
	destinations := []any{&epistemic, &origin, &previous.Tier, &previous.Kind, &previous.Key, &previous.Content, &oldUseCases, &previous.Confidence, &previous.Scope.Type, &previous.Scope.Value}
	predicate := "id=$1 AND lifecycle_state='active'"
	var owner, revision, lifecycle string
	if condition != nil {
		if !condition.validFor(id) {
			return preparedKBCorrection{}, errors.New("memory: invalid expected version")
		}
		// Lock the visible row even when already superseded, so a concurrent loser
		// receives a conflict. Hidden and missing identities remain indistinguishable.
		predicate = "id=$1"
		columns += ",(SELECT owner_id::text FROM memory_collection_owner WHERE id=1),record_revision::text,lifecycle_state"
		destinations = append(destinations, &owner, &revision, &lifecycle)
	}
	if err := s.db.QueryRow(ctx, "SELECT "+columns+" FROM memories WHERE "+predicate+" FOR UPDATE", id).Scan(destinations...); err != nil {
		if store.IsNoRows(err) {
			return preparedKBCorrection{}, ErrMemoryNotFound
		}
		return preparedKBCorrection{}, err
	}
	if condition != nil && (owner != condition.OwnerID || revision != condition.RecordRevision || lifecycle != "active") {
		return preparedKBCorrection{}, errMutationVersionConflict
	}

	if requestedConfidence == nil {
		confidence = previous.Confidence
		if math.IsNaN(confidence) || math.IsInf(confidence, 0) || confidence < 0 || confidence > 1 {
			return preparedKBCorrection{}, errors.New("memory: invalid confidence")
		}
	}

	// Identical same-author upserts retain identity. They cannot capture another
	// author's provenance and never relax an immutable record's content.
	expectedOrigin := "agent_message"
	if authority == AuthorityUser {
		expectedOrigin = "user_stated"
	}
	if metadata != nil && origin == expectedOrigin && previous.Content == content && previous.Tier == metadata.Tier && previous.Kind == metadata.Kind && oldUseCases == metadata.UseCases && previous.Confidence == confidence && (metadata.EpistemicKind == "" || metadata.EpistemicKind == epistemic) {
		previous.ID = id
		return preparedKBCorrection{unchanged: &previous}, nil
	}
	if err := admitMemoryReplacement(epistemic, origin, authority); err != nil {
		if authority == AuthorityModel && errors.Is(err, errMutationReviewRequired) {
			err = s.proposeKBCorrection(ctx, id, content, confidence, session, epistemic, previous.Tier, oldUseCases, metadata)
		}
		return preparedKBCorrection{}, err
	}
	if metadata != nil && metadata.EpistemicKind != "" && metadata.EpistemicKind != epistemic {
		if authority == AuthorityModel {
			return preparedKBCorrection{}, s.proposeKBCorrection(ctx, id, content, confidence, session, epistemic, previous.Tier, oldUseCases, metadata)
		}
		return preparedKBCorrection{}, errMutationReviewRequired
	}
	provenance, ceiling := "agent_message", 0.8
	if authority == AuthorityUser {
		provenance, ceiling = "user_stated", 1
	}
	tier, useCases := "", ""
	if metadata != nil {
		tier, useCases = metadata.Tier, metadata.UseCases
	}
	return preparedKBCorrection{id: id, content: content, confidence: confidence, session: session,
		provenance: provenance, ceiling: ceiling, tier: tier, useCases: useCases,
		replaceMetadata: metadata != nil}, nil
}

// All compatibility and keyed writers apply the same admitted correction. The
// caller must retain the preparing transaction through apply and roll back any
// error, including scope-copy, extraction, audit or receipt failure.
func (s *postgresDataStore) applyKBCorrection(ctx context.Context, correction preparedKBCorrection) (r Record, err error) {
	if correction.unchanged != nil {
		return *correction.unchanged, nil
	}

	err = s.db.QueryRow(ctx, `WITH candidate AS MATERIALIZED (
 SELECT *,pg_now_text() AS boundary FROM memories WHERE id=$1 AND lifecycle_state='active'
 AND NOT EXISTS (SELECT 1 FROM memory_rejection_tombstones t WHERE t.object_kind='memory' AND t.active=1
  AND t.memory_key=memories.key AND t.memory_content=$2 AND t.scope_type=memories.scope_type AND t.scope_value=memories.scope_value)
), closed AS (
 UPDATE memories m SET key=c.key||'#v'||m.id::text,lifecycle_state='superseded',valid_until=c.boundary,
 activation_suppressed=1,archive_reason='superseded by versioned replacement',updated_at=c.boundary
 FROM candidate c WHERE m.id=c.id RETURNING c.*
), fresh AS (
 INSERT INTO memories(tier,kind,epistemic_kind,key,content,use_cases,confidence,confidence_ceiling,
 source_session,provenance_category,scope_type,scope_value,lifecycle_state,valid_from,owner_principal,sensitivity)
 SELECT CASE WHEN $9 THEN $7 ELSE tier END,kind,CASE WHEN $10='' THEN epistemic_kind ELSE $10 END,key,$2,CASE WHEN $9 THEN $8 ELSE use_cases END,
 LEAST($3,$6,CASE WHEN (CASE WHEN $9 THEN $7 ELSE tier END)='L5' THEN 0.5 ELSE 1.0 END),
 LEAST($6,CASE WHEN (CASE WHEN $9 THEN $7 ELSE tier END)='L5' THEN 0.5 ELSE 1.0 END),
 CASE WHEN $4='' THEN source_session ELSE $4 END,$5,scope_type,scope_value,'active',boundary,owner_principal,sensitivity
 FROM closed RETURNING id,scope_type,scope_value,tier,kind,key,content,confidence
)
SELECT id,scope_type,scope_value,tier,kind,key,content,confidence FROM fresh`, correction.id, correction.content, correction.confidence, correction.session, correction.provenance, correction.ceiling, correction.tier, correction.useCases, correction.replaceMetadata, correction.epistemic).
		Scan(&r.ID, &r.Scope.Type, &r.Scope.Value, &r.Tier, &r.Kind, &r.Key, &r.Content, &r.Confidence)
	if store.IsNoRows(err) {
		return Record{}, ErrMemoryNotFound
	}
	if err != nil {
		return Record{}, err
	}
	// A sibling data-modifying CTE cannot see the new parent through the
	// secondary-scope RLS check. Copy metadata in the next statement of this
	// required transaction, after the parent is visible. Any copy/audit failure
	// still rolls back retirement, replacement, links and invalidations together.
	_, err = s.db.Exec(ctx, `WITH scopes AS (
 INSERT INTO memory_scopes(memory_id,scope_type,scope_value)
 SELECT $2,scope_type,scope_value FROM memory_scopes WHERE memory_id=$1
 ON CONFLICT DO NOTHING
)
INSERT INTO memory_links(source_id,target_id,relation) VALUES($2,$1,'supersedes')`, correction.id, r.ID)
	return r, err
}
