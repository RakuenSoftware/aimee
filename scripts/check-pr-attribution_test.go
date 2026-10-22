package main

import (
	"context"
	"errors"
	"net/http"
	"net/http/httptest"
	"sync/atomic"
	"testing"
	"time"
)

func TestAttributionTransientReadPreservesPolicyInput(t *testing.T) {
	var calls atomic.Int32
	payload := `{"body":"Co-authored-by: forbidden"}`
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.Method != "GET" || r.Header.Get("Authorization") != "Bearer fixture" {
			t.Error("method/authentication changed")
		}
		if calls.Add(1) == 1 {
			w.WriteHeader(503)
			return
		}
		_, _ = w.Write([]byte(payload))
	}))
	defer server.Close()
	body, err := readAttributionAPI(context.Background(), server.Client(), server.URL, "fixture")
	if err != nil || string(body) != payload || calls.Load() != 2 {
		t.Fatalf("body=%q calls=%d error=%v", body, calls.Load(), err)
	}
}

func TestAttributionUnavailableAndUnauthorizedStillDeny(t *testing.T) {
	for _, status := range []int{503, 403} {
		t.Run(http.StatusText(status), func(t *testing.T) {
			var calls atomic.Int32
			server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) { calls.Add(1); w.WriteHeader(status) }))
			defer server.Close()
			body, err := readAttributionAPI(context.Background(), server.Client(), server.URL, "fixture")
			want := int32(1)
			if status == 503 {
				want = 3
			}
			if body != nil || err == nil || calls.Load() != want {
				t.Fatalf("body=%q calls=%d error=%v", body, calls.Load(), err)
			}
		})
	}
}

func TestAttributionRejectsTruncatedMetadataAndRedirect(t *testing.T) {
	for _, redirect := range []bool{false, true} {
		var calls atomic.Int32
		server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) {
			calls.Add(1)
			if redirect {
				w.Header().Set("Location", "/other")
				w.WriteHeader(302)
				return
			}
			w.Header().Set("Content-Length", "100")
			_, _ = w.Write([]byte("{"))
		}))
		client := server.Client()
		client.CheckRedirect = func(*http.Request, []*http.Request) error { return http.ErrUseLastResponse }
		body, err := readAttributionAPI(context.Background(), client, server.URL, "fixture")
		server.Close()
		if body != nil || err == nil || calls.Load() != 1 {
			t.Fatalf("redirect=%v body=%q calls=%d error=%v", redirect, body, calls.Load(), err)
		}
	}
}

func TestAttributionRetryCancellation(t *testing.T) {
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) { w.WriteHeader(503); cancel() }))
	defer server.Close()
	started := time.Now()
	_, err := readAttributionAPI(ctx, server.Client(), server.URL, "fixture")
	if !errors.Is(err, context.Canceled) || time.Since(started) > time.Second {
		t.Fatalf("retry ignored cancellation: %v", err)
	}
}
