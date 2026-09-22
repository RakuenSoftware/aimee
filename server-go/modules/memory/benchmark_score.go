package memory

import (
	"encoding/json"
	"strconv"

	"github.com/JBailes/aimee/server-go/bus"
	"github.com/JBailes/aimee/server-go/internal/retrievalmetrics"
)

type benchmarkScore struct {
	Status   string   `json:"status"`
	IDs      []string `json:"retrieved_ids"`
	MRR      float64  `json:"mrr"`
	NDCG5    float64  `json:"ndcg_5"`
	NDCG10   float64  `json:"ndcg_10"`
	Recall5  float64  `json:"recall_5"`
	Recall10 float64  `json:"recall_10"`
}

func benchmarkExpectedIDs(args commandArgs, maximum int) ([]int64, bool) {
	var ids []string
	if json.Unmarshal(args["expected_ids"], &ids) != nil || ids == nil || len(ids) > maximum {
		return nil, false
	}
	expected := make([]int64, len(ids))
	for i, id := range ids {
		n, err := strconv.ParseInt(id, 10, 64)
		if err != nil || n <= 0 || strconv.FormatInt(n, 10) != id {
			return nil, false
		}
		expected[i] = n
	}
	return expected, true
}

func handleBenchmarkScore(options handlerOptions, invocation bus.ModuleInvocation, args commandArgs) ([]byte, bus.ModuleStatus) {
	query, validQuery := args.stringValue("query")
	expected, validIDs := benchmarkExpectedIDs(args, 128)
	if !validQuery || !validIDs {
		return nil, bus.ModuleStatusInvalidRequest
	}
	response, status := benchmarkRead(options, invocation, args, DataRequest{Operation: "search", Query: query, Limit: 20})
	if status != bus.ModuleStatusOK {
		return nil, status
	}
	if len(response.Records) > 20 {
		return nil, bus.ModuleStatusInternal
	}
	result := benchmarkScore{Status: "ok", IDs: make([]string, len(response.Records))}
	retrieved := make([]int64, len(response.Records))
	for i, record := range response.Records {
		if record.ID <= 0 {
			return nil, bus.ModuleStatusInternal
		}
		retrieved[i], result.IDs[i] = record.ID, strconv.FormatInt(record.ID, 10)
	}
	result.MRR, result.NDCG5, result.Recall5 = retrievalmetrics.Score(retrieved, expected, 5)
	_, result.NDCG10, result.Recall10 = retrievalmetrics.Score(retrieved, expected, 10)
	return commandResult(result)
}
