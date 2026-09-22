package memory

// Versioned current-state KB eligibility, evaluated before lane limits. The
// storage transaction supplies one stable request clock through CURRENT_TIMESTAMP.
// Scope/RLS and evidence-specific admission remain additional mandatory gates.
const currentEligibilityPolicy = "current-validity-v6"

// KB timestamps historically mix UTC wall time and RFC3339 offsets. Normalize
// both at the adapter; invalid nonempty timestamps raise a query error rather
// than being compared as text or admitted as an open endpoint. This is the same
// normalization as the PostgreSQL owner's aimee_utc_text_timestamptz for literal
// dates, additionally rejecting relative/infinite inputs such as "now".
func memoryTimeSQL(column string) string {
	return `(CASE WHEN NULLIF(btrim(` + column + `),'') IS NULL THEN NULL
 WHEN btrim(` + column + `) !~ '^[0-9]{4}-[0-9]{2}-[0-9]{2}([T ][0-9]{2}:[0-9]{2}(:[0-9]{2}([.][0-9]+)?)?(Z|[+-][0-9]{2}(:?[0-9]{2})?)?)?$'
 THEN ('invalid memory timestamp: '||` + column + `)::timestamptz
 WHEN btrim(` + column + `) ~ '[T ][0-9]{2}:[0-9]{2}(:[0-9]{2})?([.][0-9]+)?(Z|[+-][0-9]{2}(:?[0-9]{2})?)$'
 THEN ` + column + `::timestamptz ELSE ` + column + `::timestamp AT TIME ZONE 'UTC' END)`
}

// Prefixes are fixed SQL aliases supplied by this package, never request text.
func memoryValiditySQL(prefix string) string {
	return memoryValidityAtSQL(prefix, "CURRENT_TIMESTAMP")
}
func currentMemorySQL(prefix string) string {
	return prefix + `lifecycle_state='active' AND ` + prefix + `activation_suppressed=0 AND ` + memoryValiditySQL(prefix)
}

// Legacy as_of reads inspect an explicitly identified version and label its
// valid-time applicability separately. Supersession/archive/retirement suppression
// prevents current-state re-entry, not authorized inspection of those old versions.
// Revocation, quarantine, rejection and deletion must never become inspectable
// simply because the caller supplied an as_of value. Unknown states fail closed.
func historicalMemoryInspectionSQL(prefix string) string {
	return `(` + prefix + `lifecycle_state IN ('superseded','archived','retired') OR (` +
		prefix + `lifecycle_state='active' AND ` + prefix + `activation_suppressed=0))`
}

// Clock expressions are fixed owner SQL or bound timestamp parameters, never
// caller-supplied SQL. Current and historical reads share interval semantics.
func memoryStartedAtSQL(column, clock string) string {
	value := memoryTimeSQL(column)
	return `(` + value + ` IS NULL OR ` + value + `<=` + clock + `)`
}
func memoryUnexpiredAtSQL(column, clock string) string {
	value := memoryTimeSQL(column)
	return `(` + value + ` IS NULL OR ` + clock + `<` + value + `)`
}
func memoryValidityAtSQL(prefix, clock string) string {
	return memoryStartedAtSQL(prefix+"valid_from", clock) + ` AND ` + memoryUnexpiredAtSQL(prefix+"valid_until", clock)
}

// Directives and reminders have an upper validity endpoint only. Matching,
// briefing and sweeps use the same half-open boundary as memory records.
func memoryUnexpiredSQL(prefix string) string {
	return memoryUnexpiredAtSQL(prefix+"valid_until", "CURRENT_TIMESTAMP")
}
