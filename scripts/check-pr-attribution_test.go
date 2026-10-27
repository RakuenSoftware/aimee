package main

import (
	"context"
	"errors"
	"io"
	"net/http"
	"net/http/cookiejar"
	"net/http/httptest"
	"net/url"
	"strings"
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

type attributionTransport func(*http.Request) (*http.Response, error)

func (f attributionTransport) RoundTrip(r *http.Request) (*http.Response, error) { return f(r) }
func TestGitHubJobLogDownloadDropsIdentityAndRejectsSecondRedirect(t *testing.T) {
	for _, secondRedirect := range []bool{false, true} {
		calls := 0
		client := &http.Client{Transport: attributionTransport(func(r *http.Request) (*http.Response, error) {
			calls++
			header := make(http.Header)
			status := 200
			body := "exact log bytes"
			if calls == 1 {
				if r.URL.String() != "https://api.github.com/repos/owner/repo/actions/jobs/77/logs" || r.Header.Get("Authorization") != "Bearer fixture" {
					t.Error("first-hop binding/auth changed")
				}
				status = 302
				header.Set("Location", "https://logs.example.net/signed?sig=fixture")
				body = ""
			} else {
				if calls != 2 || r.URL.String() != "https://logs.example.net/signed?sig=fixture" || r.Header.Get("Authorization") != "" || r.Header.Get("Cookie") != "" || r.Header.Get("Accept") != "text/plain" {
					t.Error("signed download forwarded identity or escaped exact target")
				}
				if secondRedirect {
					status = 302
					header.Set("Location", "https://other.example.net/")
				}
			}
			return &http.Response{StatusCode: status, Header: header, Body: io.NopCloser(strings.NewReader(body)), Request: r}, nil
		}), CheckRedirect: func(*http.Request, []*http.Request) error { return http.ErrUseLastResponse }}
		jar, _ := cookiejar.New(nil)
		signed, _ := url.Parse("https://logs.example.net/")
		jar.SetCookies(signed, []*http.Cookie{{Name: "private-fixture", Value: "must-not-forward"}})
		client.Jar = jar
		raw, err := readAttributionAPI(context.Background(), client, "https://api.github.com/repos/owner/repo/actions/jobs/77/logs", "fixture")
		if calls != 2 || (err != nil) != secondRedirect || !secondRedirect && string(raw) != "exact log bytes" {
			t.Fatalf("calls=%d raw=%q error=%v", calls, raw, err)
		}
	}
}
func TestLogRedirectRequiresExactTrustedFirstHopAndHTTPS(t *testing.T) {
	for _, tc := range []struct{ source, target string }{{"https://api.github.com/repos/owner/repo/issues/1", "https://logs.example.net/"}, {"https://elsewhere.example/repos/owner/repo/actions/jobs/77/logs", "https://logs.example.net/"}, {"https://api.github.com/repos/owner/repo/actions/jobs/77/logs", "http://logs.example.net/"}, {"https://api.github.com/repos/owner/repo/actions/jobs/77/logs", "https://user@logs.example.net/"}, {"https://api.github.com/repos/owner/repo/actions/jobs/77/logs", "https://logs.example.net:8443/"}} {
		calls := 0
		client := &http.Client{Transport: attributionTransport(func(r *http.Request) (*http.Response, error) {
			calls++
			return &http.Response{StatusCode: 302, Header: http.Header{"Location": []string{tc.target}}, Body: io.NopCloser(strings.NewReader("")), Request: r}, nil
		}), CheckRedirect: func(*http.Request, []*http.Request) error { return http.ErrUseLastResponse }}
		if _, err := readAttributionAPI(context.Background(), client, tc.source, "fixture"); err == nil || calls != 1 {
			t.Fatal("untrusted redirect followed", tc, calls, err)
		}
	}
}
