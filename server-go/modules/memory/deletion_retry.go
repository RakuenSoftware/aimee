package memory

import (
	"context"
	"encoding/json"
	"errors"
	"strconv"

	"github.com/JBailes/aimee/server-go/bus"
	store "github.com/JBailes/aimee/server-go/db"
)

type kbDeletion struct {
	target    MemoryRecordVersion
	scope     Scope
	authority int
	id        int64
}

// Lock and admit before opening the audit commit. Visibility comes from the
// request transaction's RLS context; a known version grants no additional scope.
func (s *postgresDataStore) prepareKBDeletion(ctx context.Context, id int64, authority int, expected *MemoryRecordVersion) (kbDeletion, error) {
	d := kbDeletion{id: id, authority: authority, target: MemoryRecordVersion{SchemaVersion: 1, RecordID: strconv.FormatInt(id, 10)}}
	if _, ok := s.db.(store.Tx); !ok || s.placement != PlacementKB || !expected.validFor(id) || (authority != AuthorityUser && authority != AuthorityModel) {
		return d, errors.New("memory: invalid conditional deletion")
	}
	var epistemic, origin, state string
	err := s.db.QueryRow(ctx, `SELECT epistemic_kind,provenance_category,lifecycle_state,record_revision::text,
 scope_type,scope_value,(SELECT owner_id::text FROM memory_collection_owner WHERE id=1)
 FROM memories WHERE id=$1 FOR UPDATE`, id).Scan(&epistemic, &origin, &state, &d.target.RecordRevision, &d.scope.Type, &d.scope.Value, &d.target.OwnerID)
	if store.IsNoRows(err) {
		return d, ErrMemoryNotFound
	}
	if err != nil {
		return d, err
	}
	if authority == AuthorityModel && state != "active" {
		return d, ErrMemoryNotFound
	}
	if *expected != d.target {
		return d, errMutationVersionConflict
	}
	if authority == AuthorityModel {
		if err := admitMemoryReplacement(epistemic, origin, authority); err != nil {
			return d, err
		}
	}
	return d, nil
}

func (s *postgresDataStore) applyKBDeletion(ctx context.Context, d kbDeletion) (receipt *MemoryMutationReceipt, err error) {
	defer func() {
		s.recordMutation(DataRequest{Operation: "delete-as", ID: d.id, Authority: d.authority}, DataResponse{Deleted: err == nil}, err, "")
	}()
	receipt = &MemoryMutationReceipt{SchemaVersion: 2, TargetVersion: d.target, Outcome: "destroyed"}
	if d.authority == AuthorityUser {
		var id int64
		err = s.db.QueryRow(ctx, `DELETE FROM memories WHERE id=$1 RETURNING id`, d.id).Scan(&id)
	} else {
		receipt.Outcome = "retired"
		receipt.Version = d.target
		err = s.db.QueryRow(ctx, `UPDATE memories SET key=key||'#v'||id::text,lifecycle_state='superseded',
 valid_until=pg_now_text(),archive_reason='retired by model',activation_suppressed=1,
 updated_at=pg_now_text() WHERE id=$1 AND lifecycle_state='active' RETURNING record_revision::text`, d.id).Scan(&receipt.Version.RecordRevision)
	}
	if store.IsNoRows(err) {
		err = ErrMemoryNotFound
	}
	return receipt, err
}

func (s *postgresDataStore) deleteKBVersion(ctx context.Context, id int64, authority int, expected *MemoryRecordVersion) (bool, error) {
	d, err := s.prepareKBDeletion(ctx, id, authority, expected)
	if err != nil {
		return false, err
	}
	_, err = s.applyKBDeletion(ctx, d)
	return err == nil, err
}

// Retried destruction confirms the original commit without inventing a current
// canonical version. It cannot erase a resurrected ID or reveal a hidden row.
func (s *postgresDataStore) deleteKBIdempotent(ctx context.Context, r DataRequest, authority int, caller *bus.CommandContext, correlation string) (receipt *MemoryMutationReceipt, err error) {
	if _, ok := s.db.(store.Tx); !ok || s.placement != PlacementKB || r.Operation != "delete-as" || !verifiedRetryCaller(caller) || !validIdempotencyKey(r.IdempotencyKey) || !r.ExpectedVersion.validFor(r.ID) {
		return nil, errors.New("memory: invalid idempotent deletion")
	}
	if authority == AuthorityUser && !caller.UserAuthority {
		return nil, errors.New("memory: verified user authority required")
	}
	var owner string
	if err = s.db.QueryRow(ctx, `SELECT owner_id::text FROM memory_collection_owner WHERE id=1`).Scan(&owner); err != nil {
		return nil, err
	}
	if owner != r.ExpectedVersion.OwnerID {
		return nil, errMutationVersionConflict
	}
	digest, err := correctionDigest(r, authority)
	if err != nil {
		return nil, err
	}
	keyHash := retryHash([]byte(r.IdempotencyKey))
	identity, _ := json.Marshal([]string{owner, caller.Principal, keyHash})
	if _, err = s.db.Exec(ctx, `SELECT pg_advisory_xact_lock(hashtextextended($1,5748))`, string(identity)); err != nil {
		return nil, err
	}
	var priorDigest, outcome, revision string
	receipt = &MemoryMutationReceipt{SchemaVersion: 2, TargetVersion: *r.ExpectedVersion}
	err = s.db.QueryRow(ctx, `SELECT request_hash,commit_id,operation,result_revision::text FROM memory_mutation_receipts
 WHERE owner_id=$1::uuid AND actor_principal=$2 AND key_hash=$3`, owner, caller.Principal, keyHash).Scan(&priorDigest, &receipt.CommitID, &outcome, &revision)
	if err == nil {
		if digest != priorDigest {
			return nil, errIdempotencyConflict
		}
		var current bool
		if err = s.db.QueryRow(ctx, `SELECT memory_deletion_replay_current($1::uuid,$2)`, owner, keyHash).Scan(&current); err != nil {
			return nil, err
		}
		if !current {
			return nil, errReplayUnavailable
		}
		receipt.Outcome = outcome
		if outcome == "retired" {
			receipt.Version = *r.ExpectedVersion
			receipt.Version.RecordRevision = revision
		}
		receipt.Replayed = true
		return receipt, nil
	}
	if !store.IsNoRows(err) {
		return nil, err
	}
	d, err := s.prepareKBDeletion(ctx, r.ID, authority, r.ExpectedVersion)
	if err != nil {
		return nil, err
	}
	actor := FactActor{Principal: caller.Principal, Role: "model", Rank: 10, Authenticated: 1, TransportIdentity: caller.TransportIdentity}
	operation := "memory.retire"
	if authority == AuthorityUser {
		actor.Role, actor.Rank = "user", 30
		operation = "memory.delete"
	}
	if actor.TransportIdentity == "" {
		actor.TransportIdentity = actor.Principal
	}
	var previous string
	if err = s.db.QueryRow(ctx, `SELECT COALESCE(current_setting('aimee.changeset_id',true),'')`).Scan(&previous); err != nil {
		return nil, err
	}
	commit, err := s.openFactCommit(ctx, actor, operation, correlation)
	if err != nil {
		return nil, err
	}
	if _, err = s.db.Exec(ctx, `SELECT set_config('aimee.changeset_id',$1,true)`, commit); err != nil {
		return nil, err
	}
	receipt, err = s.applyKBDeletion(ctx, d)
	if err != nil {
		return nil, err
	}
	receipt.CommitID = commit
	if err = s.closeFactCommit(ctx, commit, r.ID); err != nil {
		return nil, err
	}
	revision = receipt.Version.RecordRevision
	if receipt.Outcome == "destroyed" {
		revision = d.target.RecordRevision
	}
	if _, err = s.db.Exec(ctx, `INSERT INTO memory_mutation_receipts(owner_id,actor_principal,key_hash,request_hash,commit_id,
 result_id,result_revision,operation,target_revision,scope_type,scope_value)
 VALUES($1::uuid,$2,$3,$4,$5,$6,$7::bigint,$8,$9::bigint,$10,$11)`, owner, caller.Principal, keyHash, digest, commit, r.ID, revision, receipt.Outcome, d.target.RecordRevision, d.scope.Type, d.scope.Value); err != nil {
		return nil, err
	}
	if _, err = s.db.Exec(ctx, `SELECT set_config('aimee.changeset_id',$1,true)`, previous); err != nil {
		return nil, err
	}
	return receipt, nil
}
