package memory

import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"strconv"

	"github.com/JBailes/aimee/server-go/bus"
	store "github.com/JBailes/aimee/server-go/db"
)

var errIdempotencyConflict = errors.New("memory: idempotency key was already used for a different mutation")
var errReplayUnavailable = errors.New("memory: mutation was already committed but its result is no longer available at the committed version; the mutation was not repeated")

// MemoryMutationReceipt identifies the committed mutation (a KB audit commit
// or private transaction receipt), never a cached content response. A replay must
// pass current visibility and revision checks.
type MemoryMutationReceipt struct {
	SchemaVersion int                 `json:"schema_version"`
	CommitID      string              `json:"commit_id"`
	Version       MemoryRecordVersion `json:"version"`
	Replayed      bool                `json:"replayed"`
}

func validIdempotencyKey(key string) bool {
	if len(key) < 16 || len(key) > 128 {
		return false
	}
	for _, c := range key {
		if c < 33 || c > 126 {
			return false
		}
	}
	return true
}

func verifiedRetryCaller(c *bus.CommandContext) bool {
	return c != nil && c.Authenticated && c.Principal != "" && len(c.Principal) <= 1024
}

func retryHash(value []byte) string {
	sum := sha256.Sum256(value)
	return hex.EncodeToString(sum[:])
}

// Hash the request admitted by the owner, excluding presentation, trace IDs and
// connection identity. The authenticated principal namespaces the opaque key;
// effective authority, scope and the expected version are part of the request.
func correctionDigest(r DataRequest, authority int) (string, error) {
	raw, err := json.Marshal(struct {
		SchemaVersion      int
		Operation          string
		ID                 int64
		Content            string
		Confidence         *float64
		SessionID          string
		Authority          int
		Scope              Scope
		Workspace, Project string
		IncludeAll         bool
		ExpectedVersion    *MemoryRecordVersion
	}{1, r.Operation, r.ID, r.Content, r.Confidence, r.SessionID, authority, r.Scope, r.Workspace, r.Project, r.IncludeAll, r.ExpectedVersion})
	if err != nil {
		return "", err
	}
	return retryHash(raw), nil
}

// The caller must roll back its transaction on ANY error, including a normal
// failure: the audit commit, mutation, extraction job and receipt are atomic.
// A correctionProposedError is a committed proposal outcome, not a failed write.
// Only keyed corrections pay for the additional lock and receipt queries.
func (s *postgresDataStore) replaceKBIdempotent(ctx context.Context, r DataRequest, authority int, caller *bus.CommandContext, correlation string) (record Record, receipt *MemoryMutationReceipt, err error) {
	if _, ok := s.db.(store.Tx); !ok || s.placement != PlacementKB || !verifiedRetryCaller(caller) || !validIdempotencyKey(r.IdempotencyKey) || !r.ExpectedVersion.validFor(r.ID) || !versionedCorrectionOperation(r.Operation) || (r.Operation == "supersede" && r.Confidence == nil) || (r.Operation == "update-as" && r.Confidence != nil) {
		return Record{}, nil, errors.New("memory: invalid idempotent correction")
	}
	digest, err := correctionDigest(r, authority)
	if err != nil {
		return Record{}, nil, err
	}
	keyHash := retryHash([]byte(r.IdempotencyKey))
	var owner string
	if err = s.db.QueryRow(ctx, `SELECT owner_id::text FROM memory_collection_owner WHERE id=1`).Scan(&owner); err != nil {
		return Record{}, nil, err
	}
	if owner != r.ExpectedVersion.OwnerID {
		return Record{}, nil, errMutationVersionConflict
	}
	identity, _ := json.Marshal([]string{owner, caller.Principal, keyHash})
	if _, err = s.db.Exec(ctx, `SELECT pg_advisory_xact_lock(hashtextextended($1,5748))`, string(identity)); err != nil {
		return Record{}, nil, err
	}
	receipt = &MemoryMutationReceipt{SchemaVersion: 1, Version: MemoryRecordVersion{SchemaVersion: 1, OwnerID: owner}}
	var storedDigest, proposalID string
	var id, revision int64
	err = s.db.QueryRow(ctx, `SELECT request_hash,commit_id,result_id,result_revision,COALESCE(proposal_id::text,'') FROM memory_mutation_receipts
 WHERE owner_id=$1::uuid AND actor_principal=$2 AND key_hash=$3`, owner, caller.Principal, keyHash).Scan(&storedDigest, &receipt.CommitID, &id, &revision, &proposalID)
	if err == nil {
		if storedDigest != digest {
			return Record{}, nil, errIdempotencyConflict
		}

		if proposalID != "" {
			p, err := scanCorrectionProposal(s.db.QueryRow(ctx, `SELECT `+correctionProposalReferenceColumns+` FROM memory_correction_proposals WHERE proposal_id=$1::uuid`, proposalID), false)
			if errors.Is(err, ErrMemoryNotFound) {
				return Record{}, nil, errReplayUnavailable
			}
			if err != nil {
				return Record{}, nil, err
			}
			if err = s.checkCorrectionResult(ctx, p); err != nil {
				return Record{}, nil, err
			}
			p.Draft, p.Replayed = nil, true
			return Record{}, nil, &correctionProposedError{Proposal: p}
		}
		// Never requeue extraction or return stored content. The ordinary exact read
		// applies current RLS, expiry, lifecycle and suppression gates.
		record, readErr := s.getAtVersioned(ctx, r.Scope, id, false, "", true)
		if errors.Is(readErr, ErrMemoryNotFound) {
			return Record{}, nil, errReplayUnavailable
		}
		if readErr != nil {
			return Record{}, nil, readErr
		}
		if record.Version.OwnerID != owner || record.Version.RecordRevision != strconv.FormatInt(revision, 10) {
			return Record{}, nil, errReplayUnavailable
		}
		receipt.Version = *record.Version
		receipt.Replayed = true
		// The receipt carries the exact version without changing the legacy row shape.
		record.Version = nil
		return record, receipt, nil
	}
	if !store.IsNoRows(err) {
		return Record{}, nil, err
	}
	actor := FactActor{Principal: caller.Principal, Role: "model", Rank: 10, Authenticated: 1, TransportIdentity: caller.TransportIdentity}
	if authority == AuthorityUser {
		if !caller.UserAuthority {
			return Record{}, nil, errors.New("memory: verified user authority required")
		}
		actor.Role, actor.Rank = "user", 30
	}
	if actor.TransportIdentity == "" {
		actor.TransportIdentity = actor.Principal
	}
	// Keep the row locked from admission through the canonical write. In
	// particular, a model edit requiring review must not open a write commit.
	defer func() {
		s.recordMutation(DataRequest{Operation: "supersede", ID: r.ID, SessionID: r.SessionID, Authority: authority}, DataResponse{Records: []Record{record}}, err, "")
	}()
	correction, err := s.prepareKBCorrection(ctx, r.ID, r.Content, r.Confidence, r.SessionID, authority, nil, r.ExpectedVersion)
	if err != nil {
		if proposal := proposedCorrection(err); proposal != nil {
			// A draft is a durable outcome too. Bind the same retry namespace
			// to its content-free reference, retaining it after parent erasure.
			var commit string
			if auditErr := s.db.QueryRow(ctx, `SELECT commit_id FROM fact_graph_changes WHERE object_kind='review' AND object_key=$1 AND action='insert' ORDER BY id LIMIT 1`, proposal.ID).Scan(&commit); auditErr != nil {
				return Record{}, nil, auditErr
			}
			if _, receiptErr := s.db.Exec(ctx, `INSERT INTO memory_mutation_receipts(owner_id,actor_principal,key_hash,request_hash,commit_id,result_id,result_revision,proposal_id)
 VALUES($1::uuid,$2,$3,$4,$5,$6::bigint,0,$7::uuid)`, owner, caller.Principal, keyHash, digest, commit, proposal.Target.RecordID, proposal.ID); receiptErr != nil {
				return Record{}, nil, receiptErr
			}
		}
		return Record{}, nil, err
	}
	var previous string
	if err = s.db.QueryRow(ctx, `SELECT COALESCE(current_setting('aimee.changeset_id',true),'')`).Scan(&previous); err != nil {
		return Record{}, nil, err
	}
	// Join the existing changeset/WORM path, not a second audit service.
	operation := "memory.supersede"
	if r.Operation == "update-as" {
		operation = "memory.update"
	}
	receipt.CommitID, err = s.openFactCommit(ctx, actor, operation, correlation)
	if err != nil {
		return Record{}, nil, err
	}
	if _, err = s.db.Exec(ctx, `SELECT set_config('aimee.changeset_id',$1,true)`, receipt.CommitID); err != nil {
		return Record{}, nil, err
	}
	record, err = s.applyKBCorrection(ctx, correction)
	if err != nil {
		return Record{}, nil, err
	}
	if err = s.captureStoredFactActor(ctx, record.ID, authority, caller); err != nil {
		return Record{}, nil, err
	}
	if err = s.db.QueryRow(ctx, `SELECT record_revision FROM memories WHERE id=$1`, record.ID).Scan(&revision); err != nil {
		return Record{}, nil, err
	}
	if err = s.closeFactCommit(ctx, receipt.CommitID, record.ID); err != nil {
		return Record{}, nil, err
	}
	if _, err = s.db.Exec(ctx, `INSERT INTO memory_mutation_receipts(owner_id,actor_principal,key_hash,request_hash,commit_id,result_id,result_revision)
 VALUES($1::uuid,$2,$3,$4,$5,$6,$7)`, owner, caller.Principal, keyHash, digest, receipt.CommitID, record.ID, revision); err != nil {
		return Record{}, nil, err
	}
	if _, err = s.db.Exec(ctx, `SELECT set_config('aimee.changeset_id',$1,true)`, previous); err != nil {
		return Record{}, nil, err
	}
	receipt.Version.RecordID = strconv.FormatInt(record.ID, 10)
	receipt.Version.RecordRevision = strconv.FormatInt(revision, 10)
	return record, receipt, nil
}

func versionedCorrectionOperation(operation string) bool {
	return operation == "supersede" || operation == "update-as"
}

func commandCorrectionOptions(args commandArgs, id int64, caller *bus.CommandContext) (*MemoryRecordVersion, string, map[string]any) {
	expected, valid := commandExpectedVersion(args, id)
	if !valid {
		return nil, "", commandError("invalid_argument", "expected_version must identify the owner, target and positive revision using schema_version=1")
	}
	key := ""
	if raw, exists := args["idempotency_key"]; exists {
		if json.Unmarshal(raw, &key) != nil || !validIdempotencyKey(key) || expected == nil {
			return nil, "", commandError("invalid_argument", "idempotency_key requires 16-128 printable ASCII characters and expected_version")
		}
		if !verifiedRetryCaller(caller) {
			return nil, "", commandError("forbidden", "idempotent mutations require an authenticated principal")
		}
	}
	return expected, key, nil
}
