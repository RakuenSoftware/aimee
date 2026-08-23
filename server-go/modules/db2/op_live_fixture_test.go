package db2

// The rows a generated probe's minimum-valid arguments point at.
//
// Every generated probe sends identifier one and the string "a", because those
// are the smallest values its envelope accepts. On an empty schema those name
// nothing, and the operations that refuse when their row is absent -- touch,
// reject, the directive updates, the delete-by-id pair -- would come back as
// broken statements rather than as correct refusals.
//
// So the fixture puts something under those identifiers. It is deliberately
// minimal: one row in each table an identifier-taking operation reads, and
// nothing else. Everything here rolls back with the probe.
//
// The unique index comes first and is not a fixture row at all: the schema does
// not declare it, the C builds it at module init, and the projection's edge
// upsert takes the slower path without it. Building it here is what lets the
// fast path be probed -- see EnsureSchemaIndexes, which is the same statement
// for a host to call.
var liveGeneratedFixture = []string{
	entityEdgeUniqueIndexBuildQuery,

	`INSERT INTO projects (id, name, root, scanned_at, lifecycle_state,
		current_generation)
	 VALUES (1, 'a', '/a', '2026-01-01 00:00:00', 'current', 1)
	 ON CONFLICT (id) DO NOTHING`,

	`INSERT INTO memories (id, tier, kind, key, content, confidence,
		merged_into, created_at, updated_at)
	 VALUES (1, 'L2', 'fact', 'a', 'a', 0.5, 0, '2026-01-01 00:00:00',
		'2026-01-01 00:00:00')
	 ON CONFLICT (id) DO NOTHING`,

	`INSERT INTO memory_links (id, source_id, target_id, relation)
	 VALUES (1, 1, 1, 'depends_on') ON CONFLICT (id) DO NOTHING`,

	`INSERT INTO artifacts (id, kind, state, payload)
	 VALUES ('a', 'probe', 'proposed', '{}'::jsonb) ON CONFLICT (id) DO NOTHING`,

	`INSERT INTO docs (id, content_hash, filename, scope, normalized_text)
	 VALUES (1, 'a', 'a', 'global', 'a') ON CONFLICT (id) DO NOTHING`,

	`INSERT INTO tasks (id, title, state, created_at, updated_at)
	 VALUES (1, 'a', 'todo', '2026-01-01 00:00:00', '2026-01-01 00:00:00')
	 ON CONFLICT (id) DO NOTHING`,

	`INSERT INTO anti_patterns (id, pattern, source_ref)
	 VALUES (1, 'a', 'a') ON CONFLICT (id) DO NOTHING`,

	`INSERT INTO epistemic_directives (id, question, cause, state)
	 VALUES (1, 'a', 'a', 'open') ON CONFLICT (id) DO NOTHING`,

	`INSERT INTO curiosity_items (id, gap_type, target_entity, state)
	 VALUES (1, 'missing_fact', 'a', 'open') ON CONFLICT (id) DO NOTHING`,

	`INSERT INTO kb_documents (id, project, file_path, file_hash, chunk_index,
		content, generation)
	 VALUES (1, 'a', 'a', 'a', 0, 'a', 1) ON CONFLICT (id) DO NOTHING`,

	`INSERT INTO memory_reembed_progress (id) VALUES (1)
	 ON CONFLICT (id) DO NOTHING`,
}
