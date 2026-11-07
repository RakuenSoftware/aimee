package postgres

import (
	"bytes"
	"context"
	"encoding/json"
	"fmt"
	"reflect"
	"strings"
	"testing"
	"time"

	"github.com/JBailes/aimee/server-go/db"
	domain "github.com/JBailes/aimee/server-go/modules/aimee"
	"github.com/JBailes/aimee/server-go/modules/aimee/families"
	"github.com/RakuenSoftware/ouroboros-ring/go/ring"
	"github.com/jackc/pgx/v5/pgxpool"
)

// Real PostgreSQL -> actual SQL handler -> actual wire encoder/decoder ->
// family reducer. Only the explicit negative/reference controls alter markers.
type completionWireCaller struct {
	sharedStoreCaller
	mode     string
	pages    int
	maxReply int
}

func (c *completionWireCaller) Call(ctx context.Context, kind, stage uint32, trace uint64, timeout time.Duration, body []byte) ([]byte, error) {
	reply, err := c.sharedStoreCaller.Call(ctx, kind, stage, trace, timeout, body)
	if err != nil {
		return nil, err
	}
	if len(reply) > c.maxReply {
		c.maxReply = len(reply)
	}
	if len(reply) > int(ring.ModuleMessageMaxBody) {
		return nil, fmt.Errorf("actual SQL frame exceeds transport")
	}
	if !bytes.Contains(body, []byte("AS terminal_page")) {
		return reply, nil
	}
	c.pages++
	if c.mode == "" {
		return reply, nil
	}
	r := reader{buf: reply}
	status, e := r.u32()
	if e != nil {
		return nil, e
	}
	state, e := r.str()
	if e != nil {
		return nil, e
	}
	message, e := r.str()
	if e != nil {
		return nil, e
	}
	if status != statusOK {
		return nil, fmt.Errorf("unexpected controlled SQL refusal")
	}
	width, e := r.u32()
	if e != nil {
		return nil, e
	}
	count, e := r.u32()
	if e != nil {
		return nil, e
	}
	if width != 6 {
		return nil, fmt.Errorf("history marker SQL width")
	}
	w := &writer{}
	w.header(status, state, message)
	w.u32(width)
	w.u32(count)
	for row := uint32(0); row < count; row++ {
		for column := uint32(0); column < width; column++ {
			v, e := r.value()
			if e != nil {
				return nil, e
			}
			if column == 5 {
				if _, ok := v.(bool); !ok {
					return nil, fmt.Errorf("actual PG marker is not bool")
				}
				switch c.mode {
				case "conservative-reference":
					v = false
				case "NULL":
					v = nil
				case "text":
					v = "true"
				case "integer":
					v = int64(1)
				default:
					return nil, fmt.Errorf("unknown controlled marker mode")
				}
			}
			if e = w.value(v); e != nil {
				return nil, e
			}
		}
	}
	if r.at != len(reply) {
		return nil, fmt.Errorf("controlled decoder trailing SQL data")
	}
	return w.buf, nil
}
func completionSQLStore(t *testing.T, pool *pgxpool.Pool, mode string) (db.Store, *completionWireCaller) {
	t.Helper()
	h := &sqlHandler{txs: newTxRegistry(), poolFn: func(context.Context) (*pgxpool.Pool, error) { return pool, nil }}
	caller := &completionWireCaller{sharedStoreCaller: sharedStoreCaller{h.handle}, mode: mode}
	s, err := db.NewStore(caller)
	if err != nil {
		t.Fatal(err)
	}
	return s, caller
}
func completionSQLHistory(t *testing.T, s db.Store, session, actor string) (uint32, []string, error) {
	t.Helper()
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()
	tx, err := s.Begin(ctx)
	if err != nil {
		return 0, nil, err
	}
	defer tx.Rollback(context.Background())
	status, cells, err := families.Sessions.Ops[33].Run(ctx, tx, []string{session, "primary", actor, ""})
	if err == nil && status == domain.StatusOK {
		err = tx.Commit(ctx)
	}
	return status, cells, err
}
func TestCanonicalHistoryCompletionActualSQLWire(t *testing.T) {
	pool := sharedTestDatabase(t)
	sourcePageSchema(t, pool)
	ctx, cancel := context.WithTimeout(context.Background(), 90*time.Second)
	defer cancel()
	for _, tc := range []struct {
		name               string
		rows               int
		escaped            bool
		queries, reference int
	}{
		{"empty", 0, false, 1, 1}, {"partial-terminal", 3, false, 1, 2}, {"exact256", 256, false, 2, 2}, {"over256", 257, false, 2, 3}, {"8MiB-cut-before-last-candidate", 26, true, 2, 3},
	} {
		t.Run(tc.name, func(t *testing.T) {
			session := "completion-" + tc.name
			if _, err := pool.Exec(ctx, `INSERT INTO server_sessions(id,principal) VALUES($1,'actor')`, session); err != nil {
				t.Fatal(err)
			}
			for i := 0; i < tc.rows; i++ {
				var value any
				if i < tc.rows-2 {
					text := "archive"
					if tc.escaped {
						text = strings.Repeat("\x01", 65536)
					}
					value = map[string]any{"kind": "history", "payload": map[string]any{"scope": "legacy-archive", "phase": "archive", "index": i, "archive_text": text}}
				} else if i == tc.rows-2 {
					value = map[string]any{"kind": "history", "payload": map[string]any{"scope": "primary", "phase": "message", "index": 0, "message": map[string]any{"role": "user", "blocks": []any{map[string]any{"type": "text", "text": "exact unchanged current checkpoint"}}}}}
				} else {
					value = map[string]any{"kind": "history", "payload": map[string]any{"scope": "primary", "phase": "commit", "count": 1}}
				}
				raw, err := json.Marshal(value)
				if err != nil {
					t.Fatal(err)
				}
				if _, err = pool.Exec(ctx, `INSERT INTO session_ir_events(session_id,sequence,event_id,turn_id,actor,payload) VALUES($1,$2,$3,'turn','actor',$4)`, session, i+1, fmt.Sprint("event-", i), string(raw)); err != nil {
					t.Fatal(err)
				}
			}
			selected, wire := completionSQLStore(t, pool, "")
			status, cells, err := completionSQLHistory(t, selected, session, "actor")
			if err != nil {
				t.Fatal(err)
			}
			want := uint32(domain.StatusOK)
			if tc.rows == 0 {
				want = domain.StatusMissing
			}
			if status != want || wire.pages != tc.queries {
				t.Fatal("actual SQL page outcome", status, want, wire.pages, tc.queries)
			}
			if wire.maxReply > 8388608 {
				t.Fatal("new marker exceeded conservative SQL reply byte budget", wire.maxReply)
			}
			reference, oldWire := completionSQLStore(t, pool, "conservative-reference")
			oldStatus, oldCells, err := completionSQLHistory(t, reference, session, "actor")
			if err != nil || oldStatus != status || !reflect.DeepEqual(oldCells, cells) || oldWire.pages != tc.reference {
				t.Fatal("history bytes/reference EOF strategy differ", err, oldStatus, status, oldWire.pages, tc.reference)
			}
			denied, denyWire := completionSQLStore(t, pool, "")
			deniedStatus, deniedCells, err := completionSQLHistory(t, denied, session, "foreign-actor")
			if err != nil || deniedStatus != domain.StatusMissing || len(deniedCells) != 0 || denyWire.pages != 1 {
				t.Fatal("foreign owner disclosed current history", err, deniedStatus)
			}
		})
	}
	// The actual nullable wire destination accepts bool/NULL without coercion;
	// the history reducer, unlike the generic codec, MUST reject NULL markers.
	for _, mode := range []string{"NULL", "text", "integer"} {
		t.Run("malformed-marker-"+mode, func(t *testing.T) {
			s, _ := completionSQLStore(t, pool, mode)
			_, cells, err := completionSQLHistory(t, s, "completion-partial-terminal", "actor")
			if err == nil || len(cells) != 0 {
				t.Fatal("malformed SQL marker became partial history")
			}
		})
	}
}
