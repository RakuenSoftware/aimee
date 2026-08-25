package postgres

import (
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
	"github.com/JBailes/aimee/server-go/db3"
)

func appliedEvent(t *testing.T, principal uint32, applied db3.Applied) bus.Event {
	t.Helper()
	payload, err := db3.EncodeApplied(applied)
	if err != nil {
		t.Fatal(err)
	}
	return bus.Event{
		Frame:   bus.Frame{HdrFlags: bus.FNotification, EventKind: db3.EventApplied, PrincipalRef: principal},
		Payload: payload,
	}
}

func TestAnAcknowledgementFromTheProvisionedProviderSettlesTheLedger(t *testing.T) {
	// The only thing that deletes an outbox row. Without this path the ledger
	// grows without bound and replays every operation on each lease expiry.
	attachment := &VectorBus{provider: VectorProvider{Principal: 456, Instance: "pgroute"}}
	settled := make(chan db3.Applied, 4)
	attachment.ObserveApplied(func(principal uint32, applied db3.Applied) {
		if principal != 456 {
			t.Errorf("settled under principal %d", principal)
		}
		settled <- applied
	})

	attachment.Absorb(appliedEvent(t, 456, db3.Applied{
		OperationID: 9, Generation: 3, Watermark: 9, Result: db3.AppliedOK,
	}))
	select {
	case applied := <-settled:
		if applied.OperationID != 9 || applied.Generation != 3 {
			t.Fatalf("settled %+v", applied)
		}
	default:
		t.Fatal("the acknowledgement never reached the ledger")
	}
}

func TestAnAcknowledgementFromAnyoneElseIsIgnored(t *testing.T) {
	// Exactly one provider is granted. An Applied from another principal claims
	// an operation landed in a store this deployment does not read, and acting
	// on it deletes the row that was the only record it had not.
	attachment := &VectorBus{provider: VectorProvider{Principal: 456}}
	attachment.ObserveApplied(func(uint32, db3.Applied) {
		t.Fatal("an acknowledgement from an unprovisioned principal settled the ledger")
	})
	attachment.Absorb(appliedEvent(t, 457, db3.Applied{
		OperationID: 9, Generation: 3, Watermark: 9, Result: db3.AppliedOK,
	}))
}

func TestAMalformedAcknowledgementIsDroppedRatherThanSettled(t *testing.T) {
	// Dropping leaves the operation claimed until its lease expires and then
	// redelivered, which is the safe direction: Apply is idempotent by id.
	attachment := &VectorBus{provider: VectorProvider{Principal: 456}}
	attachment.ObserveApplied(func(uint32, db3.Applied) {
		t.Fatal("a malformed acknowledgement settled the ledger")
	})
	attachment.Absorb(bus.Event{
		Frame:   bus.Frame{HdrFlags: bus.FNotification, EventKind: db3.EventApplied, PrincipalRef: 456},
		Payload: []byte("not an applied"),
	})
}
