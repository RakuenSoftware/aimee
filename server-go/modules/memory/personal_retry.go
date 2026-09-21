package memory

import (
	"context"
	"encoding/json"
	"errors"
	"strconv"

	"github.com/JBailes/aimee/server-go/bus"
	store "github.com/JBailes/aimee/server-go/db"
)

// Only keyed calls take the extra lock and receipt reads. Unkeyed mutations
// retain their existing cost. The receipt is a durable transaction identity,
// not a cached response or a claim that downstream consumers have caught up.
func (s *postgresDataStore) mutatePersonalIdempotent(ctx context.Context, r DataRequest, caller *bus.CommandContext) (Record, *MemoryMutationReceipt, error) {
	creating := r.Operation == "store"
	if s.placement != PlacementServer || (!creating && r.Operation != "supersede" && r.Operation != "delete") ||
		!verifiedRetryCaller(caller) || !validIdempotencyKey(r.IdempotencyKey) ||
		(!creating && (r.ExpectedVersion == nil || !r.ExpectedVersion.validFor(r.ID))) ||
		(creating && (r.ExpectedVersion != nil || r.Key == "" || r.Kind == "" || r.Content == "")) ||
		(r.Operation != "delete" && r.Confidence == nil) {
		return Record{}, nil, errors.New("memory: invalid private idempotent mutation")
	}
	if db, ok := s.db.(store.DB); ok {
		tx, err := db.Begin(ctx)
		if err != nil {
			return Record{}, nil, err
		}
		defer tx.Rollback(context.WithoutCancel(ctx))
		bound := *s
		bound.db = tx
		record, receipt, err := bound.mutatePersonalIdempotent(ctx, r, caller)
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
	if !creating && owner != r.ExpectedVersion.OwnerID {
		return Record{}, nil, errMutationVersionConflict
	}
	actor := personalCaller(caller, r.Authority)
	var digest string
	var err error
	if creating {
		digest, err = creationDigest(r, actor.authority)
	} else {
		digest, err = correctionDigest(r, actor.authority)
	}
	if err != nil {
		return Record{}, nil, err
	}
	keyHash := retryHash([]byte(r.IdempotencyKey))
	identity, _ := json.Marshal([]string{owner, actor.principal, keyHash})
	if _, err = s.db.Exec(ctx, `SELECT pg_advisory_xact_lock(hashtextextended($1,5751))`, string(identity)); err != nil {
		return Record{}, nil, err
	}
	receipt := &MemoryMutationReceipt{SchemaVersion: 1, Version: MemoryRecordVersion{SchemaVersion: 1, OwnerID: owner}}
	if creating {
		receipt.SchemaVersion, receipt.Outcome = 2, "stored"
	} else {
		receipt.Version = *r.ExpectedVersion
	}
	var storedDigest, proposalID string
	err = s.db.QueryRow(ctx, `SELECT request_hash,commit_id::text,target_id::text,result_revision::text,COALESCE(proposal_id::text,'')
 FROM user_memory_mutation_receipts WHERE owner_id=$1::uuid AND actor_principal=$2 AND key_hash=$3`, owner, actor.principal, keyHash).Scan(&storedDigest, &receipt.CommitID, &receipt.Version.RecordID, &receipt.Version.RecordRevision, &proposalID)
	if err == nil {
		if storedDigest != digest {
			return Record{}, nil, errIdempotencyConflict
		}
		if r.Operation == "delete" {
			// A retirement receipt never returns retained content. Reactivation,
			// further revision or erasure cannot turn it into a new mutation.
			var revision, state string
			err = s.db.QueryRow(ctx, `SELECT record_revision::text,lifecycle_state FROM user_memories WHERE id=$1`, r.ID).Scan(&revision, &state)
			if store.IsNoRows(err) || (err == nil && (state != "retired" || revision != receipt.Version.RecordRevision || proposalID != "")) {
				return Record{}, nil, errReplayUnavailable
			}
			if err != nil {
				return Record{}, nil, err
			}
			receipt.Replayed = true
			return Record{ID: r.ID, Version: &receipt.Version}, receipt, nil
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
		resultID := r.ID
		if creating {
			resultID, err = strconv.ParseInt(receipt.Version.RecordID, 10, 64)
			if err != nil || resultID <= 0 {
				return Record{}, nil, errReplayUnavailable
			}
		}
		record, err := s.getAtVersioned(ctx, r.Scope, resultID, false, "", true)
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
	var record Record
	var outcome error
	if creating {
		record, outcome = bound.Put(ctx, r.Scope, Record{Scope: r.Scope, Tier: r.Tier, Kind: r.Kind, Key: r.Key, Content: r.Content, Confidence: *r.Confidence})
		if outcome == nil {
			record, outcome = bound.getAtVersioned(ctx, r.Scope, record.ID, false, "", true)
		}
	} else if r.Operation == "delete" {
		record, outcome = bound.retirePersonalVersion(ctx, r.Scope, r.ID, *r.ExpectedVersion)
	} else {
		record, outcome = bound.correctPersonalVersion(ctx, r.Scope, r.ID, r.Content, *r.Confidence, *r.ExpectedVersion)
	}
	var proposal any
	var targetID int64
	var targetRevision string
	if !creating {
		targetID, targetRevision = r.ID, r.ExpectedVersion.RecordRevision
	}
	if p := proposedCorrection(outcome); p != nil {
		proposal = p.ID
		if creating {
			targetID, err = strconv.ParseInt(p.Target.RecordID, 10, 64)
			if err != nil || targetID <= 0 {
				return Record{}, nil, errors.New("memory: invalid creation proposal target")
			}
			targetRevision = p.Target.RecordRevision
			receipt.Version = p.Target
		}
		receipt.Version.RecordRevision = "0"
	} else if outcome != nil {
		return Record{}, nil, outcome
	} else {
		receipt.Version = *record.Version
		if creating {
			targetID, targetRevision = record.ID, record.Version.RecordRevision
		}
	}
	err = s.db.QueryRow(ctx, `INSERT INTO user_memory_mutation_receipts(owner_id,actor_principal,key_hash,request_hash,target_id,target_revision,result_revision,proposal_id,operation)
 VALUES($1::uuid,$2,$3,$4,$5,$6::bigint,$7::bigint,$8::uuid,$9) RETURNING commit_id::text`, owner, actor.principal, keyHash, digest, targetID, targetRevision, receipt.Version.RecordRevision, proposal, r.Operation).Scan(&receipt.CommitID)
	if err != nil {
		return Record{}, nil, err
	}
	if outcome != nil {
		return Record{}, nil, outcome
	}
	return record, receipt, nil
}
