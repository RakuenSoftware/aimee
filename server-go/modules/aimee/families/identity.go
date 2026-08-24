package families

import (
	"context"
	"strings"

	store "github.com/JBailes/aimee/server-go/modules/aimee"
)

// The remote-client identity family: who owns this server, and which bearer
// tokens and client certificates speak for them.
const (
	EventIdentity uint32 = 11789
	StageIdentity uint32 = 13

	opRemoteClientClaim   uint32 = 1
	opRemoteClientAbandon uint32 = 2
	opRemoteClientBind    uint32 = 3
	opRemoteClientTier    uint32 = 4
)

// Claim outcomes, from remote_client_grant.h.
const (
	claimNew          = 0
	claimUnbound      = 1
	claimBound        = 2
	claimOwnedByOther = 3
	claimInvalid      = 4
	claimStorage      = 5
)

// Bind outcomes. Unlike the claim's enum these are the C function's return
// values directly: 1 bound, 0 no such grant, -1 refused or broken, -2 already
// bound to a DIFFERENT certificate.
const (
	bindOK          = 1
	bindNoSuchGrant = 0
	bindFailed      = -1
	bindOtherCert   = -2
)

const (
	principalMax    = 255
	bearerHashLen   = 64
	certSerialMax   = 79
	principalPrefix = "webuser:"
)

const (
	firstUserSQL = `SELECT principal FROM remote_first_user WHERE singleton`

	firstUserInsertSQL = `INSERT INTO remote_first_user (singleton, principal, created_at)
	                      VALUES (true, $1, $2)`

	// Prefer a bound grant: once paired, re-running Deploy must not mint a new
	// standing credential. Otherwise return the one pending enrollment, so a
	// browser refresh recovers the same quickstart command rather than a
	// second one.
	grantForPrincipalSQL = `SELECT principal, bearer_sha256, coalesce(cert_serial, ''), tier
	                          FROM remote_client_grants
	                         WHERE principal = $1
	                         ORDER BY (cert_serial IS NOT NULL) DESC, created_at ASC
	                         LIMIT 1`

	grantInsertSQL = `INSERT INTO remote_client_grants
	                      (bearer_sha256, principal, tier, cert_serial, created_at, bound_at)
	                  VALUES ($1, $2, 'full', NULL, $3, NULL)`

	// Only an UNBOUND grant may be abandoned: once a certificate is paired to
	// it, deleting the row would strand a client that is still presenting one.
	abandonSQL = `DELETE FROM remote_client_grants
	               WHERE bearer_sha256 = $1 AND cert_serial IS NULL`

	boundSerialSQL = `SELECT coalesce(cert_serial, '') FROM remote_client_grants
	                   WHERE bearer_sha256 = $1`

	bindSQL = `UPDATE remote_client_grants
	              SET cert_serial = $1, bound_at = $2
	            WHERE bearer_sha256 = $3 AND cert_serial IS NULL`

	tierSQL = `SELECT principal, tier FROM remote_client_grants WHERE cert_serial = $1`
)

// tierRank is the integer the wire carries for a tier name.
func tierRank(tier string) int {
	switch tier {
	case "full":
		return 2
	case "data":
		return 1
	default:
		return 0
	}
}

// printableBounded mirrors the C's text_valid: printable, bounded, in bytes.
func printableBounded(s string, min, max int) bool {
	return controlFree(s, min, max)
}

// certSerialValid mirrors serial_valid: hex, either case, 1..79. Either case
// because a certificate serial is transcribed from the certificate and the C
// accepted both -- unlike the nonce, which is generated here and is lowercase
// by construction.
func certSerialValid(s string) bool {
	if len(s) < 1 || len(s) > certSerialMax {
		return false
	}
	for i := 0; i < len(s); i++ {
		c := s[i]
		if !((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')) {
			return false
		}
	}
	return true
}

// claimReply is the five-cell answer: outcome, then the grant.
func claimReply(result int, principal, bearer, serial string, tier int) (uint32, []string, error) {
	return store.StatusOK, []string{
		store.Itoa(result), principal, bearer, serial, store.Itoa(tier),
	}, nil
}

// remoteClientClaim is op 1: establish or recover this server's owner and their
// standing credential.
//
// The first caller to claim becomes the owner. A different principal claiming
// afterwards is told the server is owned by someone else rather than being
// given a grant -- this is the whole of the first-user protection.
func remoteClientClaim(ctx context.Context, db store.DB, f []string) (uint32, []string, error) {
	principal, bearer := f[0], f[1]
	now, okNow := store.Atoi64(f[2])
	if !printableBounded(principal, 9, principalMax) ||
		!strings.HasPrefix(principal, principalPrefix) ||
		!lowercaseHex(bearer, bearerHashLen, bearerHashLen) || !okNow || now < 0 {
		return claimReply(claimInvalid, "", "", "", 0)
	}

	tx, err := db.Begin(ctx)
	if err != nil {
		return claimReply(claimStorage, "", "", "", 0)
	}
	defer func() { _ = tx.Rollback(ctx) }()

	var owner string
	err = tx.QueryRow(ctx, firstUserSQL).Scan(&owner)
	switch {
	case err == nil:
		if owner != principal {
			if err := tx.Commit(ctx); err != nil {
				return claimReply(claimStorage, "", "", "", 0)
			}
			return claimReply(claimOwnedByOther, "", "", "", 0)
		}
	case store.IsNoRows(err):
		if _, err := tx.Exec(ctx, firstUserInsertSQL, principal, now); err != nil {
			return claimReply(claimStorage, "", "", "", 0)
		}
	default:
		return claimReply(claimStorage, "", "", "", 0)
	}

	var gotPrincipal, gotBearer, gotSerial, gotTier string
	err = tx.QueryRow(ctx, grantForPrincipalSQL, principal).
		Scan(&gotPrincipal, &gotBearer, &gotSerial, &gotTier)
	switch {
	case err == nil:
		result := claimUnbound
		if gotSerial != "" {
			result = claimBound
		}
		if err := tx.Commit(ctx); err != nil {
			return claimReply(claimStorage, "", "", "", 0)
		}
		return claimReply(result, gotPrincipal, gotBearer, gotSerial, tierRank(gotTier))
	case !store.IsNoRows(err):
		return claimReply(claimStorage, "", "", "", 0)
	}

	if _, err := tx.Exec(ctx, grantInsertSQL, bearer, principal, now); err != nil {
		return claimReply(claimStorage, "", "", "", 0)
	}
	if err := tx.Commit(ctx); err != nil {
		return claimReply(claimStorage, "", "", "", 0)
	}
	// A fresh grant is always 'full' and always unbound, which is why the tier
	// is spelled here rather than read back.
	return claimReply(claimNew, principal, bearer, "", tierRank("full"))
}

// remoteClientAbandon is op 2: drop an enrollment that was never completed.
func remoteClientAbandon(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	if !lowercaseHex(f[0], bearerHashLen, bearerHashLen) {
		return store.StatusInvalid, nil, nil
	}
	// A grant that is already bound matches no row, and that is not an error:
	// the C reported success as long as the statement ran, because the
	// postcondition -- no pending enrollment for this bearer -- holds either
	// way.
	if _, err := q.Exec(ctx, abandonSQL, f[0]); err != nil {
		return 0, nil, err
	}
	return store.StatusOK, nil, nil
}

// bindReply is the single-cell answer for a bind.
func bindReply(code int) (uint32, []string, error) {
	return store.StatusOK, []string{store.Itoa(code)}, nil
}

// remoteClientBind is op 3: pair a client certificate to a standing grant.
//
// Re-binding the SAME certificate succeeds, so a retried enrollment is not an
// error. Binding a DIFFERENT one is refused: the grant is already speaking for
// a certificate, and quietly repointing it would transfer the principal's
// authority to another key.
func remoteClientBind(ctx context.Context, db store.DB, f []string) (uint32, []string, error) {
	bearer, serial := f[0], f[1]
	now, okNow := store.Atoi64(f[2])
	if !lowercaseHex(bearer, bearerHashLen, bearerHashLen) || !certSerialValid(serial) ||
		!okNow || now < 0 {
		return bindReply(bindFailed)
	}

	tx, err := db.Begin(ctx)
	if err != nil {
		return bindReply(bindFailed)
	}
	defer func() { _ = tx.Rollback(ctx) }()

	var bound string
	err = tx.QueryRow(ctx, boundSerialSQL, bearer).Scan(&bound)
	switch {
	case store.IsNoRows(err):
		if err := tx.Commit(ctx); err != nil {
			return bindReply(bindFailed)
		}
		return bindReply(bindNoSuchGrant)
	case err != nil:
		return bindReply(bindFailed)
	}
	if bound != "" {
		result := bindOtherCert
		if bound == serial {
			result = bindOK
		}
		if err := tx.Commit(ctx); err != nil {
			return bindReply(bindFailed)
		}
		return bindReply(result)
	}

	tag, err := tx.Exec(ctx, bindSQL, serial, now, bearer)
	if err != nil {
		return bindReply(bindFailed)
	}
	if tag.RowsAffected() != 1 {
		// The row was unbound a statement ago. Anything other than exactly one
		// row means a concurrent writer got there first, and reporting success
		// would claim a pairing this call did not make.
		return bindReply(bindFailed)
	}
	if err := tx.Commit(ctx); err != nil {
		return bindReply(bindFailed)
	}
	return bindReply(bindOK)
}

// remoteClientTier is op 4: which principal a client certificate speaks for,
// and at what tier.
//
// It always answers OK. An unknown certificate is ("", 0) -- the empty
// principal IS the "no such grant" answer, and the caller reads it as one.
func remoteClientTier(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	if !certSerialValid(f[0]) {
		return store.StatusOK, []string{"", store.Itoa(-1)}, nil
	}
	var principal, tier string
	switch err := q.QueryRow(ctx, tierSQL, f[0]).Scan(&principal, &tier); {
	case store.IsNoRows(err):
		return store.StatusOK, []string{"", store.Itoa(0)}, nil
	case err != nil:
		return store.StatusOK, []string{"", store.Itoa(-1)}, nil
	}
	return store.StatusOK, []string{principal, store.Itoa(tierRank(tier))}, nil
}

// Identity is the family, ready to be bound to kind 11789.
var Identity = store.Family{
	Name:  "identity",
	Event: EventIdentity,
	Stage: StageIdentity,
	Ops: map[uint32]store.Op{
		opRemoteClientClaim:   {Name: "remote_client_claim", Cells: 5, Args: 3, RunDB: remoteClientClaim},
		opRemoteClientAbandon: {Name: "remote_client_abandon", Args: 1, Tx: true, Run: remoteClientAbandon},
		opRemoteClientBind:    {Name: "remote_client_bind", Args: 3, RunDB: remoteClientBind},
		opRemoteClientTier:    {Name: "remote_client_tier", Cells: 2, Args: 1, Run: remoteClientTier},
	},
}
