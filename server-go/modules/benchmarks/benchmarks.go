// Package benchmarks implements the benchmarks process wire contract.
package benchmarks

import (
	"encoding/binary"
	"math"

	"github.com/JBailes/aimee/server-go/bus"
)

const (
	EventRun uint32 = 10497
	StageRun uint32 = 1

	requestMagic        uint32 = 0x51524942
	responseMagic       uint32 = 0x53524942
	wireVersion         uint32 = 1
	maxResults                 = 32
	requestRetrievedOff        = 24
	requestRelevantOff         = 280
	requestLen                 = 536
	responseLen                = 32
)

func zeroPadding(value []byte) bool {
	for _, item := range value {
		if item != 0 {
			return false
		}
	}
	return true
}

func isRelevant(id int64, relevant []int64) bool {
	for _, candidate := range relevant {
		if candidate == id {
			return true
		}
	}
	return false
}

func score(retrieved, relevant []int64, k int) (mrr, ndcg, recall float64) {
	for index, id := range retrieved {
		if isRelevant(id, relevant) {
			mrr = 1 / float64(index+1)
			break
		}
	}
	if len(relevant) == 0 {
		return mrr, 0, 0
	}

	limit := min(len(retrieved), k)
	var dcg float64
	found := 0
	for index, id := range retrieved[:limit] {
		if isRelevant(id, relevant) {
			dcg += 1 / math.Log2(float64(index)+2)
			found++
		}
	}
	idealLimit := min(len(relevant), k)
	var idcg float64
	for index := range idealLimit {
		idcg += 1 / math.Log2(float64(index)+2)
	}
	if idcg > 0 {
		ndcg = dcg / idcg
	}
	return mrr, ndcg, float64(found) / float64(len(relevant))
}

// Handle calculates the legacy IR metrics for one bounded retrieval result set.
func Handle(invocation bus.ModuleInvocation, request []byte) ([]byte, bus.ModuleStatus) {
	if invocation.StageID != StageRun || len(request) != requestLen ||
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
	mrr, ndcg, recall := score(retrieved, relevant, k)
	response := make([]byte, responseLen)
	binary.LittleEndian.PutUint32(response[0:4], responseMagic)
	binary.LittleEndian.PutUint32(response[4:8], wireVersion)
	binary.LittleEndian.PutUint64(response[8:16], math.Float64bits(mrr))
	binary.LittleEndian.PutUint64(response[16:24], math.Float64bits(ndcg))
	binary.LittleEndian.PutUint64(response[24:32], math.Float64bits(recall))
	return response, bus.ModuleStatusOK
}
