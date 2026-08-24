package families

import (
	"context"

	store "github.com/JBailes/aimee/server-go/modules/aimee"
)

// User memories: durable facts and preferences about the person, recalled into
// a prompt by section.

// The recall sections the wire can ask for.
const (
	userRecallIdentity    = 1
	userRecallPreferences = 2
)

const (
	// Both recalls share everything but which rows they select, so the shared
	// half is written once.
	//
	// The key patterns here are fixed literals rather than anything a caller
	// supplies, so there is nothing to escape: the % is meant as a wildcard.
	//
	// valid_until is part of that shared half, and the C consulted it NOWHERE --
	// the column was written by nothing and read by nothing, so a memory given
	// an expiry was recalled forever regardless of it. An unconsulted expiry is
	// worse than an absent one: a caller that set it believes the fact stops
	// being asserted, and the store keeps asserting it. Both recalls now honour
	// it, and a NULL still means "no expiry".
	userRecallLiveSQL = `WHERE tier IN ('L2', 'L3', 'L4', 'L5')
	                       AND lifecycle_state = 'active'
	                       AND (valid_until IS NULL OR valid_until > now()) `

	userRecallIdentitySQL = `SELECT id, tier, kind, key, content
	                           FROM user_memories ` + userRecallLiveSQL + `
	                            AND kind = 'fact'
	                            AND (key LIKE 'identity:%' OR key LIKE 'name:%'
	                              OR key LIKE 'role:%'     OR key LIKE 'user:%'
	                              OR key LIKE 'self:%')
	                          ORDER BY confidence DESC, id DESC
	                          LIMIT $1`

	userRecallPreferencesSQL = `SELECT id, tier, kind, key, content
	                              FROM user_memories ` + userRecallLiveSQL + `
	                               AND kind = 'preference'
	                             ORDER BY confidence DESC, id DESC
	                             LIMIT $1`

	userMemoryAnySQL = `SELECT EXISTS (SELECT 1 FROM user_memories)`

	// Insert or update by the UNIQUE(kind, key) constraint. An upsert
	// deliberately revives a retired memory: writing a fact again is a statement
	// that it holds, so lifecycle_state returns to active.
	//
	// valid_until is cleared for the same reason. Reviving a memory while
	// leaving a lapsed expiry on it would set lifecycle_state = 'active' and
	// still have the recall skip it -- active and invisible, which is the most
	// confusing state the table can hold.
	userMemoryUpsertSQL = `INSERT INTO user_memories
	                           (kind, tier, key, content, confidence, source_session, updated_at)
	                       VALUES ($1, $2, $3, $4, $5, $6, now())
	                       ON CONFLICT (kind, key) DO UPDATE SET
	                           content         = EXCLUDED.content,
	                           tier            = EXCLUDED.tier,
	                           confidence      = EXCLUDED.confidence,
	                           source_session  = EXCLUDED.source_session,
	                           lifecycle_state = 'active',
	                           valid_until     = NULL,
	                           updated_at      = now()`
)

func userMemoryRow(scan func(...any) error) ([]string, error) {
	var (
		id                       int64
		tier, kind, key, content string
	)
	if err := scan(&id, &tier, &kind, &key, &content); err != nil {
		return nil, err
	}
	return []string{store.I64toa(id), tier, kind, key, content}, nil
}

// userMemoryListRecall reads one section's memories.
//
// An unknown section is refused rather than answered with an empty list: the C
// returned zero rows for it, which a caller cannot tell apart from a section
// that is genuinely empty, and the two mean very different things.
func userMemoryListRecall(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	section, ok := store.Atoi64(f[0])
	if !ok {
		return store.StatusInvalid, nil, nil
	}
	max, ok := boundedMax(f[1])
	if !ok {
		return store.StatusInvalid, nil, nil
	}
	var sql string
	switch section {
	case userRecallIdentity:
		sql = userRecallIdentitySQL
	case userRecallPreferences:
		sql = userRecallPreferencesSQL
	default:
		return store.StatusInvalid, nil, nil
	}
	return collect(ctx, q, sql, 5, userMemoryRow, max)
}

// userMemoryAny answers whether the store holds any memory at all.
//
// EXISTS rather than the C's "SELECT 1 ... LIMIT 1" read as a row-or-no-row:
// the question is a boolean, so it is answered as one and an empty store is an
// answer rather than a miss.
func userMemoryAny(ctx context.Context, q store.Queryer, _ []string) (uint32, []string, error) {
	var any bool
	if err := q.QueryRow(ctx, userMemoryAnySQL).Scan(&any); err != nil {
		return store.StatusFailed, nil, err
	}
	return store.StatusOK, []string{store.Btoa(any)}, nil
}

func userMemoryUpsert(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	kind, tier, key := f[0], f[1], f[2]
	if kind == "" || key == "" {
		return store.StatusInvalid, nil, nil
	}
	if tier == "" {
		tier = "L2"
	}
	confidence, ok := store.Atof(f[4])
	if !ok {
		return store.StatusInvalid, nil, nil
	}
	if _, err := q.Exec(ctx, userMemoryUpsertSQL,
		kind, tier, key, f[3], confidence, f[5]); err != nil {
		return store.StatusFailed, nil, err
	}
	return store.StatusOK, nil, nil
}
