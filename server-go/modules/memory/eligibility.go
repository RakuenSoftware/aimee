package memory

// Versioned current-state KB eligibility, evaluated before lane limits. The
// storage transaction supplies one stable request clock through CURRENT_TIMESTAMP.
// Scope/RLS and evidence-specific admission remain additional mandatory gates.
const currentEligibilityPolicy = "current-validity-v8"

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

// A visible source cannot authorize content derived from another hidden or
// ineligible source. Existing assertion surfaces may select only live evidence;
// graph surfaces retain their stricter all-evidence boundary. Aliases and scope
// predicates are fixed owner SQL, never request text. This is serving policy,
// not review admission.
func currentMemoryEvidenceSQL(edgeAlias, scopePredicate string, liveOnly bool) string {
	if scopePredicate == "" {
		scopePredicate = "TRUE"
	}
	evidence := ""
	if liveOnly {
		evidence = ` AND f.invalidated_at=''`
	}
	// The primary key permits at most one parent. A bounded lateral lookup keeps
	// PostgreSQL from replacing these few probes with a scan of all visible
	// memories under RLS. Eligibility is evaluated inside that identity lookup.
	return `NOT EXISTS(SELECT 1 FROM fact_evidence f LEFT JOIN LATERAL (
 SELECT m.id FROM memories m WHERE m.id=` + memoryLocatorIDSQL("f.source_id") + ` AND ` + currentMemorySQL("m.") + ` AND (` + scopePredicate + `) LIMIT 1
 ) m ON TRUE
 WHERE f.assertion_id=` + edgeAlias + `.id AND f.source_kind='memory'` + evidence + ` AND m.id IS NULL)`
}

// Parse the canonical locator once on the evidence side so the parent primary
// key remains indexable. Malformed, noncanonical and overflowing locators resolve
// to NULL, exactly as the previous equality with 'memory:'||m.id::text did.
// The inner CASE guards bigint conversion; PostgreSQL may reorder AND terms.
func memoryLocatorIDSQL(column string) string {
	value := `substring(` + column + ` FROM 8)`
	return `(CASE WHEN ` + column + ` ~ '^memory:(0|-?[1-9][0-9]{0,18})$'
 THEN CASE WHEN ` + value + `::numeric BETWEEN -9223372036854775808 AND 9223372036854775807
 THEN ` + value + `::bigint END END)`
}
