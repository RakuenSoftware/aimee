package families

import (
	"context"
	"strings"

	store "github.com/JBailes/aimee/server-go/modules/aimee"
)

// Git branch ownership: which session owns a branch, and which feature branch a
// session is on.
const (
	EventGitOwnership uint32 = 11778
	StageGitOwnership uint32 = 2

	opOwnershipUpsert           uint32 = 1
	opOwnershipDelete           uint32 = 2
	opOwnershipOwnerGet         uint32 = 3
	opOwnershipBranchForSession uint32 = 4
	opOwnershipSessionByPrefix  uint32 = 5
	opFeatureBranchUpsert       uint32 = 6
	opFeatureBranchGet          uint32 = 7
)

const (
	ownershipUpsertSQL = `INSERT INTO branch_ownership (repo_path, branch_name, session_id)
	                           VALUES ($1, $2, $3)
	                      ON CONFLICT (repo_path, branch_name)
	                      DO UPDATE SET session_id = EXCLUDED.session_id`

	ownershipDeleteSQL = `DELETE FROM branch_ownership
	                       WHERE repo_path = $1 AND branch_name = $2`

	ownerGetSQL = `SELECT session_id FROM branch_ownership
	                WHERE repo_path = $1 AND branch_name = $2`

	// ORDER BY makes the answer stable. The C took LIMIT 1 with no ordering, so
	// a session owning two branches in one repo got whichever the scan reached
	// first -- and could get a different one on the next call with no write in
	// between.
	branchForSessionSQL = `SELECT branch_name FROM branch_ownership
	                        WHERE repo_path = $1 AND session_id = $2
	                        ORDER BY branch_name
	                        LIMIT 1`

	// ESCAPE is what makes this a PREFIX match rather than a pattern match. The
	// C built "<prefix>%" by string concatenation and bound it straight into
	// LIKE, so a caller-supplied prefix containing % or _ silently matched more
	// than it asked for -- and a bare '%' matched every session in the table.
	sessionByPrefixSQL = `SELECT session_id FROM branch_ownership
	                       WHERE session_id LIKE $1 ESCAPE '\'
	                       ORDER BY session_id
	                       LIMIT 1`

	featureBranchUpsertSQL = `INSERT INTO session_feature_branch (repo_path, session_id, feature_branch)
	                               VALUES ($1, $2, $3)
	                          ON CONFLICT (repo_path, session_id)
	                          DO UPDATE SET feature_branch = EXCLUDED.feature_branch`

	featureBranchGetSQL = `SELECT feature_branch FROM session_feature_branch
	                        WHERE repo_path = $1 AND session_id = $2`
)

// likePrefix turns a literal prefix into a LIKE pattern that matches it and
// nothing else.
//
// The backslash must be escaped first: escaping it after the metacharacters
// would double the backslashes this function had just introduced.
func likePrefix(prefix string) string {
	escaped := strings.ReplaceAll(prefix, `\`, `\\`)
	escaped = strings.ReplaceAll(escaped, "%", `\%`)
	escaped = strings.ReplaceAll(escaped, "_", `\_`)
	return escaped + "%"
}

// likeContains turns a caller's raw search text into a LIKE pattern matching it
// as a SUBSTRING, escaped so the text is matched literally.
//
// The C wrapped searches in "%...%" by concatenation with no escaping, so a
// query containing % or _ silently matched more than it asked for -- the same
// shape as the prefix bug above, in a different family.
func likeContains(query string) string {
	escaped := strings.ReplaceAll(query, `\`, `\\`)
	escaped = strings.ReplaceAll(escaped, "%", `\%`)
	escaped = strings.ReplaceAll(escaped, "_", `\_`)
	return "%" + escaped + "%"
}

// lookup is the shape every read in this family has: one text answer, or
// MISSING when there is no row.
//
// An empty stored value is MISSING too, matching the C: its reply carried a
// fixed-size buffer, and an empty one was indistinguishable from no row, so
// callers already treat the two the same.
func lookup(ctx context.Context, q store.Queryer, sql string, args ...any) (uint32, []string, error) {
	var value string
	switch err := q.QueryRow(ctx, sql, args...).Scan(&value); {
	case store.IsNoRows(err):
		return store.StatusMissing, nil, nil
	case err != nil:
		return 0, nil, err
	}
	if value == "" {
		return store.StatusMissing, nil, nil
	}
	return store.StatusOK, []string{value}, nil
}

// nonEmpty refuses a request whose identifying fields are blank. The C stage
// checked this before dispatching, and a blank repo path or branch name is a
// key that would collide with every other blank one.
func nonEmpty(fields ...string) bool {
	for _, f := range fields {
		if f == "" {
			return false
		}
	}
	return true
}

func ownershipUpsert(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	if !nonEmpty(f[0], f[1], f[2]) {
		return store.StatusInvalid, nil, nil
	}
	if _, err := q.Exec(ctx, ownershipUpsertSQL, f[0], f[1], f[2]); err != nil {
		return 0, nil, err
	}
	return store.StatusOK, nil, nil
}

// ownershipDelete succeeds whether or not a row was there: the postcondition --
// this branch is unowned -- holds either way, and the C reported success as
// long as the statement ran.
func ownershipDelete(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	if !nonEmpty(f[0], f[1]) {
		return store.StatusInvalid, nil, nil
	}
	if _, err := q.Exec(ctx, ownershipDeleteSQL, f[0], f[1]); err != nil {
		return 0, nil, err
	}
	return store.StatusOK, nil, nil
}

func ownershipOwnerGet(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	if !nonEmpty(f[0], f[1]) {
		return store.StatusInvalid, nil, nil
	}
	return lookup(ctx, q, ownerGetSQL, f[0], f[1])
}

func ownershipBranchForSession(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	if !nonEmpty(f[0], f[1]) {
		return store.StatusInvalid, nil, nil
	}
	return lookup(ctx, q, branchForSessionSQL, f[0], f[1])
}

// ownershipSessionByPrefix resolves an abbreviated session id.
//
// The prefix is matched literally. A caller passing "%" gets no match rather
// than an arbitrary session, which is what the C's unescaped concatenation
// would have handed back.
func ownershipSessionByPrefix(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	if !nonEmpty(f[0]) {
		return store.StatusInvalid, nil, nil
	}
	return lookup(ctx, q, sessionByPrefixSQL, likePrefix(f[0]))
}

func featureBranchUpsert(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	if !nonEmpty(f[0], f[1], f[2]) {
		return store.StatusInvalid, nil, nil
	}
	if _, err := q.Exec(ctx, featureBranchUpsertSQL, f[0], f[1], f[2]); err != nil {
		return 0, nil, err
	}
	return store.StatusOK, nil, nil
}

func featureBranchGet(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	if !nonEmpty(f[0], f[1]) {
		return store.StatusInvalid, nil, nil
	}
	return lookup(ctx, q, featureBranchGetSQL, f[0], f[1])
}

// GitOwnership is the family, ready to be bound to kind 11778.
var GitOwnership = store.Family{
	Name:  "git_ownership",
	Event: EventGitOwnership,
	Stage: StageGitOwnership,
	Ops: map[uint32]store.Op{
		opOwnershipUpsert:           {Name: "ownership_upsert", Args: 3, Tx: true, Run: ownershipUpsert},
		opOwnershipDelete:           {Name: "ownership_delete", Args: 2, Tx: true, Run: ownershipDelete},
		opOwnershipOwnerGet:         {Name: "ownership_owner_get", Args: 2, Run: ownershipOwnerGet},
		opOwnershipBranchForSession: {Name: "ownership_branch_for_session", Args: 2, Run: ownershipBranchForSession},
		opOwnershipSessionByPrefix:  {Name: "ownership_session_by_prefix", Args: 1, Run: ownershipSessionByPrefix},
		opFeatureBranchUpsert:       {Name: "feature_branch_upsert", Args: 3, Tx: true, Run: featureBranchUpsert},
		opFeatureBranchGet:          {Name: "feature_branch_get", Args: 2, Run: featureBranchGet},
	},
}
