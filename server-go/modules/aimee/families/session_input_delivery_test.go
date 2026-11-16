package families

import (
	"context"
	"errors"
	"fmt"
	wire "github.com/JBailes/aimee/server-go/aimee"
	store "github.com/JBailes/aimee/server-go/modules/aimee"
	"github.com/RakuenSoftware/ouroboros-ring/go/ring"
	"strings"
	"testing"
)

func TestOriginalInputDeliveryActualHostAuthorization(t *testing.T) {
	for id, count := range map[uint32]int{58: 6, 59: 4, 60: 5, 61: 6} {
		op := Sessions.Ops[id]
		if !op.HostOnly || !op.Tx || op.Timeout != store.CanonicalHistoryTimeout {
			t.Fatalf("operation%d lacks actual host/transaction budget", id)
		}
		args := make([]string, count)
		for i := range args {
			args[i] = "claimed-owner-receiver"
		}
		frame, _ := wire.EncodeFields(id, args)
		db := &sessDB{}
		_, status := Sessions.Handler(db)(ring.ModuleInvocation{StageID: StageSessions, PrincipalRef: 7}, frame)
		if status != ring.ModuleStatusInvalidRequest || len(db.executed) != 0 {
			t.Fatal("body receiver tag gave a module host authority")
		}
	}
}
func TestOriginalInputDeliveryMalformedIntentHasNoDatabaseAccess(t *testing.T) {
	for _, intent := range []string{`{}`, `{"schema_version":1,"receiver_tag":"x","intent_key":"` + strings.Repeat("a", 64) + `","opaque_intent":"x","authority":true}`, `{"schema_version":1,"receiver_tag":"x","intent_key":"bad","opaque_intent":"x"}`, `{"schema_version":1,"receiver_tag":"x","intent_key":"` + strings.Repeat("a", 64) + `","opaque_intent":""}`} {
		status, _, err := sessionInputDeliveryBegin(context.Background(), nil, []string{"s", "op", "turn", "actor", "{}", intent})
		if err != nil || status != store.StatusInvalid {
			t.Fatal(status, err)
		}
	}
}

func TestOriginalInputDeliveryConflictIdentity(t *testing.T) {
	exact := &store.StoreError{SQLState: "P0001", Message: "canonical_input_delivery_conflict", Op: "delete"}
	if !originalInputDeliveryConflict(exact) || !originalInputDeliveryConflict(fmt.Errorf("wrapped: %w", exact)) {
		t.Fatal("exact backend identity not classified")
	}
	var nilRefusal *store.StoreError
	for _, err := range []error{nil, nilRefusal, errors.New("canonical_input_delivery_conflict"), &store.StoreError{SQLState: "23503", Message: "canonical_input_delivery_conflict"}, &store.StoreError{SQLState: "P0001", Message: "prefix canonical_input_delivery_conflict"}, &store.StoreError{SQLState: "P0001", Message: "canonical_input_delivery_conflict suffix"}, &store.StoreError{SQLState: "P0001", Message: "other failure"}} {
		if originalInputDeliveryConflict(err) {
			t.Fatal("unrelated diagnostic became conflict")
		}
	}
}

func TestOriginalInputDeliveryMalformedAckHasNoDatabaseAccess(t *testing.T) {
	for _, result := range []string{`{"value":"\u0000"}`, `{"a":1,"a":2}`, `{} {}`, ""} {
		status, _, err := sessionInputDeliveryAck(context.Background(), nil, []string{"1", "receiver", "actor", strings.Repeat("a", 64), "delivered", result})
		if err != nil || status != store.StatusInvalid {
			t.Fatal("malformed documentary result reached database", status, err)
		}
	}
}
