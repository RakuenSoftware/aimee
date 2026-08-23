package db2

import "context"

// The unique index the projection's edge upsert needs, which the schema does
// not declare.
//
// schema.sql deliberately leaves it out: a legacy instance may hold duplicate
// triples, and a plain CREATE UNIQUE INDEX in the schema would fail on every
// startup for as long as it did. The C builds it best-effort at module init
// instead, and its own comment records what happens when the build fails --
// "code-graph projection ON CONFLICT will no-op until duplicate triples are
// deduped".
//
// This module has no init of its own, so the step is exported for the host to
// call. Until it is called, projection_sync_project fails on a store that has
// never run the C module -- which is how the missing precondition was found:
// the operation's live probe could not insert an edge.
const entityEdgeUniqueIndexBuildQuery = `CREATE UNIQUE INDEX IF NOT EXISTS
 idx_ee_unique_triple ON entity_edges (source, relation, target)`

// EnsureSchemaIndexes builds the indexes this module needs and the schema does
// not declare.
//
// Best-effort by design, exactly as the C's is: an instance holding duplicate
// triples cannot have the index built and must be deduped first, and refusing
// to start over it would be worse than running without the projection. The
// error is returned rather than swallowed so a host can log it; a host that
// ignores it gets the C's behaviour.
//
// Safe to call more than once -- IF NOT EXISTS -- and safe to call before any
// request is served, which is where it belongs.
func EnsureSchemaIndexes(ctx context.Context, store Store) error {
	if store == nil {
		return nil
	}
	_, err := store.Exec(ctx, entityEdgeUniqueIndexBuildQuery)
	return err
}
