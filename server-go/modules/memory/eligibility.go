package memory

// Versioned current-state KB eligibility, evaluated before lane limits. The
// storage transaction supplies one stable request clock through CURRENT_TIMESTAMP.
// Scope/RLS and evidence-specific admission remain additional mandatory gates.
const currentEligibilityPolicy = "current-validity-v3"

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
	from := memoryTimeSQL(prefix + "valid_from")
	return `(` + from + ` IS NULL OR ` + from + `<=CURRENT_TIMESTAMP) AND ` + memoryUnexpiredSQL(prefix)
}
func currentMemorySQL(prefix string) string {
	return prefix + `lifecycle_state='active' AND ` + prefix + `activation_suppressed=0 AND ` + memoryValiditySQL(prefix)
}

// Directives and reminders have an upper validity endpoint only. Matching,
// briefing and sweeps use the same half-open boundary as memory records.
func memoryUnexpiredSQL(prefix string) string {
	until := memoryTimeSQL(prefix + "valid_until")
	return `(` + until + ` IS NULL OR CURRENT_TIMESTAMP<` + until + `)`
}
