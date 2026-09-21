package memory

import (
	"context"
	"encoding/json"
	"errors"
	"strconv"

	"github.com/JBailes/aimee/server-go/bus"
	store "github.com/JBailes/aimee/server-go/db"
)

// Creation has no existing record version. Bind every admitted store input and
// effective authority, keeping the established correction digest byte-stable.
func creationDigest(r DataRequest, authority int) (string, error) {
	raw, err := json.Marshal(struct {
		SchemaVersion                                                int
		Operation                                                    string
		Scope                                                        Scope
		Workspace, Project                                           string
		IncludeAll                                                   bool
		Authority                                                    int
		Tier, Kind, EpistemicKind, Key, Content, UseCases, SessionID string
		Confidence                                                   *float64
	}{2, r.Operation, r.Scope, r.Workspace, r.Project, r.IncludeAll, authority,
		r.Tier, r.Kind, r.EpistemicKind, r.Key, r.Content, r.UseCases, r.SessionID, r.Confidence})
	if err != nil {
		return "", err
	}
	return retryHash(raw), nil
}

func commandCreationKey(args commandArgs, caller *bus.CommandContext) (string, map[string]any) {
	raw, exists := args["idempotency_key"]
	if !exists {
		return "", nil
	}
	var key string
	if json.Unmarshal(raw, &key) != nil || !validIdempotencyKey(key) {
		return "", commandError("invalid_argument", "idempotency_key requires 16-128 printable ASCII characters")
	}
	if !verifiedRetryCaller(caller) {
		return "", commandError("forbidden", "idempotent mutations require an authenticated principal")
	}
	return key, nil
}

// The caller owns the transaction: canonical write, extraction, WORM intent and
// receipt either commit together or all roll back. Admission can instead produce
// a linked review proposal, whose reference occupies the same retry namespace.
func (s *postgresDataStore) storeKBIdempotent(ctx context.Context, r DataRequest, caller *bus.CommandContext, correlation string) (Record, *MemoryMutationReceipt, error) {
	if _, ok := s.db.(store.Tx); !ok || s.placement != PlacementKB || r.Operation != "insert-epistemic" || !verifiedRetryCaller(caller) || !validIdempotencyKey(r.IdempotencyKey) || r.ExpectedVersion != nil {
		return Record{}, nil, errors.New("memory: invalid shared idempotent store")
	}
	if r.Authority == AuthorityUser && !caller.UserAuthority {
		return Record{}, nil, errors.New("memory: verified user authority required")
	}
	digest, err := creationDigest(r, r.Authority)
	if err != nil {
		return Record{}, nil, err
	}
	var owner string
	if err = s.db.QueryRow(ctx, `SELECT owner_id::text FROM memory_collection_owner WHERE id=1`).Scan(&owner); err != nil {
		return Record{}, nil, err
	}
	keyHash := retryHash([]byte(r.IdempotencyKey))
	identity, _ := json.Marshal([]string{owner, caller.Principal, keyHash})
	if _, err = s.db.Exec(ctx, `SELECT pg_advisory_xact_lock(hashtextextended($1,5748))`, string(identity)); err != nil {
		return Record{}, nil, err
	}
	receipt := &MemoryMutationReceipt{SchemaVersion: 2, Outcome: "stored", Version: MemoryRecordVersion{SchemaVersion: 1, OwnerID: owner}}
	var storedDigest, proposalID, operation string
	var id, revision int64
	err = s.db.QueryRow(ctx, `SELECT request_hash,commit_id,result_id,result_revision,COALESCE(proposal_id::text,''),operation
 FROM memory_mutation_receipts WHERE owner_id=$1::uuid AND actor_principal=$2 AND key_hash=$3`, owner, caller.Principal, keyHash).Scan(&storedDigest, &receipt.CommitID, &id, &revision, &proposalID, &operation)
	if err == nil {
		if storedDigest != digest {
			return Record{}, nil, errIdempotencyConflict
		}
		if proposalID != "" {
			return Record{}, nil, s.replayKBCorrectionProposal(ctx, proposalID)
		}
		record, e := s.getAtVersioned(ctx, r.Scope, id, false, "", true)
		if errors.Is(e, ErrMemoryNotFound) {
			return Record{}, nil, errReplayUnavailable
		}
		if e != nil {
			return Record{}, nil, e
		}
		if record.Version == nil || record.Version.OwnerID != owner || record.Version.RecordRevision != strconv.FormatInt(revision, 10) {
			return Record{}, nil, errReplayUnavailable
		}
		receipt.Version, receipt.Replayed = *record.Version, true
		if operation == "store_noop" {
			receipt.Outcome = "unchanged"
		}
		record.Version = nil
		return record, receipt, nil
	}
	if !store.IsNoRows(err) {
		return Record{}, nil, err
	}
	plan, err := s.prepareKBStore(ctx, r)
	if err != nil {
		if p := proposedCorrection(err); p != nil {
			var commit string
			if e := s.db.QueryRow(ctx, `SELECT commit_id FROM fact_graph_changes WHERE object_kind='review' AND object_key=$1 AND action='insert' ORDER BY id LIMIT 1`, p.ID).Scan(&commit); e != nil {
				return Record{}, nil, e
			}
			if _, e := s.db.Exec(ctx, `INSERT INTO memory_mutation_receipts(owner_id,actor_principal,key_hash,request_hash,commit_id,result_id,result_revision,proposal_id,operation,target_revision,scope_type,scope_value)
 VALUES($1::uuid,$2,$3,$4,$5,$6::bigint,0,$7::uuid,'store',$8::bigint,$9,$10)`, owner, caller.Principal, keyHash, digest, commit, p.Target.RecordID, p.ID, p.Target.RecordRevision, r.Scope.Type, r.Scope.Value); e != nil {
				return Record{}, nil, e
			}
		}
		return Record{}, nil, err
	}
	actor := FactActor{Principal: caller.Principal, Role: "model", Rank: 10, Authenticated: 1, TransportIdentity: caller.TransportIdentity}
	if r.Authority == AuthorityUser {
		actor.Role, actor.Rank = "user", 30
	}
	if actor.TransportIdentity == "" {
		actor.TransportIdentity = actor.Principal
	}
	operation = "store"
	if plan.correction != nil && plan.correction.unchanged != nil {
		operation = "store_noop"
		receipt.Outcome = "unchanged"
	}
	var previous string
	if err = s.db.QueryRow(ctx, `SELECT COALESCE(current_setting('aimee.changeset_id',true),'')`).Scan(&previous); err != nil {
		return Record{}, nil, err
	}
	receipt.CommitID, err = s.openFactCommit(ctx, actor, "memory."+operation, correlation)
	if err != nil {
		return Record{}, nil, err
	}
	if _, err = s.db.Exec(ctx, `SELECT set_config('aimee.changeset_id',$1,true)`, receipt.CommitID); err != nil {
		return Record{}, nil, err
	}
	record, err := s.applyKBStore(ctx, plan)
	if err != nil {
		return Record{}, nil, err
	}
	if err = s.captureStoredFactActor(ctx, record.ID, r.Authority, caller); err != nil {
		return Record{}, nil, err
	}
	if err = s.db.QueryRow(ctx, `SELECT record_revision FROM memories WHERE id=$1`, record.ID).Scan(&revision); err != nil {
		return Record{}, nil, err
	}
	if operation == "store_noop" {
		// Bind the unchanged observation without a fabricated memory mutation or
		// confirmation event. The parent field still carries the correlation.
		if _, err = s.db.Exec(ctx, `UPDATE fact_graph_commits SET origin_ref=$2 WHERE commit_id=$1 AND status='open'`, receipt.CommitID, "memory:"+strconv.FormatInt(record.ID, 10)+":"+strconv.FormatInt(revision, 10)); err != nil {
			return Record{}, nil, err
		}
	}
	if err = s.closeFactCommit(ctx, receipt.CommitID, record.ID); err != nil {
		return Record{}, nil, err
	}
	if _, err = s.db.Exec(ctx, `INSERT INTO memory_mutation_receipts(owner_id,actor_principal,key_hash,request_hash,commit_id,result_id,result_revision,operation,scope_type,scope_value)
 VALUES($1::uuid,$2,$3,$4,$5,$6,$7,$8,$9,$10)`, owner, caller.Principal, keyHash, digest, receipt.CommitID, record.ID, revision, operation, r.Scope.Type, r.Scope.Value); err != nil {
		return Record{}, nil, err
	}
	if _, err = s.db.Exec(ctx, `SELECT set_config('aimee.changeset_id',$1,true)`, previous); err != nil {
		return Record{}, nil, err
	}
	receipt.Version.RecordID, receipt.Version.RecordRevision = strconv.FormatInt(record.ID, 10), strconv.FormatInt(revision, 10)
	return record, receipt, nil
}
