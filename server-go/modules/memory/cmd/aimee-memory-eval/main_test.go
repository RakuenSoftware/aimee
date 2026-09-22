package main

import (
	"bytes"
	"context"
	"encoding/json"
	"os"
	"strings"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
	"github.com/JBailes/aimee/server-go/modules/memory"
)

func TestEvaluationTransportRefusesMalformedFrames(t *testing.T) {
	for _, input := range []string{
		`{"stage":"data","body":{}} {}`,
		`{"stage":"data","body":{},"principal":0}`,
		`{"stage":"data","body":null}`,
		`{"stage":"data","command":"runtime","body":{}}`,
		`{"stage":"unknown","body":{}}`,
		strings.Repeat("x", maxEvaluationLine+1),
	} {
		called := false
		err := serve(context.Background(), strings.NewReader(input), &bytes.Buffer{}, func(bus.ModuleInvocation, []byte) ([]byte, bus.ModuleStatus) {
			called = true
			return nil, bus.ModuleStatusOK
		})
		if err == nil || called {
			t.Fatal("malformed frame reached memory owner", err, called)
		}
	}
}

func TestEvaluationTransportPreservesOwnerFailure(t *testing.T) {
	var output bytes.Buffer
	err := serve(context.Background(), strings.NewReader(`{"stage":"data","body":{"operation":"get","id":1}}`), &output,
		func(invocation bus.ModuleInvocation, body []byte) ([]byte, bus.ModuleStatus) {
			if invocation.StageID != memory.StageData || invocation.PrincipalRef != 0 {
				t.Fatal(invocation)
			}
			return nil, bus.ModuleStatusInternal
		})
	if err != nil {
		t.Fatal(err)
	}
	var reply evaluationReply
	if err := json.Unmarshal(output.Bytes(), &reply); err != nil || reply.Status != bus.ModuleStatusInternal || string(reply.Body) != "null" {
		t.Fatal(output.String(), err)
	}
}

func TestEvaluationSharedOwnerReplay(t *testing.T) {
	url := os.Getenv("AIMEE_DB_TEST_URL")
	if url == "" {
		if os.Getenv("AIMEE_DB_TEST_REQUIRED") == "1" {
			t.Fatal("AIMEE_DB_TEST_URL required")
		}
		t.Skip("set AIMEE_DB_TEST_URL to an explicit disposable PostgreSQL admin DSN")
	}
	t.Setenv("AIMEE_DB2_EVAL_URL", url)
	const schema = "../../../../../src/modules/db2/c/schema.sql"
	const insert = `{"stage":"data","body":{"operation":"insert-epistemic","tier":"L2","kind":"fact","key":"isolated-eval-fixture","content":"isolated-eval-fixture shared Go owner","confidence":0.9,"session_id":"corpus","epistemic_kind":"world_fact","project":"evaluation"}}` + "\n"
	const search = `{"stage":"data","body":{"operation":"search","query":"isolated-eval-fixture","project":"evaluation","limit":10}}` + "\n"
	const score = `{"stage":"command","command":"runtime","body":{"operation":"benchmark-score","query":"isolated-eval-fixture","project":"evaluation","expected_ids":["1"]}}` + "\n"
	var output bytes.Buffer
	if err := run(context.Background(), schema, 3, strings.NewReader(insert+search+score), &output); err != nil {
		t.Fatal(err)
	}
	decoder := json.NewDecoder(&output)
	for i := 0; i < 3; i++ {
		var reply evaluationReply
		if err := decoder.Decode(&reply); err != nil || reply.Status != bus.ModuleStatusOK {
			t.Fatalf("reply %d: %s status %d: %v", i, reply.Body, reply.Status, err)
		}
		if i < 2 {
			var data memory.DataResponse
			if err := json.Unmarshal(reply.Body, &data); err != nil || len(data.Records) != 1 || data.Records[0].ID != 1 || data.Records[0].Key != "isolated-eval-fixture" {
				t.Fatalf("seed/read mismatch: %s: %v", reply.Body, err)
			}
		} else {
			var data struct {
				MRR float64  `json:"mrr"`
				IDs []string `json:"retrieved_ids"`
			}
			if err := json.Unmarshal(reply.Body, &data); err != nil || data.MRR != 1 || len(data.IDs) != 1 || data.IDs[0] != "1" {
				t.Fatalf("score did not see seed: %s: %v", reply.Body, err)
			}
		}
	}
	output.Reset()
	if err := run(context.Background(), schema, 3, strings.NewReader(search+score), &output); err != nil {
		t.Fatal(err)
	}
	decoder = json.NewDecoder(&output)
	var reply evaluationReply
	if err := decoder.Decode(&reply); err != nil || reply.Status != bus.ModuleStatusOK {
		t.Fatal(reply, err)
	}
	var data memory.DataResponse
	if err := json.Unmarshal(reply.Body, &data); err != nil || len(data.Records) != 0 {
		t.Fatal("fresh run reused earlier corpus", string(reply.Body), err)
	}
	if err := decoder.Decode(&reply); err != nil || reply.Status != bus.ModuleStatusOK {
		t.Fatal(reply, err)
	}
	var result struct {
		MRR float64  `json:"mrr"`
		IDs []string `json:"retrieved_ids"`
	}
	if err := json.Unmarshal(reply.Body, &result); err != nil || result.MRR != 0 || len(result.IDs) != 0 {
		t.Fatal("fresh score reused earlier corpus", string(reply.Body), err)
	}
}
