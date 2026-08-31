// Package families implements the aimee module served families: the store
// operations behind its nineteen event kinds.
//
// One file per family, each owning its SQL and the rules around it. The wire
// framing, dispatch and transaction discipline are shared, and live one
// directory up.
package families

import (
	"context"
	"log"

	store "github.com/JBailes/aimee/server-go/modules/aimee"
)

// The jti stores' result codes, from server_identity_jti.h and its management
// twin. These are NOT the wire status: every consume answers with wire OK and
// puts the outcome in the reply's single field, which is what the C client
// reads. A non-OK wire status means "the module was unreachable" to that
// client, so reporting a storage failure that way would tell it something
// different from what the C module said.
const (
	jtiOK        = 0
	jtiReplay    = 1
	jtiSaturated = 2
	jtiStorage   = 3
	jtiInvalid   = 4
)

// jtiLiveLimit is SERVER_{IDENTITY,MANAGEMENT}_JTI_LIVE_LIMIT. Both stores use
// the same number and the same saturation rule.
const jtiLiveLimit = 4096

// EventJTIReplay and StageJTIReplay are from the catalog: ref 30's kind block.
const (
	EventJTIReplay uint32 = 11791
	StageJTIReplay uint32 = 15

	opIdentityJTIConsume   uint32 = 1
	opManagementJTIConsume uint32 = 2
)

// asciiToken mirrors the C's ascii_token: the identifier alphabet, bounded.
func asciiToken(s string, min, max int) bool {
	if len(s) < min || len(s) > max {
		return false
	}
	for i := 0; i < len(s); i++ {
		c := s[i]
		switch {
		case c >= 'A' && c <= 'Z', c >= 'a' && c <= 'z', c >= '0' && c <= '9':
		case c == '.' || c == '_' || c == '-':
		default:
			return false
		}
	}
	return true
}

// controlFree mirrors the C's control_free: printable, bounded. It works in
// BYTES, like the C's strnlen-based check, so a multi-byte UTF-8 subject is
// bounded the same way it was before.
func controlFree(s string, min, max int) bool {
	if len(s) < min || len(s) > max {
		return false
	}
	for i := 0; i < len(s); i++ {
		if c := s[i]; c < 0x20 || c == 0x7f {
			return false
		}
	}
	return true
}

// lowercaseHex mirrors the C's lowercase_hex.
func lowercaseHex(s string, min, max int) bool {
	if len(s) < min || len(s) > max {
		return false
	}
	for i := 0; i < len(s); i++ {
		c := s[i]
		if !((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')) {
			return false
		}
	}
	return true
}

func validTier(s string) bool {
	return s == "off" || s == "data" || s == "full"
}

// jtiTimes checks the ordering every token record must satisfy.
func jtiTimes(issuedAt, expiresAt, consumedAt int64) bool {
	return issuedAt >= 0 && issuedAt < expiresAt &&
		consumedAt >= issuedAt && consumedAt < expiresAt
}

// consumeSQL is one store's three statements. The two jti stores differ only in
// their table and columns, so the algorithm is written once and parameterised.
type consumeSQL struct {
	gc     string
	count  string
	insert string
}

// consume is the shared body of both consume operations.
//
// It manages its own transaction because the COMMIT/ROLLBACK decision is part
// of the answer rather than a consequence of it, and the two are not the same
// question here:
//
//   - Saturation COMMITS. The garbage-collection pass that just ran is real
//     work, and throwing it away would mean a saturated store could never
//     recover -- every consume would sweep and then discard the sweep.
//   - A replay ROLLS BACK. The caller is told the jti was already spent, and
//     nothing about that attempt should persist.
//
// The replay guarantee itself comes from the primary key, not from the
// transaction: the C opened BEGIN IMMEDIATE to serialise writers, but a second
// consume of the same jti is refused by the key whatever the isolation level.
// The saturation count is a soft cap and is allowed to race by a small margin,
// exactly as it could before.
func consume(ctx context.Context, db store.DB, sql consumeSQL,
	args []any, consumedAt int64) (uint32, []string, error) {
	fail := func(code int) (uint32, []string, error) {
		return store.StatusOK, []string{store.Itoa(code)}, nil
	}

	tx, err := db.Begin(ctx)
	if err != nil {
		log.Printf("aimee: jti begin failed: %v", err)
		return fail(jtiStorage)
	}
	rollback := func() { _ = tx.Rollback(ctx) }

	// Bounded sweep of expired entries, oldest first. Bounded rather than
	// "delete everything expired" so one consume cannot turn into an unbounded
	// delete on a store that has been idle for a long time.
	if _, err := tx.Exec(ctx, sql.gc, consumedAt); err != nil {
		log.Printf("aimee: jti gc failed: %v", err)
		rollback()
		return fail(jtiStorage)
	}

	var live int64
	if err := tx.QueryRow(ctx, sql.count).Scan(&live); err != nil {
		log.Printf("aimee: jti count failed: %v", err)
		rollback()
		return fail(jtiStorage)
	}
	if live >= jtiLiveLimit {
		// Saturation denies rather than evicting a live entry: dropping an
		// unexpired jti to make room would hand back a replay window.
		if err := tx.Commit(ctx); err != nil {
			log.Printf("aimee: jti commit after saturation failed: %v", err)
			return fail(jtiStorage)
		}
		return fail(jtiSaturated)
	}

	// ON CONFLICT DO NOTHING rather than catching a constraint error: a replay
	// is an expected answer, not an exception, and zero rows affected says so
	// without having to classify a driver error by SQLSTATE.
	tag, err := tx.Exec(ctx, sql.insert, args...)
	if err != nil {
		log.Printf("aimee: jti insert failed: %v", err)
		rollback()
		return fail(jtiStorage)
	}
	if tag.RowsAffected() == 0 {
		rollback()
		return fail(jtiReplay)
	}
	if err := tx.Commit(ctx); err != nil {
		log.Printf("aimee: jti commit failed: %v", err)
		return fail(jtiStorage)
	}
	return fail(jtiOK)
}

var identitySQL = consumeSQL{
	gc: `DELETE FROM server_identity_jti
	      WHERE jti IN (SELECT jti FROM server_identity_jti
	                     WHERE expires_at < $1
	                     ORDER BY expires_at, jti
	                     LIMIT 4096)`,
	count: `SELECT count(*) FROM server_identity_jti`,
	insert: `INSERT INTO server_identity_jti
	             (jti, issuer, kid, audience, subject, team_id, tier, issued_at, expires_at, consumed_at)
	         VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9, $10)
	         ON CONFLICT (jti) DO NOTHING`,
}

var managementSQL = consumeSQL{
	gc: `DELETE FROM server_management_jti
	      WHERE jti IN (SELECT jti FROM server_management_jti
	                     WHERE expires_at < $1
	                     ORDER BY expires_at, jti
	                     LIMIT 4096)`,
	count: `SELECT count(*) FROM server_management_jti`,
	insert: `INSERT INTO server_management_jti
	             (jti, issuer, kid, audience, subject, team_id, capability, peer_issuer,
	              peer_serial, peer_fingerprint, request_sha256, correlation_id,
	              issued_at, expires_at, consumed_at)
	         VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9, $10, $11, $12, $13, $14, $15)
	         ON CONFLICT (jti) DO NOTHING`,
}

// identityConsume is op 1: the data-plane identity token's replay check.
//
// Request fields, in order: jti, issuer, kid, audience, subject, team_id, tier,
// issued_at, expires_at, consumed_at.
func identityConsume(ctx context.Context, db store.DB, f []string) (uint32, []string, error) {
	invalid := func() (uint32, []string, error) {
		return store.StatusOK, []string{store.Itoa(jtiInvalid)}, nil
	}
	teamID, okTeam := store.Atoi64(f[5])
	issuedAt, okIssued := store.Atoi64(f[7])
	expiresAt, okExpires := store.Atoi64(f[8])
	consumedAt, okConsumed := store.Atoi64(f[9])
	if !okTeam || !okIssued || !okExpires || !okConsumed {
		return invalid()
	}
	// The jti floor is 8, matching the server verifier's accepted range, and
	// the tier must be one of the three defined levels -- an unrecognised tier
	// is a corrupt record, not a token to consume.
	if !asciiToken(f[0], 8, 128) || !controlFree(f[1], 1, 255) || !asciiToken(f[2], 1, 64) ||
		!asciiToken(f[3], 1, 127) || !controlFree(f[4], 1, 576) || teamID <= 0 ||
		!validTier(f[6]) || !jtiTimes(issuedAt, expiresAt, consumedAt) {
		return invalid()
	}
	args := []any{f[0], f[1], f[2], f[3], f[4], teamID, f[6], issuedAt, expiresAt, consumedAt}
	return consume(ctx, db, identitySQL, args, consumedAt)
}

// managementConsume is op 2: the management-plane token's replay check. Its jti
// floor is 16 rather than 8, and it carries the peer's certificate identity.
func managementConsume(ctx context.Context, db store.DB, f []string) (uint32, []string, error) {
	invalid := func() (uint32, []string, error) {
		return store.StatusOK, []string{store.Itoa(jtiInvalid)}, nil
	}
	teamID, okTeam := store.Atoi64(f[5])
	issuedAt, okIssued := store.Atoi64(f[12])
	expiresAt, okExpires := store.Atoi64(f[13])
	consumedAt, okConsumed := store.Atoi64(f[14])
	if !okTeam || !okIssued || !okExpires || !okConsumed {
		return invalid()
	}
	if !asciiToken(f[0], 16, 128) || !controlFree(f[1], 1, 255) || !asciiToken(f[2], 1, 64) ||
		!asciiToken(f[3], 1, 127) || !controlFree(f[4], 1, 576) || teamID <= 0 ||
		!asciiToken(f[6], 1, 64) || !controlFree(f[7], 1, 511) ||
		!lowercaseHex(f[8], 1, 79) || !lowercaseHex(f[9], 64, 64) ||
		!lowercaseHex(f[10], 64, 64) || !asciiToken(f[11], 1, 128) ||
		!jtiTimes(issuedAt, expiresAt, consumedAt) {
		return invalid()
	}
	args := []any{f[0], f[1], f[2], f[3], f[4], teamID, f[6], f[7], f[8], f[9], f[10], f[11],
		issuedAt, expiresAt, consumedAt}
	return consume(ctx, db, managementSQL, args, consumedAt)
}

// JTIReplay is the family, ready to be bound to kind 11791.
var JTIReplay = store.Family{
	Name:  "jti_replay",
	Event: EventJTIReplay,
	Stage: StageJTIReplay,
	Ops: map[uint32]store.Op{
		opIdentityJTIConsume: {
			Name: "identity_jti_consume", Args: 10, RunDB: identityConsume,
		},
		opManagementJTIConsume: {
			Name: "management_jti_consume", Args: 15, RunDB: managementConsume,
		},
	},
}
