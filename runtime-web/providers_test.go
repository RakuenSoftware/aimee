package main

import (
	"fmt"
	"io"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
)

func TestProviderConnectionProxy(t *testing.T) {
	for _, tc := range []struct {
		path, method, body string
		code               int
	}{
		{"connections", "GET", "", 200},
		{"connection_models", "POST", `{"name":"second"}`, 200},
		{"save_connection", "POST", `{"name":"second","create":true}`, 200},
		{"remove_connection", "POST", `{"name":"second","remove_models":true}`, 200},
		{"save_connection", "POST", `{"name":"duplicate","create":true}`, 400},
	} {
		t.Run(tc.path+tc.body, func(t *testing.T) {
			v1 := http.NewServeMux()
			v1.HandleFunc("/v1/provider/"+tc.path, func(w http.ResponseWriter, r *http.Request) {
				if tc.method == "POST" && r.Header.Get("X-Aimee-Webuser") != "alice" {
					t.Error("missing attested actor")
				}
				body, _ := io.ReadAll(r.Body)
				if r.Method != tc.method || string(body) != tc.body {
					t.Errorf("forwarded %s %s", r.Method, body)
				}
				w.WriteHeader(tc.code)
				if tc.code == 200 {
					fmt.Fprint(w, `{"status":"ok","providers":[]}`)
				} else {
					fmt.Fprint(w, `{"error":"provider name already exists"}`)
				}
			})
			s := &server{cfg: startFakeV1(t, v1)}
			rr := httptest.NewRecorder()
			r := withUser(httptest.NewRequest(tc.method, "/api/providers", strings.NewReader(tc.body)), "alice")
			r.Header.Set("Origin", "http://example.com")
			switch tc.path {
			case "connection_models":
				s.handleProviderConnectionModels(rr, r)
			case "connections":
				s.handleProviderConnections(rr, r)
			case "save_connection":
				s.handleProviderSaveConnection(rr, r)
			case "remove_connection":
				s.handleProviderRemoveConnection(rr, r)
			}
			if rr.Code != tc.code {
				t.Fatalf("status %d: %s", rr.Code, rr.Body.String())
			}
			if !strings.Contains(rr.Body.String(), map[bool]string{true: "providers", false: "already exists"}[tc.code == 200]) {
				t.Fatal(rr.Body.String())
			}
		})
	}
}

func TestProviderMutationRejectsCrossOrigin(t *testing.T) {
	for _, handler := range []func(*server, http.ResponseWriter, *http.Request){(*server).handleProviderSaveConnection, (*server).handleProviderRemoveConnection} {
		s := &server{}
		r := withUser(httptest.NewRequest("POST", "/api/providers/save", strings.NewReader(`{"name":"bad"}`)), "alice")
		r.Header.Set("Origin", "https://untrusted.example")
		out := httptest.NewRecorder()
		handler(s, out, r)
		if out.Code != http.StatusForbidden {
			t.Fatal(out.Code, out.Body.String())
		}
	}
}
