package families

import (
	"context"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
	store "github.com/JBailes/aimee/server-go/modules/aimee"
	wire "github.com/JBailes/aimee/server-go/aimee"
)

type retiredMemoryDB struct{ t *testing.T }

func (d retiredMemoryDB) Exec(context.Context, string, ...any) (store.Tag, error) {
	d.t.Fatal("retired memory attempted SQL write")
	return nil, nil
}
func (d retiredMemoryDB) Query(context.Context, string, ...any) (store.Rows, error) {
	d.t.Fatal("retired memory attempted SQL read")
	return nil, nil
}
func (d retiredMemoryDB) QueryRow(context.Context, string, ...any) store.Row {
	d.t.Fatal("retired memory attempted SQL row read")
	return nil
}
func (d retiredMemoryDB) Begin(context.Context) (store.Tx, error) {
	d.t.Fatal("retired memory attempted transaction")
	return nil, nil
}

func TestRetiredPersonalMemoryWireCannotReadOrReviveLegacyRows(t *testing.T) {
	for _, tc := range []struct {
		name   string
		op     uint32
		fields []string
	}{
		{"identity", opUserMemoryListRecall, []string{"1", "32"}},
		{"preferences", opUserMemoryListRecall, []string{"2", "32"}},
		{"existence", opUserMemoryAny, nil},
		{"revive", opUserMemoryUpsert, []string{"fact", "L2", "identity:test", "synthetic text", "1", "test"}},
	} {
		t.Run(tc.name, func(t *testing.T) {
			frame, err := wire.EncodeFields(tc.op, tc.fields)
			if err != nil {
				t.Fatal(err)
			}
			reply, status := Conversation.Handler(retiredMemoryDB{t})(bus.ModuleInvocation{StageID: StageConversation}, frame)
			if status != bus.ModuleStatusOK {
				t.Fatalf("transport status %v", status)
			}
			result, fields, err := wire.DecodeFields(reply)
			if err != nil || result != store.StatusInvalid || len(fields) != 0 {
				t.Fatalf("retired result=%d fields=%d err=%v", result, len(fields), err)
			}
		})
	}
}
