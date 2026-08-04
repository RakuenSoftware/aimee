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

func latencyRequest(values []float64) []byte {
	request := make([]byte, latencyRequestLen)
	binary.LittleEndian.PutUint32(request[0:4], latencyRequestMagic)
	binary.LittleEndian.PutUint32(request[4:8], wireVersion)
	binary.LittleEndian.PutUint32(request[8:12], uint32(len(values)))
	for index, value := range values {
		offset := latencyValuesOff + index*8
		binary.LittleEndian.PutUint64(request[offset:offset+8], math.Float64bits(value))
	}
	return request
}

func decodeLatency(t *testing.T, response []byte) (uint32, [5]float64) {
	t.Helper()
	if len(response) != latencyResponseLen ||
		binary.LittleEndian.Uint32(response[0:4]) != latencyResponseMagic ||
		binary.LittleEndian.Uint32(response[4:8]) != wireVersion ||
		binary.LittleEndian.Uint32(response[12:16]) != 0 {
		t.Fatalf("invalid latency response %x", response)
	}
	var values [5]float64
	for index := range values {
		offset := 16 + index*8
		values[index] = math.Float64frombits(binary.LittleEndian.Uint64(response[offset : offset+8]))
	}
	return binary.LittleEndian.Uint32(response[8:12]), values
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

func TestLatencySummaryParity(t *testing.T) {
	tests := []struct {
		name   string
		input  []float64
		values [5]float64
	}{
		{"one", []float64{3.5}, [5]float64{3.5, 3.5, 3.5, 3.5, 3.5}},
		{"unsorted", []float64{10, 1, 5, 3, 8}, [5]float64{5, 10, 10, 1, 10}},
		{"nearest rank", []float64{1, 2, 3, 4, 5, 6, 7, 8, 9, 10}, [5]float64{6, 10, 10, 1, 10}},
	}
	for _, test := range tests {
		t.Run(test.name, func(t *testing.T) {
			response, status := Handle(bus.ModuleInvocation{StageID: StageLatency},
				latencyRequest(test.input))
			if status != bus.ModuleStatusOK {
				t.Fatalf("status = %d", status)
			}
			count, values := decodeLatency(t, response)
			if count != uint32(len(test.input)) || values != test.values {
				t.Fatalf("summary = %d/%v, want %d/%v", count, values, len(test.input), test.values)
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
	if _, status := Handle(bus.ModuleInvocation{StageID: StageLatency + 1},
		scoreRequest(nil, nil, 1)); status != bus.ModuleStatusInvalidRequest {
		t.Fatalf("wrong-stage status = %d", status)
	}
}

func TestBenchmarksRejectsMalformedLatencyWire(t *testing.T) {
	tests := [][]byte{nil, latencyRequest(nil), latencyRequest([]float64{-1}),
		latencyRequest([]float64{math.NaN()}), latencyRequest([]float64{math.Inf(1)})}
	badMagic := latencyRequest([]float64{1})
	badMagic[0] = 0
	tests = append(tests, badMagic)
	badVersion := latencyRequest([]float64{1})
	badVersion[4]++
	tests = append(tests, badVersion)
	badCount := latencyRequest([]float64{1})
	binary.LittleEndian.PutUint32(badCount[8:12], maxLatencies+1)
	tests = append(tests, badCount)
	reserved := latencyRequest([]float64{1})
	reserved[12] = 1
	tests = append(tests, reserved)
	padding := latencyRequest([]float64{1})
	padding[latencyValuesOff+8] = 1
	tests = append(tests, padding)
	for index, request := range tests {
		if _, status := Handle(bus.ModuleInvocation{StageID: StageLatency}, request); status != bus.ModuleStatusInvalidRequest {
			t.Errorf("malformed latency request %d status = %d", index, status)
		}
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
	invocation.StageID = StageLatency
	if _, status := Handle(invocation, latencyRequest([]float64{1})); status != bus.ModuleStatusCancelled {
		t.Fatalf("expired latency invocation status = %d", status)
	}
	if _, status := Handle(invocation, latencyRequest(nil)); status != bus.ModuleStatusInvalidRequest {
		t.Fatalf("malformed expired latency request status = %d", status)
	}
}
