package families

import (
	"bytes"
	"context"
	"encoding/hex"
	"errors"
	"log"

	store "github.com/JBailes/aimee/server-go/modules/aimee"
)

// EventMgmtJWKS is the management JWKS cache: the signed key set the management
// plane fetched, kept so a restart does not have to re-fetch before it can
// verify anything.
const (
	EventMgmtJWKS uint32 = 11793
	StageMgmtJWKS uint32 = 17

	opMgmtJWKSRead       uint32 = 1
	opMgmtJWKSGeneration uint32 = 2
	opMgmtJWKSInstall    uint32 = 3
)

// Install outcomes. Like the jti stores, these ride in the reply's field rather
// than the wire status: 0 means the cache now holds this envelope, 1 means it
// already holds a DIFFERENT one and refused to replace it.
const (
	jwksInstalled = 0
	jwksConflict  = 1
)

// Bounds from mgmt_jwks_cache.h. They are re-checked here because the module
// must refuse what the store's CHECK constraints would reject anyway, and a
// refusal is a better answer than a constraint error.
const (
	jwksEnvelopeMax = 3071
	jwksBytesMax    = 1023
	sha256Bytes     = 32
)

const (
	jwksReadSQL = `SELECT generation, envelope_bytes, valid_from, valid_until,
	                      envelope_sha256, manifest_sha256, trust_bundle_sha256
	                 FROM server_management_jwks_cache
	                WHERE singleton`

	jwksGenerationSQL = `SELECT generation FROM server_management_jwks_cache WHERE singleton`

	jwksDigestsSQL = `SELECT envelope_sha256, trust_bundle_sha256
	                    FROM server_management_jwks_cache
	                   WHERE singleton`

	jwksInsertSQL = `INSERT INTO server_management_jwks_cache
	                     (singleton, generation, valid_from, valid_until, jwks_bytes,
	                      envelope_bytes, envelope_sha256, manifest_sha256,
	                      trust_bundle_sha256, fetched_at)
	                 VALUES (true, 1, $1, $2, $3, $4, $5, $6, $7, $8)`
)

// jwksRead is op 1: the whole cached row, or MISSING when nothing is cached.
//
// A cold cache is MISSING and a broken store is FAILED, and the two must not be
// merged: a caller told "nothing cached" will go and fetch, while one told
// "failed" will not act on an absence that was never established.
func jwksRead(ctx context.Context, q store.Queryer, _ []string) (uint32, []string, error) {
	var (
		generation, validFrom, validUntil int64
		envelope                          string
		envDigest, manDigest, bundle      []byte
	)
	err := q.QueryRow(ctx, jwksReadSQL).Scan(&generation, &envelope, &validFrom, &validUntil,
		&envDigest, &manDigest, &bundle)
	switch {
	case store.IsNoRows(err):
		return store.StatusMissing, nil, nil
	case err != nil:
		return 0, nil, err
	}
	// The C refused a row whose envelope did not fit its buffer rather than
	// returning a truncated one. The bound is the store's now, so a row that
	// fails it is a corrupt row rather than one this process cannot carry --
	// but it is still not something to hand back half of.
	if envelope == "" || len(envelope) > jwksEnvelopeMax {
		return 0, nil, errors.New("aimee: cached envelope is empty or over-long")
	}
	if len(envDigest) != sha256Bytes || len(manDigest) != sha256Bytes || len(bundle) != sha256Bytes {
		return 0, nil, errors.New("aimee: cached digest is not 32 bytes")
	}
	return store.StatusOK, []string{
		store.I64toa(generation),
		store.I64toa(validFrom),
		store.I64toa(validUntil),
		envelope,
		hex.EncodeToString(envDigest),
		hex.EncodeToString(manDigest),
		hex.EncodeToString(bundle),
	}, nil
}

// jwksGeneration is op 2: just the generation, for a caller deciding whether to
// re-read the rest.
func jwksGeneration(ctx context.Context, q store.Queryer, _ []string) (uint32, []string, error) {
	var generation int64
	switch err := q.QueryRow(ctx, jwksGenerationSQL).Scan(&generation); {
	case store.IsNoRows(err):
		return store.StatusMissing, nil, nil
	case err != nil:
		return 0, nil, err
	}
	return store.StatusOK, []string{store.I64toa(generation)}, nil
}

// decodeDigest parses one hex-encoded sha256 from the wire.
func decodeDigest(field string) ([]byte, bool) {
	raw, err := hex.DecodeString(field)
	if err != nil || len(raw) != sha256Bytes {
		return nil, false
	}
	return raw, true
}

// jwksInstall is op 3: install the fetched key set, once.
//
// It is install-once by design, not upsert. If a row is already cached the only
// question asked is whether it is the SAME envelope and trust bundle; if it is,
// the call succeeds having changed nothing, and if it is not, the call reports
// a conflict and leaves the cached one alone. Silently replacing a signed key
// set with a different signed key set is exactly the write this must not make.
func jwksInstall(ctx context.Context, db store.DB, f []string) (uint32, []string, error) {
	validFrom, okFrom := store.Atoi64(f[0])
	validUntil, okUntil := store.Atoi64(f[1])
	fetchedAt, okFetched := store.Atoi64(f[2])
	if !okFrom || !okUntil || !okFetched {
		return store.StatusInvalid, nil, nil
	}
	envDigest, okEnv := decodeDigest(f[5])
	manDigest, okMan := decodeDigest(f[6])
	bundle, okBundle := decodeDigest(f[7])
	if !okEnv || !okMan || !okBundle {
		return store.StatusInvalid, nil, nil
	}
	jwksBytes, err := hex.DecodeString(f[3])
	if err != nil || len(jwksBytes) < 1 || len(jwksBytes) > jwksBytesMax {
		return store.StatusInvalid, nil, nil
	}
	envelope := f[4]
	if envelope == "" || len(envelope) > jwksEnvelopeMax {
		return store.StatusInvalid, nil, nil
	}
	if validUntil <= validFrom || validFrom < 0 || fetchedAt < 0 {
		return store.StatusInvalid, nil, nil
	}

	tx, err := db.Begin(ctx)
	if err != nil {
		return 0, nil, err
	}
	defer func() { _ = tx.Rollback(ctx) }()

	var storedEnv, storedBundle []byte
	err = tx.QueryRow(ctx, jwksDigestsSQL).Scan(&storedEnv, &storedBundle)
	switch {
	case err == nil:
		// A plain comparison, not a constant-time one: both sides are public
		// digests of a signed envelope, and the caller re-verifies the
		// signature before trusting anything read back.
		same := bytes.Equal(storedEnv, envDigest) && bytes.Equal(storedBundle, bundle)
		if !same {
			return store.StatusOK, []string{store.Itoa(jwksConflict)}, nil
		}
		if err := tx.Commit(ctx); err != nil {
			return 0, nil, err
		}
		return store.StatusOK, []string{store.Itoa(jwksInstalled)}, nil
	case !store.IsNoRows(err):
		return 0, nil, err
	}

	if _, err := tx.Exec(ctx, jwksInsertSQL, validFrom, validUntil, jwksBytes, envelope,
		envDigest, manDigest, bundle, fetchedAt); err != nil {
		log.Printf("aimee: jwks install failed: %v", err)
		return 0, nil, err
	}
	if err := tx.Commit(ctx); err != nil {
		return 0, nil, err
	}
	return store.StatusOK, []string{store.Itoa(jwksInstalled)}, nil
}

// MgmtJWKS is the family, ready to be bound to kind 11793.
var MgmtJWKS = store.Family{
	Name:  "mgmt_jwks",
	Event: EventMgmtJWKS,
	Stage: StageMgmtJWKS,
	Ops: map[uint32]store.Op{
		opMgmtJWKSRead:       {Name: "mgmt_jwks_read", Cells: 7, Args: 0, Run: jwksRead},
		opMgmtJWKSGeneration: {Name: "mgmt_jwks_generation", Args: 0, Run: jwksGeneration},
		opMgmtJWKSInstall:    {Name: "mgmt_jwks_install", Args: 8, RunDB: jwksInstall},
	},
}
