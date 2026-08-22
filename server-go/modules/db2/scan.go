package db2

// Reading a nullable column.
//
// pgx refuses to scan a NULL into a plain string or int64; it needs a pointer,
// and hands back nil. The C layer never had to think about this because
// aimee_pg_column_text answers "" for a NULL and aimee_pg_column_int64 answers
// zero, so every C row mapper quietly absorbed them.
//
// That difference is invisible until a real database produces a NULL, which no
// fake will: it is exactly what the live probe exists to catch, and it is how
// decision_log's `outcome` and `task_id` were found -- both nullable, both
// scanned into plain types, both failing the whole read-back so the operation
// answered unacknowledged for a write that had landed.
//
// A column declared NOT NULL is scanned directly. Reaching for one of these on
// a column that cannot be null would say something untrue about the schema.

// text is a nullable text column, with NULL read as the empty string -- what
// the C mapper produced.
func text(value *string) string {
	if value == nil {
		return ""
	}
	return *value
}

// number is a nullable integer column, with NULL read as zero.
func number(value *int64) int64 {
	if value == nil {
		return 0
	}
	return *value
}
