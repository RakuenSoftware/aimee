package config

import (
	"context"
	"encoding/json"
	"os"
	"path/filepath"
	"testing"
	"time"
)

type contractVector struct {
	Name         string `json:"name"`
	RequestJSON  string `json:"request_json"`
	ResponseJSON string `json:"response_json"`
}

func loadContractVectors(t *testing.T) []contractVector {
	t.Helper()
	raw, err := os.ReadFile(filepath.Join("..", "..", "src", "modules", "config",
		"eventcontract", "operations.json"))
	if err != nil {
		t.Fatal(err)
	}
	var document struct {
		EventKind uint32           `json:"event_kind"`
		StageID   uint32           `json:"stage_id"`
		Vectors   []contractVector `json:"vectors"`
	}
	if err := json.Unmarshal(raw, &document); err != nil {
		t.Fatal(err)
	}
	if document.EventKind != EventConfig || document.StageID != StageConfig || len(document.Vectors) == 0 {
		t.Fatalf("contract identity/vectors = %d/%d/%d", document.EventKind, document.StageID,
			len(document.Vectors))
	}
	return document.Vectors
}

type contractCaller struct {
	t        *testing.T
	request  string
	response string
}

func (c contractCaller) Call(_ context.Context, eventKind, stageID uint32, _ uint64,
	_ time.Duration, body []byte) ([]byte, error) {
	c.t.Helper()
	if eventKind != EventConfig || stageID != StageConfig {
		c.t.Fatalf("caller identity = %d/%d", eventKind, stageID)
	}
	if string(body) != c.request {
		c.t.Fatalf("request = %s, want %s", body, c.request)
	}
	return []byte(c.response), nil
}

func TestCallerConformsToOwnedEventContract(t *testing.T) {
	for _, vector := range loadContractVectors(t) {
		t.Run(vector.Name, func(t *testing.T) {
			caller := contractCaller{t: t, request: vector.RequestJSON, response: vector.ResponseJSON}
			client, err := NewClient(caller, time.Second)
			if err != nil {
				t.Fatal(err)
			}
			var request Request
			if err := json.Unmarshal([]byte(vector.RequestJSON), &request); err != nil {
				t.Fatal(err)
			}
			response, err := client.request(request)
			if request.Operation == OpStringValue {
				if moduleError, ok := err.(*ModuleError); !ok || moduleError.Code != "not_found" {
					t.Fatalf("error = %#v", err)
				}
				return
			}
			if err != nil || !response.OK {
				t.Fatalf("response = %+v, error = %v", response, err)
			}
		})
	}
}

func TestCallerRejectsTrailingResponseData(t *testing.T) {
	client, err := NewClient(contractCaller{t: t, request: "{\"operation\":\"values\"}",
		response: "{\"ok\":true,\"values\":{}}{}"}, time.Second)
	if err != nil {
		t.Fatal(err)
	}
	if _, err := client.Values(); err != ErrMalformed {
		t.Fatalf("trailing response error = %v", err)
	}
}
