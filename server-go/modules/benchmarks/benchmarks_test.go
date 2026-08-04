package benchmarks

import (
	"encoding/binary"
	"math"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
)

func scoreRequest(retrieved, relevant []int64, k uint32) []byte {
	request := make([]byte, requestLen)
	binary.LittleEndian.PutUint32(request[0:4], requestMagic)
	binary.LittleEndian.PutUint32(request[4:8], wireVersion)
	binary.LittleEndian.PutUint32(request[8:12], k)
	binary.LittleEndian.PutUint32(request[12:16], uint32(len(retrieved)))
	binary.LittleEndian.PutUint32(request[16:20], uint32(len(relevant)))
	for index, id := range retrieved {
		offset := requestRetrievedOff + index*8
		binary.LittleEndian.PutUint64(request[offset:offset+8], uint64(id))
	}
	for index, id := range relevant {
		offset := requestRelevantOff + index*8
		binary.LittleEndian.PutUint64(request[offset:offset+8], uint64(id))
	}
	return request
}

func decodeScores(t *testing.T, response []byte) (float64, float64, float64) {
	t.Helper()
	if len(response) != responseLen || binary.LittleEndian.Uint32(response[0:4]) != responseMagic ||
		binary.LittleEndian.Uint32(response[4:8]) != wireVersion {
		t.Fatalf("invalid response %x", response)
	}
	return math.Float64frombits(binary.LittleEndian.Uint64(response[8:16])),
		math.Float64frombits(binary.LittleEndian.Uint64(response[16:24])),
		math.Float64frombits(binary.LittleEndian.Uint64(response[24:32]))
}

func closeEnough(left, right float64) bool {
	return math.Abs(left-right) < 1e-12
}

func TestIRScoringParity(t *testing.T) {
	tests := []struct {
		name                string
		retrieved, relevant []int64
		k                   uint32
		mrr, ndcg, recall   float64
	}{
		{"perfect", []int64{11, 22, 33}, []int64{11, 22, 33}, 3, 1, 1, 1},
		{"rank two", []int64{5, 9, 7}, []int64{9}, 3, 0.5, 1 / math.Log2(3), 1},
		{"partial", []int64{1, 2, 3}, []int64{2, 4}, 2, 0.5,
			(1 / math.Log2(3)) / (1 + 1/math.Log2(3)), 0.5},
		{"cut off", []int64{1, 2}, []int64{2}, 1, 0.5, 0, 0},
		{"no relevant", []int64{1, 2}, nil, 2, 0, 0, 0},
		// The legacy C metrics count duplicate retrieved IDs independently.
		{"legacy duplicate", []int64{7, 7}, []int64{7}, 2, 1,
			1 + 1/math.Log2(3), 2},
	}
	for _, test := range tests {
		t.Run(test.name, func(t *testing.T) {
			response, status := Handle(bus.ModuleInvocation{StageID: StageRun},
				scoreRequest(test.retrieved, test.relevant, test.k))
			if status != bus.ModuleStatusOK {
				t.Fatalf("status = %d", status)
			}
			mrr, ndcg, recall := decodeScores(t, response)
			if !closeEnough(mrr, test.mrr) || !closeEnough(ndcg, test.ndcg) ||
				!closeEnough(recall, test.recall) {
				t.Fatalf("scores = %.15f/%.15f/%.15f, want %.15f/%.15f/%.15f",
					mrr, ndcg, recall, test.mrr, test.ndcg, test.recall)
			}
		})
	}
}

func TestBenchmarksRejectsMalformedWire(t *testing.T) {
	tests := [][]byte{nil, scoreRequest(nil, nil, 0), scoreRequest(nil, nil, maxResults+1)}
	badMagic := scoreRequest(nil, nil, 1)
	badMagic[0] = 0
	tests = append(tests, badMagic)
	badVersion := scoreRequest(nil, nil, 1)
	badVersion[4]++
	tests = append(tests, badVersion)
	badCount := scoreRequest(nil, nil, 1)
	binary.LittleEndian.PutUint32(badCount[12:16], maxResults+1)
	tests = append(tests, badCount)
	reserved := scoreRequest(nil, nil, 1)
	reserved[20] = 1
	tests = append(tests, reserved)
	padding := scoreRequest([]int64{1}, nil, 1)
	padding[requestRetrievedOff+8] = 1
	tests = append(tests, padding)
	for index, request := range tests {
		if _, status := Handle(bus.ModuleInvocation{StageID: StageRun}, request); status != bus.ModuleStatusInvalidRequest {
			t.Errorf("malformed request %d status = %d", index, status)
		}
	}
	if _, status := Handle(bus.ModuleInvocation{StageID: StageRun + 1},
		scoreRequest(nil, nil, 1)); status != bus.ModuleStatusInvalidRequest {
		t.Fatalf("wrong-stage status = %d", status)
	}
}

func TestBenchmarksHonorsCancellationAfterValidation(t *testing.T) {
	invocation := bus.ModuleInvocation{StageID: StageRun, DeadlineNS: 1}
	if _, status := Handle(invocation, scoreRequest([]int64{1}, []int64{1}, 1)); status != bus.ModuleStatusCancelled {
		t.Fatalf("expired invocation status = %d", status)
	}
	if _, status := Handle(invocation, nil); status != bus.ModuleStatusInvalidRequest {
		t.Fatalf("malformed expired-request status = %d", status)
	}
}
