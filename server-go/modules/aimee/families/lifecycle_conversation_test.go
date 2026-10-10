package families

import (
	"context"
	store "github.com/JBailes/aimee/server-go/modules/aimee"
	"testing"
)

type conversationResumeRow struct{ reason string }

func (r conversationResumeRow) Scan(dest ...any) error {
	*dest[0].(*string) = "discuss"
	*dest[1].(*string) = r.reason
	return nil
}

type conversationResumeTx struct {
	*scriptedTx
	reason string
}

func (t *conversationResumeTx) QueryRow(context.Context, string, ...any) store.Row {
	return conversationResumeRow{t.reason}
}

type conversationResumeDB struct {
	scriptedDB
	reason string
}

func (d *conversationResumeDB) Begin(context.Context) (store.Tx, error) {
	return &conversationResumeTx{&scriptedTx{&d.scriptedDB}, d.reason}, nil
}

func TestConversationRunnerWaitsCanRetryWithoutReleasingHumanGates(t *testing.T) {
	for _, reason := range []string{"conversation_input", "binding_pending", "human_gate", "children_pending", ""} {
		t.Run(reason, func(t *testing.T) {
			db := &conversationResumeDB{reason: reason}
			status, _, err := wfeResume(context.Background(), db, []string{"wi_conversation"})
			allowed := reason == "conversation_input" || reason == "binding_pending"
			if err != nil {
				t.Fatal(err)
			}
			if allowed {
				if status != store.StatusOK || !db.committed {
					t.Fatalf("retry refused: %d, committed=%v", status, db.committed)
				}
			} else if status != store.StatusFailed || db.committed {
				t.Fatalf("protected wait released: %d", status)
			}
		})
	}
}
