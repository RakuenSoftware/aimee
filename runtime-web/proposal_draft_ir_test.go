package main

import (
	"io"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
)

func TestProposalDraftCarriesCanonicalMessageAndAuthenticatedActor(t *testing.T) {
	const body = ` {"message":{"role":"user","blocks":[{"type":"text","text":"Title: 雪\nNotes: {\"n\":1}"}]},"model":"draft-model"} `
	calls := 0
	cfg := startFakeV1(t, http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		calls++
		got, _ := io.ReadAll(r.Body)
		if r.URL.Path != "/v1/model/draft" || string(got) != body || r.Header.Get("X-Aimee-Webuser") != "alice" || r.Header.Get("Authorization") != "" {
			t.Errorf("draft message/actor changed: path=%s body=%q", r.URL.Path, got)
		}
		_, _ = w.Write([]byte(`{"status":"ok","text":"proposal artifact","agent":"draft-model"}`))
	}))
	s := &server{cfg: cfg}
	req := withUser(httptest.NewRequest("POST", "/api/proposal/draft", strings.NewReader(body)), "alice")
	req.Header.Set("X-Aimee-Webuser", "forged")
	rec := httptest.NewRecorder()
	s.handleProposalDraft(rec, req)
	if rec.Code != 200 || calls != 1 || !strings.Contains(rec.Body.String(), "proposal artifact") {
		t.Fatalf("draft: code=%d calls=%d", rec.Code, calls)
	}
}

func TestProposalDraftRejectsNonIRBeforeTransport(t *testing.T) {
	calls := 0
	cfg := startFakeV1(t, http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) { calls++ }))
	s := &server{cfg: cfg}
	for _, body := range []string{
		`{"prompt":"legacy text does not belong on an internal hop"}`,
		`{"message":{"role":"assistant","blocks":[{"type":"text","text":"wrong role"}]}}`,
		`{"message":{"role":"user","blocks":[{"type":"image","media_type":"image/png","media_ref":"https://fixture.invalid/a.png"}]}}`,
		`{"message":{"role":"user","blocks":[{"type":"text","text":"long enough"}]},"prompt":"ambiguous"}`,
		`{"message":{"role":"user","blocks":[{"type":"text","text":"long enough","cache_control":"ephemeral"}]}}`,
		`{"message":{"role":"user","blocks":[{"type":"text","text":"long enough"}]} ,"message":null}`,
	} {
		rec := httptest.NewRecorder()
		s.handleProposalDraft(rec, withUser(httptest.NewRequest("POST", "/api/proposal/draft", strings.NewReader(body)), "alice"))
		if rec.Code != http.StatusBadRequest || calls != 0 {
			t.Fatalf("invalid draft admitted: code=%d body=%s calls=%d", rec.Code, body, calls)
		}
	}
}
