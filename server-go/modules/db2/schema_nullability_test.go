package db2

import (
	"context"
	"sort"
	"strings"
	"testing"
	"time"
)

// pgx refuses to scan a NULL into a plain string or int64, and the C layer this
// replaces never had to care: aimee_pg_column_text answers "" for a NULL, so
// every C row mapper absorbed them. A port that scans a nullable column
// directly fails on every row that has one -- which for decision_log's
// `outcome` was every row a write had just made, so the operation reported
// unacknowledged for a write that had landed.
//
// A fake never has this defect, and auditing by hand only settles the tables
// read on the day of the audit. This is the durable half: the nullable columns
// of every table the port reads, declared here and checked against the schema.
// A column that becomes nullable, or a table that joins the port's reach with
// one already nullable, fails here and names the scan to go and look at.
//
// A column listed here is not necessarily scanned -- several are columns the
// port selects around. What matters is that the set does not grow unnoticed.
var knownNullableColumns = map[string][]string{
	"artifacts":                   {"last_accessed_at", "last_decay_at", "reflected_at", "turn_id"},
	"audit_events":                {"after_snapshot", "before_snapshot"},
	"curator_invalidation_events": {},
	"decision_log":                {"outcome", "task_id"},
	"docs":                        {},
	"entity_edges":                {"object_kind", "relation_id", "subject_kind", "window_id"},
	"kb_documents":                {"kb_fts_tsv", "next_chunk_id", "page_end", "page_start", "prev_chunk_id"},
	"kb_enrollments":              {},
	"kb_file_index":               {"content"},
	"kb_ingest_queue":             {"completed_at", "error_message", "started_at"},
	"kb_runtime_state":            {},
	"memories": {
		"artifact_hash", "artifact_ref", "artifact_type", "effectiveness",
		"last_used_at", "memories_code_fts_text", "memories_fts_tsv",
		"memory_negation_fts_tsv", "source_session", "valid_from", "valid_until",
	},
	"memory_entities":      {},
	"memory_summaries":     {},
	"memory_temporal_refs": {},
	"memory_units":         {"memory_units_fts_tsv"},
	"projects":             {"kb_project"},
	"prospective_memories": {"prospective_memories_fts_tsv"},
}

func TestLiveNullableColumnsAreTheOnesThePortExpects(t *testing.T) {
	store, closeStore := liveStore(t)
	defer closeStore()

	tables := make([]string, 0, len(knownNullableColumns))
	for table := range knownNullableColumns {
		tables = append(tables, table)
	}
	sort.Strings(tables)

	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()
	rows, err := store.Query(ctx, `SELECT table_name, column_name
		 FROM information_schema.columns
		WHERE is_nullable = 'YES' AND table_name = ANY($1)
		ORDER BY table_name, column_name`, tables)
	if err != nil {
		t.Fatalf("read information_schema: %v", err)
	}
	defer rows.Close()

	found := map[string][]string{}
	for rows.Next() {
		var table, column string
		if err := rows.Scan(&table, &column); err != nil {
			t.Fatalf("scan: %v", err)
		}
		found[table] = append(found[table], column)
	}
	if rows.Err() != nil {
		t.Fatalf("read: %v", rows.Err())
	}

	for _, table := range tables {
		expected := append([]string(nil), knownNullableColumns[table]...)
		sort.Strings(expected)
		actual := found[table]
		sort.Strings(actual)
		if strings.Join(expected, ",") != strings.Join(actual, ",") {
			t.Errorf("%s nullable columns are [%s], this package expects [%s]; "+
				"any operation scanning one of the new ones needs a pointer target "+
				"and text()/number() -- see scan.go",
				table, strings.Join(actual, ","), strings.Join(expected, ","))
		}
	}
}
