package memory

import (
	"bytes"
	"context"
	"errors"
	"github.com/RakuenSoftware/ouroboros-ring/go/ring"
	"testing"
	"time"
)

type sdkCallFunc func(context.Context, uint32, uint32, uint64, time.Duration, []byte) ([]byte, error)

func (f sdkCallFunc) Call(ctx context.Context, event, stage uint32, trace uint64, deadline time.Duration, body []byte) ([]byte, error) {
	return f(ctx, event, stage, trace, deadline, body)
}

func TestOpaqueDataJSONKeepsVersionedFieldsAndWideIntegerTokens(t *testing.T) {
	request := []byte(`{ "operation":"future-v2", "metadata":{"future":true,"belief":18446744073709551615,"clock":1792929600000000001} }`)
	reply := []byte(`{ "records":[{"id":9007199254740993}], "future":{"receipt":18446744073709551615} }`)
	calls := 0
	client, err := NewClient(sdkCallFunc(func(_ context.Context, event, stage uint32, trace uint64, deadline time.Duration, body []byte) ([]byte, error) {
		calls++
		if event != EventData || stage != StageData || trace != 13 || deadline <= 0 || deadline > time.Second || !bytes.Equal(request, body) {
			t.Fatalf("rewritten route/context or raw request: %d/%d %s", event, stage, body)
		}
		return reply, nil
	}), time.Second)
	if err != nil {
		t.Fatal(err)
	}
	got, err := client.DataJSON(context.Background(), 13, request)
	if err != nil || !bytes.Equal(got, reply) || calls != 1 {
		t.Fatalf("raw result changed: %s %v calls=%d", got, err, calls)
	}
}
func TestOpaqueDataJSONEnvelopeErrorsDoNotReachAuthority(t *testing.T) {
	calls := 0
	client, _ := NewClient(sdkCallFunc(func(context.Context, uint32, uint32, uint64, time.Duration, []byte) ([]byte, error) {
		calls++
		return nil, nil
	}), 0)
	for _, body := range []string{"", "null", "[]", "{}", `{"operation":17}`, `{"operation":" "}`, `{"operation":"get"} {}`} {
		if _, err := client.DataJSON(context.Background(), 0, []byte(body)); !errors.Is(err, ErrClientRequest) {
			t.Fatalf("accepted invalid envelope %q: %v", body, err)
		}
	}
	if calls != 0 {
		t.Fatalf("invalid envelope reached authority %d times", calls)
	}
}
func TestOpaqueDataJSONPreservesActualAuthorityRefusalWithoutRetry(t *testing.T) {
	for _, failure := range []error{ring.ErrModuleCallCapabilityAbsent, ring.ErrModuleCallCancelled, ring.ErrModuleCallDeadline, &ring.ModuleCallStatusError{Status: ring.ModuleStatusInvalidRequest}} {
		calls := 0
		client, _ := NewClient(sdkCallFunc(func(context.Context, uint32, uint32, uint64, time.Duration, []byte) ([]byte, error) {
			calls++
			return nil, failure
		}), 0)
		if _, err := client.DataJSON(context.Background(), 0, []byte(`{"operation":"store","future_schema":true}`)); !errors.Is(err, failure) || calls != 1 {
			t.Fatalf("refusal rewritten or retried: %v calls=%d", err, calls)
		}
	}
}
