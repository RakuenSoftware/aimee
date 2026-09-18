// Package benchmarks implements the benchmarks process wire contract.
package benchmarks

import (
	"encoding/binary"
	"math"
	"sort"

	"github.com/JBailes/aimee/server-go/bus"
	"github.com/JBailes/aimee/server-go/internal/retrievalmetrics"
)

const (
	EventRun     uint32 = 10497
	StageRun     uint32 = 1
	EventLatency uint32 = 10498
	StageLatency uint32 = 2

	requestMagic         uint32 = 0x51524942
	responseMagic        uint32 = 0x53524942
	wireVersion          uint32 = 1
	maxResults                  = 32
	requestRetrievedOff         = 24
	requestRelevantOff          = 280
	requestLen                  = 536
	responseLen                 = 32
	latencyRequestMagic  uint32 = 0x51524c42
	latencyResponseMagic uint32 = 0x53524c42
	maxLatencies                = 512
	latencyValuesOff            = 16
	latencyRequestLen           = 4112
	latencyResponseLen          = 56
)

func zeroPadding(value []byte) bool {
	for _, item := range value {
		if item != 0 {
			return false
		}
	}
	return true
}

func handleScore(invocation bus.ModuleInvocation, request []byte) ([]byte, bus.ModuleStatus) {
	if len(request) != requestLen ||
		binary.LittleEndian.Uint32(request[0:4]) != requestMagic ||
		binary.LittleEndian.Uint32(request[4:8]) != wireVersion ||
		binary.LittleEndian.Uint32(request[8:12]) == 0 ||
		binary.LittleEndian.Uint32(request[8:12]) > maxResults ||
		binary.LittleEndian.Uint32(request[12:16]) > maxResults ||
		binary.LittleEndian.Uint32(request[16:20]) > maxResults ||
		binary.LittleEndian.Uint32(request[20:24]) != 0 {
		return nil, bus.ModuleStatusInvalidRequest
	}
	k := int(binary.LittleEndian.Uint32(request[8:12]))
	retrievedCount := int(binary.LittleEndian.Uint32(request[12:16]))
	relevantCount := int(binary.LittleEndian.Uint32(request[16:20]))
	if !zeroPadding(request[requestRetrievedOff+retrievedCount*8:requestRelevantOff]) ||
		!zeroPadding(request[requestRelevantOff+relevantCount*8:]) {
		return nil, bus.ModuleStatusInvalidRequest
	}
	if invocation.Cancelled() {
		return nil, bus.ModuleStatusCancelled
	}

	retrieved := make([]int64, retrievedCount)
	for index := range retrieved {
		offset := requestRetrievedOff + index*8
		retrieved[index] = int64(binary.LittleEndian.Uint64(request[offset : offset+8]))
	}
	relevant := make([]int64, relevantCount)
	for index := range relevant {
		offset := requestRelevantOff + index*8
		relevant[index] = int64(binary.LittleEndian.Uint64(request[offset : offset+8]))
	}
	mrr, ndcg, recall := retrievalmetrics.Score(retrieved, relevant, k)
	response := make([]byte, responseLen)
	binary.LittleEndian.PutUint32(response[0:4], responseMagic)
	binary.LittleEndian.PutUint32(response[4:8], wireVersion)
	binary.LittleEndian.PutUint64(response[8:16], math.Float64bits(mrr))
	binary.LittleEndian.PutUint64(response[16:24], math.Float64bits(ndcg))
	binary.LittleEndian.PutUint64(response[24:32], math.Float64bits(recall))
	return response, bus.ModuleStatusOK
}

func nearestRank(sorted []float64, percentile float64) float64 {
	index := int(percentile/100*float64(len(sorted)-1) + 0.5)
	return sorted[index]
}

func handleLatency(invocation bus.ModuleInvocation, request []byte) ([]byte, bus.ModuleStatus) {
	if len(request) != latencyRequestLen ||
		binary.LittleEndian.Uint32(request[0:4]) != latencyRequestMagic ||
		binary.LittleEndian.Uint32(request[4:8]) != wireVersion ||
		binary.LittleEndian.Uint32(request[8:12]) == 0 ||
		binary.LittleEndian.Uint32(request[8:12]) > maxLatencies ||
		binary.LittleEndian.Uint32(request[12:16]) != 0 {
		return nil, bus.ModuleStatusInvalidRequest
	}
	count := int(binary.LittleEndian.Uint32(request[8:12]))
	if !zeroPadding(request[latencyValuesOff+count*8:]) {
		return nil, bus.ModuleStatusInvalidRequest
	}
	latencies := make([]float64, count)
	for index := range latencies {
		offset := latencyValuesOff + index*8
		latencies[index] = math.Float64frombits(binary.LittleEndian.Uint64(request[offset : offset+8]))
		if math.IsNaN(latencies[index]) || math.IsInf(latencies[index], 0) || latencies[index] < 0 {
			return nil, bus.ModuleStatusInvalidRequest
		}
	}
	if invocation.Cancelled() {
		return nil, bus.ModuleStatusCancelled
	}

	sort.Float64s(latencies)
	values := [...]float64{
		nearestRank(latencies, 50),
		nearestRank(latencies, 95),
		nearestRank(latencies, 99),
		latencies[0],
		latencies[len(latencies)-1],
	}
	response := make([]byte, latencyResponseLen)
	binary.LittleEndian.PutUint32(response[0:4], latencyResponseMagic)
	binary.LittleEndian.PutUint32(response[4:8], wireVersion)
	binary.LittleEndian.PutUint32(response[8:12], uint32(count))
	for index, value := range values {
		offset := 16 + index*8
		binary.LittleEndian.PutUint64(response[offset:offset+8], math.Float64bits(value))
	}
	return response, bus.ModuleStatusOK
}

// Handle calculates deterministic benchmark summaries for bounded inputs.
func Handle(invocation bus.ModuleInvocation, request []byte) ([]byte, bus.ModuleStatus) {
	switch invocation.StageID {
	case StageRun:
		return handleScore(invocation, request)
	case StageLatency:
		return handleLatency(invocation, request)
	default:
		return nil, bus.ModuleStatusInvalidRequest
	}
}
