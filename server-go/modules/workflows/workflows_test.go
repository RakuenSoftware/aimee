package workflows

import (
	"encoding/binary"
	"strings"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
)

func advanceWire(bound, workItem, observed, actualStage, actualState, nonce, lastNonce string) []byte {
	request := make([]byte, requestLen)
	binary.LittleEndian.PutUint32(request[0:4], requestMagic)
	binary.LittleEndian.PutUint32(request[4:8], wireVersion)
	if nonce != "" {
		binary.LittleEndian.PutUint32(request[8:12], 1)
	}
	values := [...]string{bound, workItem, observed, actualStage, actualState, nonce, lastNonce}
	offsets := [...]int{requestBoundOff, requestWorkItemOff, requestObservedOff,
		requestActualStageOff, requestActualStateOff, requestNonceOff, requestLastNonceOff}
	for index, value := range values {
		binary.LittleEndian.PutUint32(request[12+index*4:16+index*4], uint32(len(value)))
		copy(request[offsets[index]:], value)
	}
	return request
}

func outcome(t *testing.T, response []byte) AdvanceOutcome {
	t.Helper()
	if len(response) != responseLen || binary.LittleEndian.Uint32(response[0:4]) != responseMagic ||
		binary.LittleEndian.Uint32(response[4:8]) != wireVersion ||
		binary.LittleEndian.Uint32(response[12:16]) != 0 {
		t.Fatalf("invalid response %x", response)
	}
	return AdvanceOutcome(binary.LittleEndian.Uint32(response[8:12]))
}

func TestAdvanceDecisionParity(t *testing.T) {
	tests := []struct {
		name                                     string
		bound, workItem, observed, actual, state string
		nonce, lastNonce                         string
		want                                     AdvanceOutcome
	}{
		{"fresh advance", "wi_1", "wi_1", "understand", "understand", "active", "", "", AdvanceOK},
		{"unbound", "", "wi_1", "understand", "understand", "active", "", "", AdvanceUnbound},
		{"different binding", "wi_2", "wi_1", "understand", "understand", "active", "", "", AdvanceUnbound},
		{"stale stage", "wi_1", "wi_1", "understand", "split", "active", "", "", AdvanceStale},
		{"missing actual stage", "wi_1", "wi_1", "understand", "", "active", "", "", AdvanceStale},
		{"accepted terminal", "wi_1", "wi_1", "understand", "understand", "accepted", "", "", AdvanceTerminal},
		{"rejected terminal", "wi_1", "wi_1", "understand", "understand", "rejected", "", "", AdvanceTerminal},
		{"abandoned terminal", "wi_1", "wi_1", "understand", "understand", "abandoned", "", "", AdvanceTerminal},
		{"missing work item", "wi_1", "", "understand", "understand", "active", "", "", AdvanceBadArgs},
		{"missing observed stage", "wi_1", "wi_1", "", "understand", "active", "", "", AdvanceBadArgs},
		{"replay precedes stale", "wi_1", "wi_1", "understand", "split", "active", "n1", "n1", AdvanceReplay},
		{"replay precedes terminal", "wi_1", "wi_1", "understand", "split", "accepted", "n1", "n1", AdvanceReplay},
		{"different nonce is stale", "wi_1", "wi_1", "understand", "split", "active", "n2", "n1", AdvanceStale},
	}
	for _, test := range tests {
		t.Run(test.name, func(t *testing.T) {
			response, status := Handle(bus.ModuleInvocation{StageID: StageAdvance},
				advanceWire(test.bound, test.workItem, test.observed, test.actual, test.state,
					test.nonce, test.lastNonce))
			if status != bus.ModuleStatusOK {
				t.Fatalf("status = %d", status)
			}
			if got := outcome(t, response); got != test.want {
				t.Fatalf("outcome = %d, want %d", got, test.want)
			}
		})
	}
}

func TestAdvanceRejectsMalformedWire(t *testing.T) {
	valid := func() []byte {
		return advanceWire("wi_1", "wi_1", "understand", "understand", "active", "", "")
	}
	tests := [][]byte{nil, valid()[:requestLen-1]}
	badMagic := valid()
	badMagic[0] = 0
	tests = append(tests, badMagic)
	badVersion := valid()
	badVersion[4]++
	tests = append(tests, badVersion)
	badFlag := valid()
	binary.LittleEndian.PutUint32(badFlag[8:12], 2)
	tests = append(tests, badFlag)
	reserved := valid()
	reserved[40] = 1
	tests = append(tests, reserved)
	tooLong := valid()
	binary.LittleEndian.PutUint32(tooLong[12:16], workItemMax+1)
	tests = append(tests, tooLong)
	padding := valid()
	padding[requestBoundOff+len("wi_1")] = 1
	tests = append(tests, padding)
	embeddedZero := valid()
	embeddedZero[requestWorkItemOff+1] = 0
	tests = append(tests, embeddedZero)
	flagWithoutNonce := valid()
	binary.LittleEndian.PutUint32(flagWithoutNonce[8:12], 1)
	tests = append(tests, flagWithoutNonce)
	nonceWithoutFlag := valid()
	binary.LittleEndian.PutUint32(nonceWithoutFlag[32:36], 2)
	copy(nonceWithoutFlag[requestNonceOff:], "n1")
	tests = append(tests, nonceWithoutFlag)
	for index, request := range tests {
		if _, status := Handle(bus.ModuleInvocation{StageID: StageAdvance}, request); status != bus.ModuleStatusInvalidRequest {
			t.Errorf("malformed request %d status = %d", index, status)
		}
	}
	if _, status := Handle(bus.ModuleInvocation{StageID: StageAdvance + 1}, valid()); status != bus.ModuleStatusInvalidRequest {
		t.Fatalf("wrong-stage status = %d", status)
	}
}

func TestAdvanceWireBounds(t *testing.T) {
	maxWork := strings.Repeat("w", workItemMax)
	maxStage := strings.Repeat("s", stageMax)
	maxState := strings.Repeat("a", stateMax)
	maxNonce := strings.Repeat("n", nonceMax)
	request := advanceWire(maxWork, maxWork, maxStage, maxStage, maxState, maxNonce, maxNonce)
	if _, status := Handle(bus.ModuleInvocation{StageID: StageAdvance}, request); status != bus.ModuleStatusOK {
		t.Fatalf("maximum canonical wire status = %d", status)
	}
}

func TestAdvanceHonorsCancellationAfterValidation(t *testing.T) {
	invocation := bus.ModuleInvocation{StageID: StageAdvance, DeadlineNS: 1}
	valid := advanceWire("wi_1", "wi_1", "understand", "understand", "active", "", "")
	if _, status := Handle(invocation, valid); status != bus.ModuleStatusCancelled {
		t.Fatalf("expired invocation status = %d", status)
	}
	if _, status := Handle(invocation, nil); status != bus.ModuleStatusInvalidRequest {
		t.Fatalf("malformed expired-request status = %d", status)
	}
}
