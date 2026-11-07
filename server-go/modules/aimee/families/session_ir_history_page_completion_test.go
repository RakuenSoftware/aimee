package families

import (
	"context"
	"errors"
	"fmt"
	"reflect"
	"strconv"
	"strings"
	"testing"

	store "github.com/JBailes/aimee/server-go/modules/aimee"
)

// Model the existing bounded SQL source, including the byte cut BEFORE the
// terminal bit. Existing fakes use this only for the history-page statement.
func historyCompletionFixturePage(rows [][]any, after uint64) [][]any {
	candidates := make([][]any, 0, sessionIRHistoryPageSize)
	for _, row := range rows {
		n, _ := strconv.ParseUint(row[0].(string), 10, 64)
		if n > after {
			candidates = append(candidates, row)
			if len(candidates) == sessionIRHistoryPageSize {
				break
			}
		}
	}
	page := make([][]any, 0, len(candidates))
	bytes := 0
	for index, row := range candidates {
		for _, value := range row[1:5] {
			bytes += len(value.(string))
		}
		bytes += 64
		if bytes > 8388608 {
			break
		}
		copy := append([]any(nil), row...)
		copy = append(copy, len(candidates) < sessionIRHistoryPageSize && index == len(candidates)-1)
		page = append(page, copy)
	}
	return page
}

type completionRows struct {
	rows   [][]any
	at     int
	err    error
	closed bool
}

func (r *completionRows) Next() bool {
	if r.at >= len(r.rows) {
		return false
	}
	r.at++
	return true
}
func (r *completionRows) Scan(dest ...any) error {
	row := r.rows[r.at-1]
	if len(dest) != len(row) {
		return fmt.Errorf("controlled SQL column mismatch")
	}
	for i, target := range dest {
		switch value := target.(type) {
		case *string:
			x, ok := row[i].(string)
			if !ok {
				return fmt.Errorf("controlled non-text data")
			}
			*value = x
		case **bool:
			if row[i] == nil {
				*value = nil
				continue
			}
			x, ok := row[i].(bool)
			if !ok {
				return fmt.Errorf("controlled non-boolean marker")
			}
			*value = &x
		default:
			return fmt.Errorf("controlled unsupported destination")
		}
	}
	return nil
}
func (r *completionRows) Err() error { return r.err }
func (r *completionRows) Close()     { r.closed = true }

type completionQuery struct {
	store.Queryer
	source     [][]any
	queries    int
	cursors    []string
	queryError error
}

func (q *completionQuery) Query(ctx context.Context, sql string, args ...any) (store.Rows, error) {
	q.queries++
	if sql != sessionIRHistorySQL || len(args) != 3 || args[0] != "owned-session" || args[1] != "owner" {
		return nil, fmt.Errorf("source query or owner args changed")
	}
	if err := ctx.Err(); err != nil {
		return nil, err
	}
	if q.queryError != nil {
		return nil, q.queryError
	}
	after := args[2].(string)
	q.cursors = append(q.cursors, after)
	n, err := strconv.ParseUint(after, 10, 64)
	if err != nil {
		return nil, err
	}
	return &completionRows{rows: historyCompletionFixturePage(q.source, n)}, nil
}
func completionSource(after uint64, count, payloadBytes int) [][]any {
	rows := make([][]any, count)
	for i := range rows {
		rows[i] = []any{strconv.FormatUint(after+uint64(i)+1, 10), "turn", "owner", fmt.Sprint("event-", i), strings.Repeat("x", payloadBytes)}
	}
	return rows
}
func completionCollect(rows *sessionIRHistoryRows) ([][]string, error) {
	defer rows.Close()
	var values [][]string
	for rows.Next() {
		var a, b, c, d, e string
		if err := rows.Scan(&a, &b, &c, &d, &e); err != nil {
			return nil, err
		}
		values = append(values, []string{a, b, c, d, e})
	}
	if err := rows.Err(); err != nil {
		return nil, err
	}
	return values, nil
}
func TestHistoryCompletionBoundedPageContinuationAndBytes(t *testing.T) {
	for _, tc := range []struct {
		name                    string
		after                   uint64
		count, payload, queries int
	}{
		{"empty", 0, 0, 1, 1}, {"short-terminal", 0, 3, 1, 1},
		{"exact256-conservative-tail", 0, 256, 1, 2}, {"over256", 0, 257, 1, 2},
		{"byte-cut-short-is-not-EOF", 0, 12, (1 << 20) - 128, 2},
		{"wide-cursor", 9007199254740993, 2, 1, 1},
	} {
		t.Run(tc.name, func(t *testing.T) {
			source := completionSource(tc.after, tc.count, tc.payload)
			q := &completionQuery{source: source}
			after := strconv.FormatUint(tc.after, 10)
			first, err := q.Query(t.Context(), sessionIRHistorySQL, "owned-session", "owner", after)
			if err != nil {
				t.Fatal(err)
			}
			rows := &sessionIRHistoryRows{ctx: t.Context(), q: q, session: "owned-session", actor: "owner", after: after, rows: first}
			actual, err := completionCollect(rows)
			if err != nil {
				t.Fatal(err)
			}
			var expected [][]string
			for _, r := range source {
				expected = append(expected, []string{r[0].(string), r[1].(string), r[2].(string), r[3].(string), r[4].(string)})
			}
			if !reflect.DeepEqual(actual, expected) || q.queries != tc.queries {
				t.Fatalf("row bytes/count or queries changed: rows%d expected%d queries%d want%d", len(actual), len(expected), q.queries, tc.queries)
			}
			if tc.name == "byte-cut-short-is-not-EOF" {
				page := historyCompletionFixturePage(source, 0)
				if len(page) >= 256 || page[len(page)-1][5] != false {
					t.Fatal("byte cut claimed terminal")
				}
			}
		})
	}
}
func TestHistoryCompletionMalformedMarkerAndFailuresAreWholeErrors(t *testing.T) {
	good := []any{"1", "turn", "owner", "event", "payload", true}
	for _, tc := range []struct {
		name string
		rows [][]any
		err  error
	}{
		{"null", [][]any{{"1", "turn", "owner", "event", "payload", nil}}, nil},
		{"text", [][]any{{"1", "turn", "owner", "event", "payload", "true"}}, nil},
		{"integer", [][]any{{"1", "turn", "owner", "event", "payload", 1}}, nil},
		{"missing-column", [][]any{{"1", "turn", "owner", "event", "payload"}}, nil},
		{"terminal-before-next-row", [][]any{good, {"2", "turn", "owner", "next", "payload", true}}, nil},
		{"backend-EOF-error", [][]any{good}, errors.New("controlled rows error")},
		{"stalled-cursor", [][]any{{"0", "turn", "owner", "event", "payload", true}}, nil},
	} {
		t.Run(tc.name, func(t *testing.T) {
			r := &completionRows{rows: tc.rows, err: tc.err}
			rows := &sessionIRHistoryRows{ctx: t.Context(), q: &completionQuery{}, session: "owned-session", actor: "owner", after: "0", rows: r}
			values, err := completionCollect(rows)
			if err == nil || values != nil || !r.closed {
				t.Fatal("malformed/error page became partial output", err)
			}
		})
	}
	for _, terminal := range []bool{false, true} {
		rows := historyCompletionFixturePage(completionSource(0, 256, 1), 0)
		if terminal {
			rows[255][5] = true
		} else {
			rows = append(rows, []any{"257", "turn", "owner", "extra", "payload", false})
		}
		values, err := completionCollect(&sessionIRHistoryRows{ctx: t.Context(), q: &completionQuery{}, after: "0", rows: &completionRows{rows: rows}})
		if err == nil || values != nil {
			t.Fatal("invalid terminal/page ceiling accepted")
		}
	}
	first := &completionRows{rows: [][]any{{"1", "turn", "owner", "event", "payload", false}}}
	q := &completionQuery{queryError: errors.New("controlled next-page error")}
	if values, err := completionCollect(&sessionIRHistoryRows{ctx: t.Context(), q: q, session: "owned-session", actor: "owner", after: "0", rows: first}); err == nil || values != nil {
		t.Fatal("continuation error became partial output")
	}
}
