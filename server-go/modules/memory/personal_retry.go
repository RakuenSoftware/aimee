package memory

import (
	"context"
	"encoding/json"
	"errors"

	"github.com/JBailes/aimee/server-go/bus"
	store "github.com/JBailes/aimee/server-go/db"
)

// Only keyed calls take the extra lock and receipt reads. Unkeyed mutations
// retain their existing cost. The receipt is a durable transaction identity,
// not a cached response or a claim that downstream consumers have caught up.
func (s *postgresDataStore) correctPersonalIdempotent(ctx context.Context, r DataRequest, caller *bus.CommandContext) (Record, *MemoryMutationReceipt, error) {
	if s.placement != PlacementServer || r.Operation != "supersede" || !verifiedRetryCaller(caller) || !validIdempotencyKey(r.IdempotencyKey) || r.ExpectedVersion == nil || !r.ExpectedVersion.validFor(r.ID) || r.Confidence == nil {
		return Record{}, nil, errors.New("memory: invalid private idempotent correction")
	}
	if db, ok := s.db.(store.DB); ok {
		tx, err := db.Begin(ctx)
		if err != nil {
			return Record{}, nil, err
		}
		defer tx.Rollback(context.WithoutCancel(ctx))
		bound := *s
		bound.db = tx
		record, receipt, err := bound.correctPersonalIdempotent(ctx, r, caller)
		if err != nil && proposedCorrection(err) == nil {
			return Record{}, nil, err
		}
		outcome := err
		if err = tx.Commit(ctx); err != nil {
			return Record{}, nil, err
		}
		return record, receipt, outcome
	}
	if _, ok := s.db.(store.Tx); !ok {
		return Record{}, nil, errors.New("memory: private retry requires a transaction")
	}
	var owner string
	if err := s.db.QueryRow(ctx, `SELECT owner_id::text FROM user_memory_collection_generation WHERE id=1`).Scan(&owner); err != nil {
		return Record{}, nil, err
	}
	if owner != r.ExpectedVersion.OwnerID {
		return Record{}, nil, errMutationVersionConflict
	}
	actor := personalCaller(caller, r.Authority)
	digest, err := correctionDigest(r, actor.authority)
	if err != nil {
		return Record{}, nil, err
	}
	keyHash := retryHash([]byte(r.IdempotencyKey))
	identity, _ := json.Marshal([]string{owner, actor.principal, keyHash})
	if _, err = s.db.Exec(ctx, `SELECT pg_advisory_xact_lock(hashtextextended($1,5751))`, string(identity)); err != nil {
		return Record{}, nil, err
	}
	receipt := &MemoryMutationReceipt{SchemaVersion: 1, Version: *r.ExpectedVersion}
	var storedDigest, proposalID string
	err = s.db.QueryRow(ctx, `SELECT request_hash,commit_id::text,target_id::text,result_revision::text,COALESCE(proposal_id::text,'')
 FROM user_memory_mutation_receipts WHERE owner_id=$1::uuid AND actor_principal=$2 AND key_hash=$3`, owner, actor.principal, keyHash).Scan(&storedDigest, &receipt.CommitID, &receipt.Version.RecordID, &receipt.Version.RecordRevision, &proposalID)
	if err == nil {
		if storedDigest != digest {
			return Record{}, nil, errIdempotencyConflict
		}
		if proposalID != "" {
			// The parent must still authorize inspection even when the proposal has no
			// canonical result yet. Never free a committed key after parent erasure.
			p, err := scanCorrectionProposal(s.db.QueryRow(ctx, `SELECT `+correctionProposalReferenceColumns+` FROM user_memory_correction_proposals
 WHERE proposal_id=$1::uuid AND owner_id=$2::uuid AND EXISTS(
 SELECT 1 FROM user_memories m WHERE m.id=target_id AND m.lifecycle_state IN ('active','retired')
 AND (m.valid_until IS NULL OR m.valid_until>now()))`, proposalID, owner), false)
			if errors.Is(err, ErrMemoryNotFound) {
				return Record{}, nil, errReplayUnavailable
			}
			if err != nil {
				return Record{}, nil, err
			}
			if err = s.checkCorrectionResult(ctx, p); err != nil {
				return Record{}, nil, err
			}
			p.Draft = nil
			p.Replayed = true
			return Record{}, nil, &correctionProposedError{Proposal: p}
		}
		record, err := s.getAtVersioned(ctx, r.Scope, r.ID, false, "", true)
		if errors.Is(err, ErrMemoryNotFound) {
			return Record{}, nil, errReplayUnavailable
		}
		if err != nil {
			return Record{}, nil, err
		}
		if record.Version == nil || *record.Version != receipt.Version {
			return Record{}, nil, errReplayUnavailable
		}
		receipt.Replayed = true
		return record, receipt, nil
	}
	if !store.IsNoRows(err) {
		return Record{}, nil, err
	}
	bound := *s
	bound.personalActor = actor
	record, outcome := bound.correctPersonalVersion(ctx, r.Scope, r.ID, r.Content, *r.Confidence, *r.ExpectedVersion)
	var proposal any
	if p := proposedCorrection(outcome); p != nil {
		proposal = p.ID
		receipt.Version.RecordRevision = "0"
	} else if outcome != nil {
		return Record{}, nil, outcome
	} else {
		receipt.Version = *record.Version
	}
	err = s.db.QueryRow(ctx, `INSERT INTO user_memory_mutation_receipts(owner_id,actor_principal,key_hash,request_hash,target_id,target_revision,result_revision,proposal_id)
 VALUES($1::uuid,$2,$3,$4,$5,$6::bigint,$7::bigint,$8::uuid) RETURNING commit_id::text`, owner, actor.principal, keyHash, digest, r.ID, r.ExpectedVersion.RecordRevision, receipt.Version.RecordRevision, proposal).Scan(&receipt.CommitID)
	if err != nil {
		return Record{}, nil, err
	}
	if outcome != nil {
		return Record{}, nil, outcome
	}
	return record, receipt, nil
}
