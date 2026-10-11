package memory

import (
	"context"
	"errors"
	"testing"
	"time"
)

type replyCaller string

func (r replyCaller) Call(context.Context, uint32, uint32, uint64, time.Duration, []byte) ([]byte, error) {
	return []byte(r), nil
}
func TestStorePreservesRefusals(t *testing.T) {
	for _, tc := range []struct {
		body string
		want error
	}{
		{`{"records":[],"failure":{"kind":"capacity","retryable":false}}`, ErrCapacity},
		{`{"records":[],"failure":{"kind":"unsupported","retryable":false}}`, ErrUnsupported},
		{`{"records":[],"failure":{"kind":"unavailable","retryable":true}}`, ErrUnavailable},
	} {
		client, _ := NewClient(replyCaller(tc.body), time.Second)
		_, err := (ClientStore{Client: client}).Search(context.Background(), Scope{Type: "global", Value: "_global"}, "height", "", "", 10)
		if !errors.Is(err, tc.want) {
			t.Fatalf("got %v, want %v", err, tc.want)
		}
	}
	client, _ := NewClient(replyCaller(`{"records":[],"code":-2}`), time.Second)
	_, err := (ClientStore{Client: client}).Put(context.Background(), Scope{Type: "global", Value: "_global"}, Record{Content: "x"})
	var refusal *RefusalError
	if !errors.As(err, &refusal) || int32(refusal.Code) != -2 {
		t.Fatalf("lost signed refusal: %v", err)
	}
}
