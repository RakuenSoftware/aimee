package memory

// Versioned current-state KB eligibility, evaluated before lane limits. The
// storage transaction supplies one stable request clock through CURRENT_TIMESTAMP.
// Scope/RLS and evidence-specific admission remain additional mandatory gates.
const currentEligibilityPolicy = "current-validity-v10"

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

// Active retained inputs may be indexed before their valid-time boundary.
// This never grants serving authority: recall still applies currentMemorySQL.
// Suppression and non-active lifecycle states prohibit this indexing route.
func indexableMemorySQL(prefix string) string {
	return baseIndexableMemorySQL(prefix) + ` AND ` + currentEpisodeCardInputsSQL(prefix, false)
}

func baseIndexableMemorySQL(prefix string) string {
	return prefix + `lifecycle_state='active' AND ` + prefix + `activation_suppressed=0`
}

func currentMemorySQL(prefix string) string {
	return baseCurrentMemorySQL(prefix) + ` AND ` + currentEpisodeCardInputsSQL(prefix, false)
}

func baseCurrentMemorySQL(prefix string) string {
	return prefix + `lifecycle_state='active' AND ` + prefix + `activation_suppressed=0 AND ` + memoryValiditySQL(prefix)
}

// Legacy as_of reads inspect an explicitly identified version and label its
// valid-time applicability separately. Supersession/archive/retirement suppression
// prevents current-state re-entry, not authorized inspection of those old versions.
// Revocation, quarantine, rejection and deletion must never become inspectable
// simply because the caller supplied an as_of value. Unknown states fail closed.
func historicalMemoryInspectionSQL(prefix string) string {
	return baseHistoricalMemoryInspectionSQL(prefix) + ` AND ` + currentEpisodeCardInputsSQL(prefix, true)
}

func baseHistoricalMemoryInspectionSQL(prefix string) string {
	return `(` + prefix + `lifecycle_state IN ('superseded','archived','retired') OR (` +
		prefix + `lifecycle_state='active' AND ` + prefix + `activation_suppressed=0))`
}

// Operator alerts inspect unswept commitments and retained history. Their
// purpose does not authorize erased/quarantined content, suppressed live rows,
// unknown states or stale generated-card dependencies. Expiry remains visible
// here because alerting on overdue commitments is part of this read contract.
func alertMemoryInspectionSQL(prefix string) string {
	return `(` + baseHistoricalMemoryInspectionSQL(prefix) + ` OR (` + prefix +
		`lifecycle_state IN ('pending','fulfilled') AND ` + prefix + `activation_suppressed=0)) AND ` +
		currentEpisodeCardInputsSQL(prefix, true)
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

// Authored/derived relations retain their own interval in addition to current
// parent and producer-input eligibility. Legacy endpoint names differ from memories.
func relationValidityAtSQL(prefix, clock string) string {
	return memoryStartedAtSQL(prefix+"valid_at", clock) + ` AND ` + memoryUnexpiredAtSQL(prefix+"invalid_at", clock)
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

// Generated relation text may copy multiple memories. Observe and check every
// input, including exact revisions, before limits or profile aggregation. An old
// generator-owned row with no input observations waits for canonical reindexing.
// Authored relations retain their existing parent policy.
func currentRelationInputsSQL(alias string) string {
	dependency := `(CASE WHEN dep.source_kind='memory-relation-input-v2' THEN dep.source_ref::jsonb END)`
	return `(NOT EXISTS(SELECT 1 FROM memory_lineage own WHERE own.object_type='relation'
 AND own.object_id=` + alias + `.id AND own.source_kind='memory-index-v1') OR EXISTS(
 SELECT 1 FROM memory_lineage dep WHERE dep.object_type='relation' AND dep.object_id=` + alias + `.id
 AND dep.source_kind='memory-relation-input-v2' AND ` + dependency + `->>'record_id'=` + alias + `.memory_id::text))
 AND NOT EXISTS(SELECT 1 FROM memory_lineage dep LEFT JOIN LATERAL (
 SELECT m.id FROM memories m WHERE m.id=(` + dependency + `->>'record_id')::bigint
 AND m.record_revision::text=` + dependency + `->>'record_revision'
 AND ` + currentMemorySQL("m.") + `
 AND (NOT (` + dependency + ` ? 'link_id') OR EXISTS(SELECT 1 FROM memory_links input_link
 WHERE input_link.id=(` + dependency + `->>'link_id')::bigint
 AND input_link.source_id=` + alias + `.memory_id AND input_link.target_id=m.id
 AND COALESCE(NULLIF(input_link.relation,''),'related_to')=` + alias + `.relation))
 LIMIT 1) input ON TRUE
 WHERE dep.object_type='relation' AND dep.object_id=` + alias + `.id
 AND dep.source_kind='memory-relation-input-v2' AND input.id IS NULL)`
}

// Generator-owned episodes carry producer observations, not merely versions
// read alongside their old payload. Authored episodes retain the parent policy.
func currentEpisodeInputsSQL(alias string) string {
	input := `(CASE WHEN episode_input.source_kind='memory-episode-input-v1' THEN episode_input.source_ref::jsonb END)`
	return `(NOT EXISTS(SELECT 1 FROM memory_lineage episode_owner WHERE episode_owner.object_type='episode'
 AND episode_owner.object_id=` + alias + `.id AND episode_owner.source_kind='memory-index-v1') OR EXISTS(
 SELECT 1 FROM memory_lineage episode_input JOIN memories episode_parent
 ON episode_parent.id=` + alias + `.memory_id
 WHERE episode_input.object_type='episode' AND episode_input.object_id=` + alias + `.id
 AND episode_input.source_kind='memory-episode-input-v1'
 AND ` + input + `->>'record_id'=episode_parent.id::text
 AND ` + input + `->>'record_revision'=episode_parent.record_revision::text
 AND ` + input + `->>'episode_revision'=` + alias + `.record_revision::text
 AND ` + currentMemorySQL("episode_parent.") + `
 AND ((` + input + `->>'summary_id'='0' AND ` + input + `->>'summary_revision'='0') OR EXISTS(
 SELECT 1 FROM memory_summaries episode_summary WHERE episode_summary.id=(` + input + `->>'summary_id')::bigint
 AND episode_summary.memory_id=episode_parent.id
 AND episode_summary.record_revision::text=` + input + `->>'summary_revision'
 AND ` + summaryCurrentInputsSQL("episode_summary", "episode_parent") + `))))`
}

// Stable input identity includes every unit field used by embedding text or its
// retrieval payload. The digest avoids retaining another copy of derived text.
func unitInputDigestSQL(alias string) string {
	return `encode(sha256(convert_to(jsonb_build_array(` + alias + `.unit_type,` + alias + `.unit_key,` + alias + `.unit_text,` + alias + `.memory_kind,` + alias + `.weight)::text,'UTF8')),'hex')`
}

// This binds deterministic units to their observed parent and optional summary.
// Independent event/entity/temporal source revisions remain a separate contract.
// Temporal serving eligibility belongs to the caller: future inputs may be indexed.
func currentUnitInputsSQL(alias string) string {
	input := `(CASE WHEN unit_input.source_kind='memory-unit-input-v1' THEN unit_input.source_ref::jsonb END)`
	return `(NOT EXISTS(SELECT 1 FROM memory_lineage unit_owner WHERE unit_owner.object_type='unit'
 AND unit_owner.object_id=` + alias + `.id AND unit_owner.source_kind='memory-index-v1') OR EXISTS(
 SELECT 1 FROM memory_lineage unit_input JOIN memories unit_parent ON unit_parent.id=` + alias + `.memory_id
 WHERE unit_input.object_type='unit' AND unit_input.object_id=` + alias + `.id
 AND unit_input.source_kind='memory-unit-input-v1'
 AND ` + input + `->>'record_id'=unit_parent.id::text
 AND ` + input + `->>'record_revision'=unit_parent.record_revision::text
 AND ` + input + `->>'unit_digest'=` + unitInputDigestSQL(alias) + `
 AND ((` + input + `->>'summary_id'='0' AND ` + input + `->>'summary_revision'='0') OR EXISTS(
 SELECT 1 FROM memory_summaries unit_summary WHERE unit_summary.id=(` + input + `->>'summary_id')::bigint
 AND unit_summary.memory_id=unit_parent.id AND unit_summary.record_revision::text=` + input + `->>'summary_revision'
 AND ` + summaryCurrentInputsSQL("unit_summary", "unit_parent") + `))))`
}

// Episode cards are generated only from non-card canonical inputs. Keep that
// bounded producer contract at every serving gate: a missing observation cannot
// turn an old card into an independently authored fact. Historical inspection
// permits retained inputs but still excludes erased/revoked/hidden sources.
func currentEpisodeCardInputsSQL(prefix string, historical bool) string {
	if prefix == "" {
		prefix = "memories."
	}
	parentPolicy := baseCurrentMemorySQL("card_parent.")
	if historical {
		parentPolicy = baseHistoricalMemoryInspectionSQL("card_parent.")
	}
	observation := `(CASE WHEN card_observation.source_kind='episode-card-input-v1' THEN card_observation.source_ref::jsonb END)`
	return `NOT EXISTS(SELECT 1 FROM memory_units card_unit
 WHERE card_unit.memory_id=` + prefix + `id AND card_unit.is_episode_card=1 AND card_unit.unit_type='episode_card'
 AND NOT EXISTS(SELECT 1 FROM memory_lineage card_observation
 WHERE card_observation.object_type='memory_unit' AND card_observation.object_id=card_unit.id
 AND card_observation.source_kind='episode-card-input-v1'
 AND ` + observation + `->>'schema_version'='1'
 AND ` + observation + `->>'owner_id'=(SELECT owner_id::text FROM memory_collection_owner WHERE id=1)
 AND ` + observation + `->>'parent_revision'=` + prefix + `record_revision::text
 AND ` + observation + `->>'unit_digest'=` + unitInputDigestSQL("card_unit") + `
 AND jsonb_typeof(` + observation + `->'inputs')='array'
 AND jsonb_array_length(` + observation + `->'inputs') BETWEEN 1 AND 200
 AND NOT EXISTS(SELECT 1 FROM jsonb_array_elements(CASE WHEN jsonb_typeof(` + observation + `->'inputs')='array'
 THEN ` + observation + `->'inputs' ELSE '[]'::jsonb END) card_input
 LEFT JOIN LATERAL (SELECT card_parent.id FROM memories card_parent
 WHERE card_parent.id=(card_input->>'record_id')::bigint
 AND card_parent.record_revision::text=card_input->>'record_revision'
 AND ` + parentPolicy + `
 AND NOT EXISTS(SELECT 1 FROM memory_units ancestor_card WHERE ancestor_card.memory_id=card_parent.id AND ancestor_card.is_episode_card=1)
 LIMIT 1) required_input ON TRUE WHERE required_input.id IS NULL)))`
}
