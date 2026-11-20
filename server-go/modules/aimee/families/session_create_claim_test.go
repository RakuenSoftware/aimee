package families

import (
	"context"
	"errors"
	"strings"
	"testing"

	store "github.com/JBailes/aimee/server-go/modules/aimee"
)

// A minimal in-memory server_sessions table: enough to observe whether a
// create inserted, claimed, or collided.
type claimRow struct{ clientType, principal string }

type claimStore struct{ rows map[string]claimRow }

var errDuplicateSession = errors.New("duplicate key value violates unique constraint")

func (s *claimStore) Exec(_ context.Context, sql string, a ...any) (store.Tag, error) {
	id := a[0].(string)
	switch {
	case strings.HasPrefix(strings.TrimSpace(sql), "UPDATE server_sessions SET principal"):
		row, ok := s.rows[id]
		if !ok || row.principal != "" || (row.clientType != "chat" && row.clientType != "webchat") {
			return store.RowsAffected(0), nil
		}
		row.principal = a[1].(string)
		s.rows[id] = row
		return store.RowsAffected(1), nil
	case strings.HasPrefix(strings.TrimSpace(sql), "INSERT INTO server_sessions"):
		if _, ok := s.rows[id]; ok {
			return nil, errDuplicateSession
		}
		s.rows[id] = claimRow{clientType: a[1].(string), principal: a[2].(string)}
		return store.RowsAffected(1), nil
	}
	return nil, errors.New("unexpected SQL: " + sql)
}

func (s *claimStore) Query(context.Context, string, ...any) (store.Rows, error) {
	return nil, errors.New("not used")
}
func (s *claimStore) QueryRow(context.Context, string, ...any) store.Row { return nil }

func TestServerSessionCreateClaimsOnlyOwnerlessLegacyBrowserRows(t *testing.T) {
	ctx := context.Background()
	s := &claimStore{rows: map[string]claimRow{
		"web-legacy":    {clientType: "chat", principal: ""},
		"web-owned":     {clientType: "webchat", principal: "webuser:admin"},
		"cli-ownerless": {clientType: "claude", principal: ""},
	}}

	// A pre-upgrade browser tab is adopted by the attaching webchat actor.
	if st, _, err := serverSessionCreate(ctx, s, []string{"web-legacy", "webchat", "webuser:admin"}); err != nil || st != store.StatusOK {
		t.Fatalf("legacy browser claim: status=%d err=%v", st, err)
	}
	if got := s.rows["web-legacy"].principal; got != "webuser:admin" {
		t.Fatalf("legacy browser row principal = %q, want webuser:admin", got)
	}

	// An owned row is never rewritten: the create still collides as before.
	if _, _, err := serverSessionCreate(ctx, s, []string{"web-owned", "webchat", "webuser:other"}); err == nil {
		t.Fatal("create over an owned browser row succeeded; it must still collide")
	}
	if got := s.rows["web-owned"].principal; got != "webuser:admin" {
		t.Fatalf("owned row principal changed to %q", got)
	}

	// Another client's ownerless row is not a browser tab and is not claimed.
	if _, _, err := serverSessionCreate(ctx, s, []string{"cli-ownerless", "webchat", "webuser:admin"}); err == nil {
		t.Fatal("webchat create claimed a non-browser ownerless row")
	}

	// Non-webchat creators keep plain insert-or-collide semantics.
	if _, _, err := serverSessionCreate(ctx, s, []string{"web-legacy", "mcp", "uid:0"}); err == nil {
		t.Fatal("non-webchat create over an existing row succeeded")
	}
	if st, _, err := serverSessionCreate(ctx, s, []string{"web-new", "webchat", "webuser:admin"}); err != nil || st != store.StatusOK {
		t.Fatalf("fresh webchat create: status=%d err=%v", st, err)
	}
	if got := s.rows["web-new"]; got.principal != "webuser:admin" || got.clientType != "webchat" {
		t.Fatalf("fresh row = %+v", got)
	}
}
