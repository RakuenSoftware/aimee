package engine

import (
	"context"
	"encoding/json"
	"testing"
	"time"

	roundtablemod "github.com/JBailes/aimee/server-go/modules/roundtable"
	roundtablecfg "github.com/JBailes/aimee/server-go/modules/roundtable/panel"
)

type recordingReviewCaller struct {
	kind    uint32
	stage   uint32
	timeout time.Duration
	body    []byte
}

func (c *recordingReviewCaller) Call(_ context.Context, kind, stage uint32, _ uint64,
	timeout time.Duration, body []byte) ([]byte, error) {
	c.kind, c.stage, c.timeout, c.body = kind, stage, timeout, append([]byte(nil), body...)
	return json.Marshal(roundtablecfg.RunResult{})
}

func TestBusReviewerReusesExistingCaller(t *testing.T) {
	caller := &recordingReviewCaller{}
	reviewer, err := NewBusReviewerFromCaller(caller, 3*time.Second)
	if err != nil {
		t.Fatal(err)
	}
	request := roundtablecfg.ReviewRequest{OriginalRequest: "release review"}
	if _, err := reviewer.Review(context.Background(), request); err != nil {
		t.Fatal(err)
	}
	if caller.kind != roundtablemod.EventReview || caller.stage != roundtablemod.StageReview {
		t.Fatalf("call target = kind %d stage %d", caller.kind, caller.stage)
	}
	if caller.timeout != 3*time.Second {
		t.Fatalf("timeout = %s", caller.timeout)
	}
	var got roundtablecfg.ReviewRequest
	if err := json.Unmarshal(caller.body, &got); err != nil {
		t.Fatal(err)
	}
	if got.OriginalRequest != request.OriginalRequest {
		t.Fatalf("original request = %q", got.OriginalRequest)
	}
}

func TestBusReviewerRejectsMissingCaller(t *testing.T) {
	if _, err := NewBusReviewerFromCaller(nil, time.Second); err == nil {
		t.Fatal("expected a missing caller error")
	}
}
