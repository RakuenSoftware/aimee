package memory

import (
	"context"
	"encoding/json"
	"errors"

	"github.com/JBailes/aimee/server-go/bus"
	store "github.com/JBailes/aimee/server-go/db"
)

// Keep correction's persisted digest format stable. Lifecycle decisions also
// bind the rejection reason; the actor comes exclusively from authenticated context.
func lifecycleDigest(r DataRequest, authority int) (string, error) {
	base, err := correctionDigest(r, authority)
	if err != nil {
		return "", err
	}
	encoded, err := json.Marshal(struct {
		Base   string
		Reason string
	}{base, r.Reason})
	return retryHash(encoded), err
}

// The caller rolls back on every error, including receipt insertion failures.
// Advisory locks serialize a caller's retry key; row locks serialize distinct
// keys against other canonical mutations through audit and receipt persistence.
func (s *postgresDataStore) lifecycleKBIdempotent(ctx context.Context, r DataRequest, caller *bus.CommandContext, correlation string) (*MemoryMutationReceipt, error) {
	if _, ok := s.db.(store.Tx); !ok || s.placement != PlacementKB ||
		(r.Operation != "reject" && r.Operation != "restore") || !verifiedRetryCaller(caller) ||
		!validIdempotencyKey(r.IdempotencyKey) || !r.ExpectedVersion.validFor(r.ID) {
		return nil, errors.New("memory: invalid idempotent lifecycle mutation")
	}
	authority := AuthorityModel
	if (r.Authority == AuthorityUser && caller.UserAuthority) || r.Operation == "restore" {
		authority = AuthorityUser
	}
	var owner string
	if err := s.db.QueryRow(ctx, "SELECT owner_id::text FROM memory_collection_owner WHERE id=1").Scan(&owner); err != nil {
		return nil, err
	}
	if owner != r.ExpectedVersion.OwnerID {
		return nil, errMutationVersionConflict
	}
	digest, err := lifecycleDigest(r, authority)
	if err != nil {
		return nil, err
	}
	keyHash := retryHash([]byte(r.IdempotencyKey))
	identity, _ := json.Marshal([]string{owner, caller.Principal, keyHash})
	if _, err = s.db.Exec(ctx, "SELECT pg_advisory_xact_lock(hashtextextended($1,5748))", string(identity)); err != nil {
		return nil, err
	}
	receipt := &MemoryMutationReceipt{SchemaVersion: 2, TargetVersion: *r.ExpectedVersion, Version: *r.ExpectedVersion}
	var priorDigest string
	err = s.db.QueryRow(ctx, `SELECT request_hash,commit_id,operation,result_revision::text FROM memory_mutation_receipts
 WHERE owner_id=$1::uuid AND actor_principal=$2 AND key_hash=$3`, owner, caller.Principal, keyHash).
		Scan(&priorDigest, &receipt.CommitID, &receipt.Outcome, &receipt.Version.RecordRevision)
	if err == nil {
		if priorDigest != digest {
			return nil, errIdempotencyConflict
		}
		// RLS still governs visibility. Hold the row lock while confirming the
		// canonical outcome; a receipt never grants access to a formerly visible row.
		if err = s.lockKBLifecycleVersion(ctx, r.ID, &receipt.Version); err != nil {
			if errors.Is(err, ErrMemoryNotFound) || errors.Is(err, errMutationVersionConflict) {
				return nil, errReplayUnavailable
			}
			return nil, err
		}
		var current bool
		err = s.db.QueryRow(ctx, `SELECT EXISTS(SELECT 1 FROM memories m WHERE m.id=$1 AND
   (($2='rejected' AND m.lifecycle_state='archived' AND m.activation_suppressed=1 AND m.archive_reason=$3
      AND EXISTS(SELECT 1 FROM memory_rejection_tombstones t WHERE t.object_kind='memory' AND t.active=1
       AND t.memory_key=m.key AND t.memory_content=m.content AND t.scope_type=m.scope_type AND t.scope_value=m.scope_value AND t.reason=$3))
    OR ($2='restored' AND `+currentMemorySQL("m.")+` AND m.activation_suppressed=0 AND m.archive_reason=''
      AND NOT EXISTS(SELECT 1 FROM memory_rejection_tombstones t WHERE t.object_kind='memory' AND t.active=1
       AND t.memory_key=m.key AND t.memory_content=m.content AND t.scope_type=m.scope_type AND t.scope_value=m.scope_value))))`, r.ID, receipt.Outcome, r.Reason).Scan(&current)
		if err != nil {
			return nil, err
		}
		if !current {
			return nil, errReplayUnavailable
		}
		receipt.Replayed = true
		return receipt, nil
	}
	if !store.IsNoRows(err) {
		return nil, err
	}
	if err = s.lockKBLifecycleVersion(ctx, r.ID, r.ExpectedVersion); err != nil {
		return nil, err
	}
	actor := FactActor{Principal: caller.Principal, Role: "model", Rank: 10, Authenticated: 1, TransportIdentity: caller.TransportIdentity}
	if authority == AuthorityUser {
		actor.Role, actor.Rank = "user", 30
	}
	if actor.TransportIdentity == "" {
		actor.TransportIdentity = actor.Principal
	}
	var previous string
	if err = s.db.QueryRow(ctx, "SELECT COALESCE(current_setting('aimee.changeset_id',true),'')").Scan(&previous); err != nil {
		return nil, err
	}
	receipt.CommitID, err = s.openFactCommit(ctx, actor, "memory."+r.Operation, correlation)
	if err != nil {
		return nil, err
	}
	if _, err = s.db.Exec(ctx, "SELECT set_config('aimee.changeset_id',$1,true)", receipt.CommitID); err != nil {
		return nil, err
	}
	var updated bool
	receipt.Outcome = "rejected"
	if r.Operation == "reject" {
		err = s.db.QueryRow(ctx, `SELECT EXISTS(SELECT 1 FROM memories m
 WHERE m.id=$1 AND m.lifecycle_state='archived' AND m.activation_suppressed=1 AND m.archive_reason=$2
 AND EXISTS(SELECT 1 FROM memory_rejection_tombstones t WHERE t.object_kind='memory' AND t.active=1
 AND t.memory_key=m.key AND t.memory_content=m.content AND t.scope_type=m.scope_type AND t.scope_value=m.scope_value AND t.reason=$2))`, r.ID, r.Reason).Scan(&updated)
		if err == nil && !updated {
			updated, err = s.Reject(ctx, r.ID, r.Reason)
		}
	} else {
		receipt.Outcome = "restored"
		updated, err = s.Restore(ctx, r.ID, caller.Principal)
	}
	if err != nil {
		return nil, err
	}
	if !updated {
		return nil, ErrMemoryNotFound
	}
	var scope Scope
	if err = s.db.QueryRow(ctx, "SELECT record_revision::text,scope_type,scope_value FROM memories WHERE id=$1", r.ID).
		Scan(&receipt.Version.RecordRevision, &scope.Type, &scope.Value); err != nil {
		return nil, err
	}
	// A repeated rejection with a fresh key can retain the canonical revision.
	// Bind that no-op explicitly instead of manufacturing a memory change.
	if receipt.Version == receipt.TargetVersion {
		if _, err = s.db.Exec(ctx, "UPDATE fact_graph_commits SET origin_ref=$2 WHERE commit_id=$1",
			receipt.CommitID, "memory:"+receipt.Version.RecordID+":"+receipt.Version.RecordRevision); err != nil {
			return nil, err
		}
	}
	if err = s.closeFactCommit(ctx, receipt.CommitID, r.ID); err != nil {
		return nil, err
	}
	if _, err = s.db.Exec(ctx, `INSERT INTO memory_mutation_receipts(owner_id,actor_principal,key_hash,request_hash,commit_id,
 result_id,result_revision,operation,target_revision,scope_type,scope_value)
 VALUES($1::uuid,$2,$3,$4,$5,$6,$7::bigint,$8,$9::bigint,$10,$11)`,
		owner, caller.Principal, keyHash, digest, receipt.CommitID, r.ID, receipt.Version.RecordRevision, receipt.Outcome,
		receipt.TargetVersion.RecordRevision, scope.Type, scope.Value); err != nil {
		return nil, err
	}
	if _, err = s.db.Exec(ctx, "SELECT set_config('aimee.changeset_id',$1,true)", previous); err != nil {
		return nil, err
	}
	return receipt, nil
}
