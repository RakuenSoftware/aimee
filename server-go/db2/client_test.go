package db2

import (
	"context"
	"errors"
	"testing"
	"time"
)

type fakeCaller struct {
	response []byte
	err      error
	calls    int
	ctx      context.Context
	event    uint32
	stage    uint32
	trace    uint64
	deadline time.Duration
	request  []byte
}

func (caller *fakeCaller) Call(ctx context.Context, event, stage uint32, trace uint64,
	deadline time.Duration, request []byte) ([]byte, error) {
	caller.calls++
	caller.ctx = ctx
	caller.event = event
	caller.stage = stage
	caller.trace = trace
	caller.deadline = deadline
	caller.request = append([]byte(nil), request...)
	return caller.response, caller.err
}

func mustClient(t *testing.T, caller StageCaller, deadline time.Duration) *Client {
	t.Helper()
	client, err := NewClient(caller, deadline)
	if err != nil {
		t.Fatalf("NewClient: %v", err)
	}
	return client
}

func TestHealthCallsTheCServedStageAndReturnsTypedEvidence(t *testing.T) {
	want := HealthEvidence{SchemaOK: true, HavePGTrgm: false, KBTablesOK: true}
	caller := &fakeCaller{response: EncodeHealthResponse(want)}
	client := mustClient(t, caller, 250*time.Millisecond)
	ctx := context.WithValue(context.Background(), struct{}{}, "marker")

	got, err := client.Health(ctx, 0xabc)
	if err != nil || got != want {
		t.Fatalf("Health = (%+v, %v), want (%+v, nil)", got, err, want)
	}
	if caller.calls != 1 || caller.ctx != ctx || caller.event != EventHealth ||
		caller.stage != StageHealth || caller.trace != 0xabc || caller.deadline != 250*time.Millisecond {
		t.Fatalf("call = count:%d event:%d stage:%d trace:%x deadline:%s",
			caller.calls, caller.event, caller.stage, caller.trace, caller.deadline)
	}
	if err := DecodeHealthRequest(caller.request); err != nil {
		t.Fatalf("caller sent a request the C module rejects: %v", err)
	}
}

func TestClientDefaultsDeadlineAndAcceptsNilContext(t *testing.T) {
	caller := &fakeCaller{response: EncodeHealthResponse(HealthEvidence{})}
	client := mustClient(t, caller, 0)
	if _, err := client.Health(nil, 0); err != nil {
		t.Fatalf("Health(nil): %v", err)
	}
	if caller.ctx == nil || caller.deadline != DefaultDeadline {
		t.Fatalf("context/deadline = (%v, %s), want nonnil/%s", caller.ctx, caller.deadline, DefaultDeadline)
	}
}

func TestClientRejectsMissingConfigurationWithoutCalling(t *testing.T) {
	if client, err := NewClient(nil, time.Second); client != nil || !errors.Is(err, ErrConfig) {
		t.Fatalf("NewClient(nil) = (%v, %v)", client, err)
	}
	var nilClient *Client
	if evidence, err := nilClient.Health(context.Background(), 1); evidence != (HealthEvidence{}) || !errors.Is(err, ErrConfig) {
		t.Fatalf("nil Health = (%+v, %v)", evidence, err)
	}
	broken := &Client{}
	if _, err := broken.Health(context.Background(), 1); !errors.Is(err, ErrConfig) {
		t.Fatalf("unconfigured Health error = %v", err)
	}
}

func TestHealthPropagatesTransportAndModuleStatusErrors(t *testing.T) {
	for name, want := range map[string]error{
		"transport": errors.New("bus unavailable"),
		"status":    errors.New("module status invalid request"),
	} {
		t.Run(name, func(t *testing.T) {
			caller := &fakeCaller{err: want}
			got, err := mustClient(t, caller, time.Second).Health(context.Background(), 9)
			if got != (HealthEvidence{}) || !errors.Is(err, want) {
				t.Fatalf("Health = (%+v, %v), want zero/%v", got, err, want)
			}
		})
	}
}

func TestHealthRejectsMalformedModuleReplies(t *testing.T) {
	valid := EncodeHealthResponse(HealthEvidence{SchemaOK: true})
	for name, response := range map[string][]byte{
		"nil":      nil,
		"short":    valid[:len(valid)-1],
		"trailing": append(append([]byte(nil), valid...), 0),
		"reserved": append(append([]byte(nil), valid[:12]...), 1, 0, 0, 0),
	} {
		t.Run(name, func(t *testing.T) {
			evidence, err := mustClient(t, &fakeCaller{response: response}, time.Second).
				Health(context.Background(), 1)
			if evidence != (HealthEvidence{}) || !errors.Is(err, ErrMalformedHealth) {
				t.Fatalf("Health = (%+v, %v), want zero/ErrMalformedHealth", evidence, err)
			}
		})
	}
}
