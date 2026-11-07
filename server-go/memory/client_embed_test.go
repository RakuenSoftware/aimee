package memory

import (
	"context"
	"encoding/json"
	"errors"
	"reflect"
	"testing"
	"time"
)

type embedReply string

func (reply embedReply) Call(context.Context, uint32, uint32, uint64, time.Duration, []byte) ([]byte, error) {
	return []byte(reply), nil
}

func TestClientEmbedOperationShapes(t *testing.T) {
	for _, test := range []struct {
		name, operation, body string
		invalid               bool
	}{
		{"batch", "batch", `{"dim":3,"vectors":[[0.25,0.5,0.75],[1.25,1.5,1.75]]}`, false},
		{"dimension only", "dimension", `{"dim":3}`, false},
		{"dimension probe", "dimension", `{"dim":3,"vector":[0.25,0.5,0.75]}`, false},
		{"truncated dimension probe", "dimension", `{"dim":8,"vector":[1,2,3,4,5,6,7,8],"truncated":true}`, false},
		{"single", "", `{"dim":3,"vector":[0.25,0.5,0.75]}`, false},
		{"serving identity", "serving-id", `{"dim":0,"serving_id":"model-v1"}`, false},
		{"repair counts", "repair-failed", `{"dim":0,"repaired":2,"failed":1}`, false},
		{"batch missing row", "batch", `{"dim":3,"vectors":[[1,2,3]]}`, true},
		{"batch ragged", "batch", `{"dim":3,"vectors":[[1,2,3],[4,5]]}`, true},
		{"batch missing vectors", "batch", `{"dim":3}`, true},
		{"batch zero width", "batch", `{"dim":0,"vectors":[[],[]]}`, true},
		{"batch empty completion", "batch", `{"dim":0}`, true},
		{"batch over cap", "batch", `{"dim":9,"vectors":[[1,2,3,4,5,6,7,8,9],[1,2,3,4,5,6,7,8,9]]}`, true},
		{"batch single vector", "batch", `{"dim":3,"vector":[1,2,3]}`, true},
		{"dimension mismatched probe", "dimension", `{"dim":3,"vector":[1,2]}`, true},
		{"dimension batch", "dimension", `{"dim":3,"vectors":[[1,2,3],[4,5,6]]}`, true},
		{"dimension empty completion", "dimension", `{"dim":0}`, true},
		{"dimension over cap", "dimension", `{"dim":9}`, true},
		{"dimension probe over cap", "dimension", `{"dim":9,"vector":[1,2,3,4,5,6,7,8,9]}`, true},
		{"single missing vector", "", `{"dim":3}`, true},
		{"missing dimension", "dimension", `{}`, true},
		{"negative dimension", "dimension", `{"dim":-1}`, true},
	} {
		t.Run(test.name, func(t *testing.T) {
			client, err := NewClient(embedReply(test.body), 0)
			if err != nil {
				t.Fatal(err)
			}
			got, err := client.Embed(t.Context(), 1, EmbedRequest{Operation: test.operation, Texts: []string{"alpha", "beta"}, MaxDim: 8})
			if test.invalid {
				if !errors.Is(err, ErrClientResponse) {
					t.Fatalf("expected malformed response, got %+v, %v", got, err)
				}
				return
			}
			var want EmbedResponse
			if decodeErr := json.Unmarshal([]byte(test.body), &want); decodeErr != nil {
				t.Fatal(decodeErr)
			}
			if err != nil || !reflect.DeepEqual(got, want) {
				t.Fatalf("got %+v, %v; want %+v", got, err, want)
			}
		})
	}
}

func TestClientEmbedPreservesBatchAndDimensionOutcomes(t *testing.T) {
	for _, operation := range []string{"batch", "dimension"} {
		for _, body := range []string{
			`{"dim":0,"unavailable":true,"retry_after_ms":123}`,
			`{"dim":0,"unauthorized":true,"error":"HTTP 403"}`,
			`{"dim":0,"error":"invalid vector"}`,
		} {
			client, _ := NewClient(embedReply(body), 0)
			got, err := client.Embed(t.Context(), 1, EmbedRequest{Operation: operation, Texts: []string{"alpha", "beta"}, MaxDim: 8})
			var want EmbedResponse
			if decodeErr := json.Unmarshal([]byte(body), &want); decodeErr != nil {
				t.Fatal(decodeErr)
			}
			if err != nil || !reflect.DeepEqual(got, want) {
				t.Fatalf("%s: got %+v, %v; want %+v", operation, got, err, want)
			}
		}
	}
}
