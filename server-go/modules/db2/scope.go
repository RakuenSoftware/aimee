package db2

import "fmt"

// Scope is the visibility a request was made under.
//
// It arrives on the wire because it has to: the C reads it from a thread-local
// the caller set before the call, and a thread-local does not follow a call out
// of the process. An operation whose answer depends on the scope has to be told
// the scope.
type Scope struct {
	Active     bool
	IncludeAll bool
	Workspace  string
	Project    string
}

// DecodeScope unpacks the wire triple.
//
// scope_flags is a bitfield rather than two fields because that is what the
// contract carries: bit 0 active, bit 1 include-all.
func DecodeScope(flags uint32, workspace, project string) Scope {
	return Scope{
		Active:     flags&1 != 0,
		IncludeAll: flags&2 != 0,
		Workspace:  workspace,
		Project:    project,
	}
}

// rank is the visibility rank of one memory, kept equivalent to
// DB2_MEMORY_SCOPE_RANK_SQL in memory_scope_query.h.
//
// Local-first: the active project outranks the active workspace, which outranks
// what is shared or global. A memory carrying no scope tag at all ranks as
// shared rather than hidden, which is what keeps rows written before scoping
// existed readable. Anything else ranks zero.
//
// It does not consult include-all. That flag admits rows rather than ranking
// them, so it belongs to the filter and a memory's rank does not move with it.
//
// memoryID is SQL rather than a value: each caller splices in whichever column
// of its own statement identifies the memory.
func (s Scope) rank(memoryID string, active, workspace, project int) string {
	return fmt.Sprintf(`CASE WHEN $%d = 0 THEN 0`+
		` WHEN $%d <> '' AND EXISTS (SELECT 1 FROM memory_scopes asp`+
		` WHERE asp.memory_id = %s AND asp.scope_type = 'project'`+
		` AND asp.scope_value = $%d) THEN 3`+
		` WHEN $%d <> '' AND (EXISTS (SELECT 1 FROM memory_scopes asw`+
		` WHERE asw.memory_id = %s AND asw.scope_type = 'workspace'`+
		` AND asw.scope_value = $%d)`+
		` OR EXISTS (SELECT 1 FROM memory_workspaces aw`+
		` WHERE aw.memory_id = %s AND aw.workspace = $%d)) THEN 2`+
		` WHEN EXISTS (SELECT 1 FROM memory_scopes asg WHERE asg.memory_id = %s`+
		` AND asg.scope_type = 'global' AND asg.scope_value = '_global')`+
		` OR EXISTS (SELECT 1 FROM memory_scopes ass WHERE ass.memory_id = %s`+
		` AND ass.scope_type = 'workspace' AND ass.scope_value = '_shared')`+
		` OR (NOT EXISTS (SELECT 1 FROM memory_scopes asc0 WHERE asc0.memory_id = %s`+
		` AND asc0.scope_type IN ('global','workspace','project'))`+
		` AND NOT EXISTS (SELECT 1 FROM memory_workspaces aw0`+
		` WHERE aw0.memory_id = %s)) THEN 1 ELSE 0 END`,
		active,
		project, memoryID, project,
		workspace, memoryID, workspace, memoryID, workspace,
		memoryID, memoryID, memoryID, memoryID)
}

// filter builds the predicate a scoped read appends to its WHERE, together with
// the four values to bind, kept equivalent to DB2_MEMORY_SCOPE_FILTER_SQL.
//
// used is how many placeholders the caller's own statement has already spent;
// the four scope placeholders follow it. Passing the wrong number produces a
// statement that binds the caller's arguments to the scope, which is why every
// caller derives it from the same slice it passes to Query rather than from a
// literal.
//
// Read the predicate as three ways to be admitted: the scope is inactive, the
// caller asked for everything, or the memory ranks above zero here. The first
// is why an unscoped call must never reach a scoped statement -- inactive does
// not mean "no rows", it means "all rows".
func (s Scope) filter(memoryID string, used int) (string, []any) {
	active, includeAll := used+1, used+2
	workspace, project := used+3, used+4
	predicate := fmt.Sprintf(` AND ($%d = 0 OR $%d = 1 OR (%s) > 0)`,
		active, includeAll, s.rank(memoryID, active, workspace, project))
	return predicate, []any{
		boolToInt(s.Active), boolToInt(s.IncludeAll), s.Workspace, s.Project,
	}
}

// filterSharing is the predicate alone, for a statement that scopes two
// memories against the same scope.
//
// Both sides read the same four placeholders, so the values are bound once. The
// C gets this for free -- its four are reserved parameter numbers, reused by
// every filter in a statement -- and here it has to be said out loud, because
// calling filter twice would allocate a second set and bind the caller's own
// arguments to it.
func (s Scope) filterSharing(memoryID string, used int) string {
	predicate, _ := s.filter(memoryID, used)
	return predicate
}

// rankExpression is the same rank on its own, for the reads that order by it
// rather than only filtering on it.
func (s Scope) rankExpression(memoryID string, used int) string {
	return s.rank(memoryID, used+1, used+3, used+4)
}

func boolToInt(value bool) int {
	if value {
		return 1
	}
	return 0
}
