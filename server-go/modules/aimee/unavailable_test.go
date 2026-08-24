package aimee

import (
	"context"
	"encoding/binary"
	"errors"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
)

// An unreachable store and a failed request are different facts and must not
// arrive as the same answer.
//
// StatusFailed means "I understood the request and it did not work" -- a fact
// about the request, which a caller is right to record and move on from. An
// unreachable postgres module is a fact about the MOMENT: the identical request
// succeeds once the store is back, so a caller that writes it down as an
// outcome has persisted a wrong conclusion from a transient fault.
//
// It matters more here than anywhere else because every operation in the system
// arrives at this dispatch. During a store restart, collapsing the two tells
// every caller its own request was bad -- 463 operations all individually wrong
// at the same instant, which is the one explanation that cannot be true.

// probeFamily is one op whose body returns whatever the test needs, on a kind
// derived by the canonical rule so Handler's own checks are satisfied.
func probeFamily(body OpFunc) Family {
	return Family{
		Name:  "probe",
		Event: 4096 + PrincipalRef*256 + 1,
		Stage: 1,
		Ops:   map[uint32]Op{1: {Name: "probe_read", Args: 0, Cells: 0, Run: body}},
	}
}

// encodeRequest builds a db1-fields-v2 request with no fields.
func encodeRequest(op uint32) []byte {
	frame := make([]byte, 8)
	binary.LittleEndian.PutUint32(frame[0:4], op)
	binary.LittleEndian.PutUint32(frame[4:8], 0)
	return frame
}

func replyStatus(t *testing.T, reply []byte) uint32 {
	t.Helper()
	if len(reply) < 4 {
		t.Fatalf("reply is %d bytes, too short to carry a status", len(reply))
	}
	return binary.LittleEndian.Uint32(reply[0:4])
}

func TestAnUnreachableStoreIsATransportFailureNotARefusal(t *testing.T) {
	family := probeFamily(func(context.Context, Queryer, []string) (uint32, []string, error) {
		return 0, nil, ErrStoreUnavailable
	})
	handler := family.Handler(&fakeDB{})

	reply, status := handler(bus.ModuleInvocation{StageID: 1}, encodeRequest(1))

	if status != bus.ModuleStatusInternal {
		t.Errorf("module status = %v, want ModuleStatusInternal: an unreachable "+
			"store is not a request the module refused", status)
	}
	// And it must not ALSO carry a fields-v2 refusal, which a caller would read
	// as an answer about its own request.
	if len(reply) != 0 {
		t.Errorf("reply = % x, want empty: a transport failure carries no result", reply)
	}
}

// A wrapped ErrStoreUnavailable is still an unreachable store. The store client
// wraps it on some paths, and errors.Is is the only thing that keeps this from
// depending on which one the caller happened to take.
func TestAWrappedUnavailableIsStillTransport(t *testing.T) {
	family := probeFamily(func(context.Context, Queryer, []string) (uint32, []string, error) {
		return 0, nil, errors.Join(errors.New("loading sessions"), ErrStoreUnavailable)
	})
	_, status := family.Handler(&fakeDB{})(bus.ModuleInvocation{StageID: 1}, encodeRequest(1))
	if status != bus.ModuleStatusInternal {
		t.Errorf("wrapped unavailable = %v, want ModuleStatusInternal", status)
	}
}

// The converse, and the reason this is not simply "report every error as
// transport": an ordinary failure IS a fact about the request. Reporting it as
// the store being unreachable would have a caller retry something that fails
// identically every time.
func TestAnOrdinaryFailureIsStillARefusal(t *testing.T) {
	family := probeFamily(func(context.Context, Queryer, []string) (uint32, []string, error) {
		return 0, nil, errors.New("probe: the row was not acceptable")
	})

	reply, status := family.Handler(&fakeDB{})(bus.ModuleInvocation{StageID: 1}, encodeRequest(1))
	if status != bus.ModuleStatusOK {
		t.Fatalf("module status = %v, want OK: the module answered", status)
	}
	if got := replyStatus(t, reply); got != StatusFailed {
		t.Errorf("status = %d, want StatusFailed (%d)", got, StatusFailed)
	}
}
