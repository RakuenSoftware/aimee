package families

import (
	"context"
	store "github.com/JBailes/aimee/server-go/modules/aimee"
	"strconv"
	"unicode/utf8"
)

const sessionCacheLimit = 65536
const sessionCacheGetSQL = `SELECT output, revision FROM context_cache WHERE hash=$1 AND session_id=$2`
const sessionCacheCASSQL = `WITH changed AS (
 UPDATE context_cache SET output=$4, revision=revision+1, created_at=now()
 WHERE hash=$1 AND session_id=$2 AND revision=$3::bigint AND revision<9223372036854775807
 RETURNING revision
), created AS (
 INSERT INTO context_cache(hash,session_id,output,revision)
 SELECT $1,$2,$4,1 WHERE $3::bigint=0
 ON CONFLICT(hash) DO NOTHING RETURNING revision
) SELECT revision FROM changed UNION ALL SELECT revision FROM created`
const sessionCacheDeleteSQL = `DELETE FROM context_cache WHERE session_id=$1`

func validSessionCacheKey(f []string) bool {
	return len(f) >= 2 && len(f[0]) > 0 && len(f[0]) <= 128 && len(f[1]) > 0 && len(f[1]) <= 256 && utf8.ValidString(f[0]) && utf8.ValidString(f[1])
}
func sessionCacheGet(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	if !validSessionCacheKey(f) {
		return store.StatusInvalid, nil, nil
	}
	var value string
	var revision int64
	err := q.QueryRow(ctx, sessionCacheGetSQL, f[1], f[0]).Scan(&value, &revision)
	if store.IsNoRows(err) {
		return store.StatusMissing, nil, nil
	}
	if err != nil {
		return store.StatusFailed, nil, err
	}
	if len(value) > sessionCacheLimit || revision <= 0 {
		return store.StatusFailed, nil, nil
	}
	return store.StatusOK, []string{value, strconv.FormatInt(revision, 10)}, nil
}

const sessionCacheReceiptSQL = `SELECT output,revision FROM session_cache_receipts WHERE hash=$1 AND session_id=$2 AND nonce=$3`

func sessionCacheReceipt(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	if len(f) != 3 || !validSessionCacheKey(f) || len(f[2]) == 0 || len(f[2]) > 128 || !utf8.ValidString(f[2]) {
		return store.StatusInvalid, nil, nil
	}
	var value string
	var revision int64
	err := q.QueryRow(ctx, sessionCacheReceiptSQL, f[1], f[0], f[2]).Scan(&value, &revision)
	if store.IsNoRows(err) {
		return store.StatusMissing, nil, nil
	}
	if err != nil {
		return store.StatusFailed, nil, err
	}
	if len(value) > sessionCacheLimit || revision <= 0 {
		return store.StatusFailed, nil, nil
	}
	return store.StatusOK, []string{value, strconv.FormatInt(revision, 10)}, nil
}
func sessionCacheCAS(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	if len(f) != 5 || !validSessionCacheKey(f) || len(f[3]) == 0 || len(f[3]) > 128 || !utf8.ValidString(f[3]) || len(f[4]) > sessionCacheLimit || !utf8.ValidString(f[4]) {
		return store.StatusInvalid, nil, nil
	}
	expected, err := strconv.ParseInt(f[2], 10, 64)
	if err != nil || expected < 0 || strconv.FormatInt(expected, 10) != f[2] {
		return store.StatusInvalid, nil, nil
	}
	// The operation is registered Tx=true. Serializing an identical opaque nonce
	// makes replay exact without imposing domain interpretation on its payload.
	if _, err = q.Exec(ctx, `SELECT pg_advisory_xact_lock(hashtextextended(json_build_array($1::text,$2::text,$3::text)::text,0))`, f[0], f[1], f[3]); err != nil {
		return store.StatusFailed, nil, err
	}
	var oldExpected, revision int64
	var oldValue string
	err = q.QueryRow(ctx, `SELECT expected_revision,revision,output FROM session_cache_receipts WHERE hash=$1 AND session_id=$2 AND nonce=$3`, f[1], f[0], f[3]).Scan(&oldExpected, &revision, &oldValue)
	if err == nil {
		if oldExpected != expected || oldValue != f[4] {
			return store.StatusInvalid, nil, nil
		}
		return store.StatusOK, []string{strconv.FormatInt(revision, 10)}, nil
	}
	if !store.IsNoRows(err) {
		return store.StatusFailed, nil, err
	}
	err = q.QueryRow(ctx, sessionCacheCASSQL, f[1], f[0], expected, f[4]).Scan(&revision)
	if store.IsNoRows(err) {
		return store.StatusMissing, nil, nil
	}
	if err != nil {
		return store.StatusFailed, nil, err
	}
	if revision <= 0 {
		return store.StatusFailed, nil, nil
	}
	if _, err = q.Exec(ctx, `INSERT INTO session_cache_receipts(hash,session_id,nonce,expected_revision,revision,output) VALUES($1,$2,$3,$4,$5,$6)`, f[1], f[0], f[3], expected, revision, f[4]); err != nil {
		return store.StatusFailed, nil, err
	}
	return store.StatusOK, []string{strconv.FormatInt(revision, 10)}, nil
}
func sessionCacheDelete(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	if len(f) != 1 || len(f[0]) == 0 || len(f[0]) > 128 || !utf8.ValidString(f[0]) {
		return store.StatusInvalid, nil, nil
	}
	if _, err := q.Exec(ctx, sessionCacheDeleteSQL, f[0]); err != nil {
		return store.StatusFailed, nil, err
	}
	return store.StatusOK, nil, nil
}
