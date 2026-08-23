package db2

import (
	"context"
	"errors"

	"github.com/JBailes/aimee/server-go/bus"
	db2contract "github.com/JBailes/aimee/server-go/db2"
)

func init() {
	Register(db2contract.StageMemoryDedupeCandidates,
		db2contract.OperationMemoryDedupeCandidates, memoryDedupeCandidates)
	Register(db2contract.StageMemoryCorefAuditInsert,
		db2contract.OperationMemoryCorefAuditInsert, memoryCorefAuditInsert)
	Register(db2contract.StageAntiPatternListHot,
		db2contract.OperationAntiPatternListHot, antiPatternListHot)
	Register(db2contract.StageCrossRepoSetTrust,
		db2contract.OperationCrossRepoSetTrust, crossRepoSetTrust)
	Register(db2contract.StageEnrollmentInsert,
		db2contract.OperationEnrollmentInsert, enrollmentInsert)
}

// Two exclusions, and they rule out different things. An expired memory is not
// a dedupe candidate because merging into it would revive it, and a key
// containing the versioned marker is a memory that is deliberately kept
// alongside its other versions -- deduplicating those would undo the versioning.
//
// The C selects surprise and never reads it; the reply has no field for it.
const memoryDedupeCandidatesQuery = `SELECT id, key, confidence, use_count,
 observation_count, evidence_strength
 FROM memories
 WHERE kind = $1 AND COALESCE(valid_until, '') = '' AND key NOT LIKE '%#v%'
 LIMIT $2`

// memoryDedupeCandidates lists memories of one kind that could be duplicates of
// each other.
//
// It proposes rather than decides: everything of the kind that is still live
// comes back, and the caller compares them. The four score columns are what it
// compares on -- which of two near-identical memories to keep is a question
// about use and evidence rather than about text.
func memoryDedupeCandidates(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	kind, err := db2contract.DecodeMemoryDedupeCandidatesRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	ceiling := db2contract.MemoryDedupeCandidatesMaxRows
	rows, queryErr := store.Query(ctx, memoryDedupeCandidatesQuery,
		kind, int64(ceiling))
	if queryErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	defer rows.Close()

	candidates := make([]db2contract.MemoryDedupeCandidatesRow, 0, 16)
	for rows.Next() {
		var id, useCount, observations int64
		var key string
		var confidence, evidence float64
		if scanErr := rows.Scan(&id, &key, &confidence, &useCount,
			&observations, &evidence); scanErr != nil {
			return nil, bus.ModuleStatusInternal
		}
		candidates = append(candidates, db2contract.MemoryDedupeCandidatesRow{
			MemoryID:         clampToU64(id),
			MemoryKey:        key,
			MemoryConfidence: confidence,
			UseCount:         clampToU32(useCount),
			ObservationCount: clampToU32(observations),
			EvidenceStrength: evidence,
		})
	}
	if rows.Err() != nil {
		return nil, bus.ModuleStatusInternal
	}
	reply, encodeErr := db2contract.EncodeMemoryDedupeCandidatesReply(candidates)
	if encodeErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

const memoryCorefAuditInsertQuery = `INSERT INTO memory_coref_audit
 (memory_id, session_id, outcome, entity, mode, confidence)
 VALUES ($1, $2, $3, $4, $5, $6)`

// memoryCorefAuditInsert records what a coreference pass decided about one
// memory.
//
// An audit row per decision, including the ones that changed nothing: the
// question this table answers is why a pronoun resolved the way it did, and a
// pass that resolved nothing is as much of an answer as one that did.
//
// The outcome is stored as given. The C substitutes "none" for a NULL, and the
// adapter decodes into a buffer, so through the module an empty outcome is
// stored empty rather than as the word.
func memoryCorefAuditInsert(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	memoryID, sessionID, outcome, entity, mode, confidence, err :=
		db2contract.DecodeMemoryCorefAuditInsertRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	_, execErr := store.Exec(ctx, memoryCorefAuditInsertQuery,
		int64(memoryID), sessionID, outcome, entity, mode, confidence)
	return acknowledgement(execErr == nil,
		db2contract.EncodeMemoryCorefAuditInsertReply)
}

// The same seven columns as the full list, with a floor on the hit count. The
// C leaves the tie-break off this one and orders by hit count alone; confidence
// is added so two patterns hit the same number of times come back in a stable
// order rather than the planner's.
const antiPatternListHotQuery = `SELECT id, pattern, description, source, source_ref,
 hit_count, confidence
 FROM anti_patterns WHERE hit_count >= $1
 ORDER BY hit_count DESC, confidence DESC LIMIT $2`

// antiPatternListHot lists anti-patterns that have caught something often
// enough to be worth acting on.
//
// The threshold is the caller's, and zero means everything -- which makes this
// the same answer as the unfiltered list. That is the C's reading and it is the
// useful one: a caller passing a threshold it computed does not want an empty
// answer when the computation says zero.
func antiPatternListHot(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	threshold, err := db2contract.DecodeAntiPatternListHotRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	ceiling := db2contract.AntiPatternListHotMaxRows
	rows, queryErr := store.Query(ctx, antiPatternListHotQuery,
		int64(threshold), int64(ceiling))
	if queryErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	defer rows.Close()

	patterns := make([]db2contract.AntiPatternListHotRow, 0, 16)
	for rows.Next() {
		var id, hits int64
		var pattern, description, source, sourceRef string
		var confidence float64
		if scanErr := rows.Scan(&id, &pattern, &description, &source,
			&sourceRef, &hits, &confidence); scanErr != nil {
			return nil, bus.ModuleStatusInternal
		}
		patterns = append(patterns, db2contract.AntiPatternListHotRow{
			AntiPatternID:      clampToU64(id),
			HitCount:           clampToU32(hits),
			Confidence:         confidence,
			Pattern:            pattern,
			PatternDescription: description,
			PatternSource:      source,
			SourceRef:          sourceRef,
		})
	}
	if rows.Err() != nil {
		return nil, bus.ModuleStatusInternal
	}
	reply, encodeErr := db2contract.EncodeAntiPatternListHotReply(patterns)
	if encodeErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

// The three answers this operation can give.
const (
	trustWritten   uint32 = 0
	trustNoProject uint32 = 1
	trustFailed    uint32 = 2
)

// The trust write, the epoch bump and the audit row, in that order.
//
// The epoch is bumped only on a real transition, which the C explains: a no-op
// re-assert should not invalidate the frequency model that reads the epoch. The
// audit row is written either way, so re-asserting the same trust is recorded
// as having happened with the epoch unchanged on both sides.
const (
	crossRepoTrustUpdateQuery = `UPDATE projects SET trust = $2 WHERE name = $1`
	// FOR UPDATE, which the C does without: it reads the prior trust, decides
	// whether that is a transition, and then writes, with nothing holding the
	// row in between. Two operators asserting opposite trust at once could both
	// read the same prior, both call it a change, and bump the epoch twice for
	// one transition. The lock closes that, and the C's own note -- that this is
	// a single-tenant admin write -- is the reason it was tolerable rather than
	// a reason it is correct.
	crossRepoTrustReadQuery = `SELECT trust FROM projects WHERE name = $1 FOR UPDATE`
	crossRepoEpochQuery     = `SELECT trust_epoch, repo_set_hash
 FROM cross_repo_meta WHERE id = 1`
	crossRepoEpochBumpQuery = `UPDATE cross_repo_meta
 SET trust_epoch = trust_epoch + 1 WHERE id = 1 RETURNING trust_epoch`
	crossRepoTrustAuditQuery = `INSERT INTO cross_repo_trust_audit
 (project, actor, prior_trust, new_trust, trust_epoch_before, trust_epoch_after,
  repo_set_hash, request_id)
 VALUES ($1, $2, $3, $4, $5, $6, $7, $8)`
)

// crossRepoSetTrust marks a project trusted or untrusted, and records who did
// it.
//
// Trust decides whose code counts toward the cross-repository frequency model,
// so the epoch travels with it: a consumer holding a model built under an older
// epoch knows to rebuild. Both epochs go into the audit row, equal on a no-op,
// which is how the log distinguishes a re-assert from a change.
//
// Only two values are accepted. A trust level nobody recognises would read as
// untrusted everywhere it is compared, which is a quiet answer to a question an
// operator asked explicitly.
func crossRepoSetTrust(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	project, newTrust, actor, requestID, err :=
		db2contract.DecodeCrossRepoSetTrustRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	if newTrust != "trusted" && newTrust != "untrusted" {
		return trustReply(trustFailed, "", 0)
	}

	prior := ""
	changed := false
	result := trustWritten
	txErr := store.InTx(ctx, func(tx Store) error {
		if scanErr := tx.QueryRow(ctx, crossRepoTrustReadQuery, project).
			Scan(&prior); scanErr != nil {
			// No such project. Not a failure -- an operator naming one that
			// does not exist gets told that rather than told nothing.
			result = trustNoProject
			return errNoSuchProject
		}
		changed = prior != newTrust

		var epochBefore int64
		var repoSetHash string
		if scanErr := tx.QueryRow(ctx, crossRepoEpochQuery).
			Scan(&epochBefore, &repoSetHash); scanErr != nil {
			return scanErr
		}
		epochAfter := epochBefore

		if changed {
			if _, execErr := tx.Exec(ctx, crossRepoTrustUpdateQuery,
				project, newTrust); execErr != nil {
				return execErr
			}
			if scanErr := tx.QueryRow(ctx, crossRepoEpochBumpQuery).
				Scan(&epochAfter); scanErr != nil {
				return scanErr
			}
		}
		_, execErr := tx.Exec(ctx, crossRepoTrustAuditQuery, project, actor,
			prior, newTrust, epochBefore, epochAfter, repoSetHash, requestID)
		return execErr
	})
	if txErr != nil && result != trustNoProject {
		result, prior, changed = trustFailed, "", false
	}
	if result == trustNoProject {
		prior, changed = "", false
	}
	changedFlag := uint32(0)
	if changed {
		changedFlag = 1
	}
	return trustReply(result, prior, changedFlag)
}

func trustReply(result uint32, prior string, changed uint32) ([]byte, bus.ModuleStatus) {
	reply, err := db2contract.EncodeCrossRepoSetTrustReply(result, prior, changed)
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

// errNoSuchProject rolls the transaction back without treating a missing
// project as a failure.
var errNoSuchProject = errors.New("db2: no such project")

// The conflict guard is three conditions and the C's note explains each.
//
// A revoked row is untouchable, so a redeem can never resurrect a revoked
// certificate. The issuer and serial must match what is stored -- unless what
// is stored is empty, which is the shape a legacy backfill leaves behind. That
// exception exists because requiring equality alone left a placeholder
// permanently un-upgradable: the first real enrolment of the same fingerprint
// matched the conflict, failed the guard, updated nothing and returned no row.
// A backfill whose whole purpose is to be superseded must not block its own
// supersession.
//
// The SET writes the issuer and serial back, so upgrading a placeholder
// actually records them; without that the row stays empty and every later
// enrolment takes the placeholder path again.
//
// authority_id is deliberately not reset. It is the anchor the authority
// resolve hands out, and changing it on re-enrolment would strand whoever holds
// it.
const enrollmentInsertQuery = `INSERT INTO kb_enrollments
 (scope, fingerprint, serial, expires_at, legacy, authority_id,
  cert_issuer, cert_serial_norm)
 VALUES ($1, $2, $3, $4, $5, $6, $7, $8)
 ON CONFLICT (fingerprint) DO UPDATE SET
  scope = EXCLUDED.scope, serial = EXCLUDED.serial,
  expires_at = EXCLUDED.expires_at, cert_issuer = EXCLUDED.cert_issuer,
  cert_serial_norm = EXCLUDED.cert_serial_norm
 WHERE kb_enrollments.revoked_at = ''
   AND (kb_enrollments.cert_issuer = ''
     OR kb_enrollments.cert_issuer = EXCLUDED.cert_issuer)
   AND (kb_enrollments.cert_serial_norm = ''
     OR kb_enrollments.cert_serial_norm = EXCLUDED.cert_serial_norm)
 RETURNING id`

// enrollmentInsert records a redeemed client certificate.
//
// The serial is written twice, into serial and cert_serial_norm, because the
// C binds the normalized value to both. The pair of issuer and normalized
// serial is the revocation key; the plain serial column predates it.
//
// A conflict that fails the guard updates nothing and returns no row, which
// answers unacknowledged with no identifier. That is the correct answer to
// "re-enrol this fingerprint": something else holds it, and the caller must not
// believe it now owns the enrolment.
func enrollmentInsert(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	scope, fingerprint, issuer, serialNorm, expiresAt, legacy, err :=
		db2contract.DecodeEnrollmentInsertRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	authorityID, idErr := newAuthorityID()
	if idErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	legacyFlag := int64(0)
	if legacy != 0 {
		legacyFlag = 1
	}
	var enrollmentID int64
	acknowledged := true
	if scanErr := store.QueryRow(ctx, enrollmentInsertQuery, scope, fingerprint,
		serialNorm, expiresAt, legacyFlag, authorityID, issuer, serialNorm).
		Scan(&enrollmentID); scanErr != nil {
		acknowledged, enrollmentID = false, 0
	}
	if enrollmentID < 0 {
		enrollmentID = 0
	}
	reply, encodeErr := db2contract.EncodeEnrollmentInsertReply(
		boolToU32(acknowledged), uint64(enrollmentID))
	if encodeErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

func boolToU32(value bool) uint32 {
	if value {
		return 1
	}
	return 0
}
