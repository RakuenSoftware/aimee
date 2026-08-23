package db2

import (
	"context"
	"encoding/hex"
	"fmt"

	"github.com/JBailes/aimee/server-go/bus"
	db2contract "github.com/JBailes/aimee/server-go/db2"
)

func init() {
	Register(db2contract.StageMemoryLint,
		db2contract.OperationMemoryLint, memoryLint)
	Register(db2contract.StageWitnessCheckpointAnchorCoverage,
		db2contract.OperationWitnessCheckpointAnchorCoverage,
		witnessCheckpointAnchorCoverage)
	Register(db2contract.StageEnrollmentAuthorityResolve,
		db2contract.OperationEnrollmentAuthorityResolve, enrollmentAuthorityResolve)
	Register(db2contract.StageConsoleOidcPut,
		db2contract.OperationConsoleOidcPut, consoleOIDCPut)
}

// Three checks, each with its own limit, run in a fixed order and concatenated.
// The limits are the C's and they sum to a hundred and seventy, which is under
// the reply's own ceiling -- so the overall cap has never truncated a lint run
// and the per-check limits are what actually bound it.
//
// The order matters to a reader rather than to the data: orphans first because
// they are the cheapest to act on, then structural gaps, then references into
// memories nobody believes any more.
const (
	memoryLintOrphansQuery = `SELECT id, key FROM memories
 WHERE id NOT IN (SELECT source_id FROM memory_links)
 AND id NOT IN (SELECT target_id FROM memory_links)
 AND tier IN ('L1','L2','L3')
 ORDER BY tier, key LIMIT 100`
	// The NOT EXISTS correlates on the outer key, so a key that already has a
	// concept memory is not a gap however many other memories share it.
	memoryLintConceptGapsQuery = `SELECT key, COUNT(*) AS cnt FROM memories
 WHERE kind != 'concept'
 GROUP BY key HAVING COUNT(*) >= 3
 AND NOT EXISTS (
   SELECT 1 FROM memories m2
   WHERE m2.key = memories.key AND m2.kind = 'concept'
 )
 ORDER BY cnt DESC LIMIT 20`
	memoryLintStaleRefsQuery = `SELECT ml.source_id, ms.key, mt.key AS target_key, mt.confidence
 FROM memory_links ml
 JOIN memories ms ON ms.id = ml.source_id
 JOIN memories mt ON mt.id = ml.target_id
 WHERE mt.confidence < 0.2
 ORDER BY mt.confidence LIMIT 50`
)

// memoryLint reports what is structurally wrong with the memory graph.
//
// Read-only, and it proposes rather than repairs: an orphan may be a memory
// nothing has linked yet rather than one that lost its links, and a
// low-confidence target may be on its way back up. Deciding which is not this
// operation's job.
//
// A concept gap carries no memory identifier -- the issue is a key with several
// memories and no concept among them, so there is no single memory to point at.
// Zero is what the C writes there and it means "not a memory" rather than
// memory zero, which no row has.
func memoryLint(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	if err := db2contract.DecodeMemoryLintRequest(request); err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	ceiling := db2contract.MemoryLintMaxRows
	issues := make([]db2contract.MemoryLintRow, 0, 32)

	orphans, err := store.Query(ctx, memoryLintOrphansQuery)
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	for orphans.Next() && len(issues) < ceiling {
		var id int64
		var key string
		if scanErr := orphans.Scan(&id, &key); scanErr != nil {
			orphans.Close()
			return nil, bus.ModuleStatusInternal
		}
		issues = append(issues, db2contract.MemoryLintRow{
			LintMemoryID: uint64(id),
			IssueType:    "orphan",
			MemoryKey:    key,
			IssueMessage: "no inbound or outbound links",
		})
	}
	orphans.Close()
	if orphans.Err() != nil {
		return nil, bus.ModuleStatusInternal
	}

	gaps, err := store.Query(ctx, memoryLintConceptGapsQuery)
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	for gaps.Next() && len(issues) < ceiling {
		var key string
		var count int64
		if scanErr := gaps.Scan(&key, &count); scanErr != nil {
			gaps.Close()
			return nil, bus.ModuleStatusInternal
		}
		issues = append(issues, db2contract.MemoryLintRow{
			LintMemoryID: 0,
			IssueType:    "concept_gap",
			MemoryKey:    key,
			IssueMessage: fmt.Sprintf(
				"key appears %d times with no concept memory", count),
		})
	}
	gaps.Close()
	if gaps.Err() != nil {
		return nil, bus.ModuleStatusInternal
	}

	stale, err := store.Query(ctx, memoryLintStaleRefsQuery)
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	for stale.Next() && len(issues) < ceiling {
		var sourceID int64
		var sourceKey, targetKey string
		var confidence float64
		if scanErr := stale.Scan(
			&sourceID, &sourceKey, &targetKey, &confidence); scanErr != nil {
			stale.Close()
			return nil, bus.ModuleStatusInternal
		}
		issues = append(issues, db2contract.MemoryLintRow{
			LintMemoryID: uint64(sourceID),
			IssueType:    "stale_ref",
			MemoryKey:    sourceKey,
			// Two decimal places, as the C formats it. The number is for a
			// person reading the report, and more digits of a confidence
			// nobody set precisely would be false precision.
			IssueMessage: fmt.Sprintf(
				"links to low-confidence target '%s' (confidence=%.2f)",
				targetKey, confidence),
		})
	}
	stale.Close()
	if stale.Err() != nil {
		return nil, bus.ModuleStatusInternal
	}

	reply, encodeErr := db2contract.EncodeMemoryLintReply(issues)
	if encodeErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

// array_agg rather than an aggregate over the column, because PostgreSQL has no
// min(bytea): the sample is the first key id in sequence order, taken from the
// aggregated array. The C carries the same note and the same workaround.
//
// The comparison is <>, so this counts checkpoints signed by anything other
// than the key the caller holds -- which is the question being asked. A count
// of zero means every checkpoint in the log is anchored to that key.
const witnessAnchorCoverageQuery = `SELECT count(*),
 encode((array_agg(signer_key_id ORDER BY seq))[1], 'hex')
 FROM kb_vault_witness_checkpoint WHERE signer_key_id <> $1`

// witnessSignerKeyIDLen is the raw length of a signer key id, in bytes.
const witnessSignerKeyIDLen = 16

// witnessCheckpointAnchorCoverage answers how much of the witness log is signed
// by a key other than the one the caller holds.
//
// A non-zero count means the log contains checkpoints this anchor cannot
// verify, which is what an operator needs to see before trusting the chain --
// and the sample says which key to go and find.
//
// The key id is decoded strictly: sixteen raw bytes, thirty-two hex characters,
// and anything else fails the request rather than contributing zero nibbles. A
// key id that is nearly right is not a key id, and a lenient decode would
// answer "everything is unanchored" for a typo.
func witnessCheckpointAnchorCoverage(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	keyIDHex, err := db2contract.DecodeWitnessCheckpointAnchorCoverageRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	keyID, decodeErr := hex.DecodeString(keyIDHex)
	if decodeErr != nil || len(keyID) != witnessSignerKeyIDLen {
		// Not read rather than invalid: the C answers a well-formed reply
		// saying it could not read coverage, and a caller checking an anchor
		// wants that answer rather than a rejected request.
		return unreadableCoverage()
	}

	var unknown int64
	var sample *string
	if scanErr := store.QueryRow(ctx, witnessAnchorCoverageQuery, keyID).
		Scan(&unknown, &sample); scanErr != nil {
		return unreadableCoverage()
	}
	if unknown < 0 {
		unknown = 0
	}
	reply, encodeErr := db2contract.EncodeWitnessCheckpointAnchorCoverageReply(
		1, uint64(unknown), text(sample))
	if encodeErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

// unreadableCoverage is the reply for a coverage question that could not be
// answered: nothing read, nothing unknown, no sample. Zero unknown checkpoints
// alongside a zero read flag is not "the log is fully anchored" -- the flag is
// what distinguishes them, and a caller ignoring it reads a failure as a clean
// bill of health.
func unreadableCoverage() ([]byte, bus.ModuleStatus) {
	reply, err := db2contract.EncodeWitnessCheckpointAnchorCoverageReply(0, 0, "")
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

// All three of fingerprint, issuer and normalized serial have to match, and the
// enrollment has to be active and unrevoked. The issuer and serial pair is the
// revocation key: a certificate can be reissued with the same fingerprint
// algorithm and a different serial, and matching on the fingerprint alone would
// resolve a revoked certificate through its replacement.
//
// revoked_at is compared against the empty string rather than checked for NULL,
// because the column is NOT NULL with an empty default -- absence is spelled as
// emptiness here.
const enrollmentAuthorityResolveQuery = `SELECT authority_id FROM kb_enrollments
 WHERE fingerprint = $1 AND cert_issuer = $2 AND cert_serial_norm = $3
 AND state = 'active' AND revoked_at = ''`

// enrollmentAuthorityResolveIDLen is the exact length an authority id has.
const enrollmentAuthorityResolveIDLen = 32

// enrollmentAuthorityResolve answers which authority enrolled a certificate.
//
// An authority id of any length other than thirty-two is treated as not found.
// The C refuses it too: the identifier is a fixed-width hex value, and a row
// carrying something else is a row written by something that did not know the
// format -- resolving it would hand a caller an authority that cannot be looked
// up anywhere else.
func enrollmentAuthorityResolve(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	fingerprint, issuer, serial, err :=
		db2contract.DecodeEnrollmentAuthorityResolveRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	var authorityID string
	found := uint32(1)
	if scanErr := store.QueryRow(ctx, enrollmentAuthorityResolveQuery,
		fingerprint, issuer, serial).Scan(&authorityID); scanErr != nil {
		if !rowAbsent(scanErr) {
			return nil, bus.ModuleStatusInternal
		}
		found, authorityID = 0, ""
	}
	if len(authorityID) != enrollmentAuthorityResolveIDLen {
		found, authorityID = 0, ""
	}
	reply, encodeErr := db2contract.EncodeEnrollmentAuthorityResolveReply(
		found, authorityID)
	if encodeErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

// A singleton keyed on id = 1: there is one console and one identity provider
// behind it, so the upsert replaces the configuration rather than adding a
// second one nothing would choose between.
const consoleOIDCPutQuery = `INSERT INTO kb_console_oidc
 (id, issuer, audience, jwks_url, admin_claim, admin_values, updated_at)
 VALUES (1, $1, $2, $3, $4, $5, pg_now_text())
 ON CONFLICT (id) DO UPDATE SET
  issuer = EXCLUDED.issuer, audience = EXCLUDED.audience,
  jwks_url = EXCLUDED.jwks_url, admin_claim = EXCLUDED.admin_claim,
  admin_values = EXCLUDED.admin_values, updated_at = EXCLUDED.updated_at`

// consoleOIDCPut stores the console's identity provider settings.
//
// Every field is replaced together, including the ones the caller left empty.
// That is the C's behaviour and it is the safer of the two readings: a partial
// update that kept a previous admin claim while replacing the issuer would
// grant administrative access on the strength of a claim from a different
// provider.
func consoleOIDCPut(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	issuer, audience, jwksURL, adminClaim, adminValues, err :=
		db2contract.DecodeConsoleOidcPutRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	_, execErr := store.Exec(ctx, consoleOIDCPutQuery,
		issuer, audience, jwksURL, adminClaim, adminValues)
	return acknowledgement(execErr == nil, db2contract.EncodeConsoleOidcPutReply)
}
