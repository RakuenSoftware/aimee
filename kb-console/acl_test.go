package main

import "testing"

func TestConsoleAdminAllows_Allowed(t *testing.T) {
	ok := [][2]string{
		{"GET", "/v1/console/overview"},
		{"GET", "/v1/enrollments"},
		{"POST", "/v1/enrollments/abc123/revoke"},
		{"GET", "/v1/config/oidc"},
		{"PUT", "/v1/config/oidc"},
		{"GET", "/v1/scopes"},
		{"GET", "/v1/decisions"},
		{"GET", "/v1/decisions/42"},
		{"POST", "/v1/decisions"},
		{"POST", "/v1/decisions/42/supersede"},
		{"GET", "/v1/audit/actions"},
		{"GET", "/v1/enrollments/"}, // trailing slash tolerated
	}
	for _, c := range ok {
		if !consoleAdminAllows(c[0], c[1]) {
			t.Errorf("expected allow: %s %s", c[0], c[1])
		}
	}
}

func TestConsoleAdminAllows_Denied(t *testing.T) {
	deny := [][2]string{
		{"DELETE", "/v1/enrollments/abc/revoke"}, // wrong method
		{"POST", "/v1/console/overview"},         // wrong method
		{"get", "/v1/enrollments"},               // case-sensitive method
		{"GET", "/v1/enroll"},                    // only POST /v1/enroll is allowed
		{"GET", "/v1/review"},                    // curator scope, not console-admin
		{"POST", "/v1/review/7/accept"},
		{"GET", "/v1/ingest/status"},
		{"POST", "/v1/enrollments/abc/revoke/extra"}, // extra segment
		{"GET", "/v1/enrollments/abc"},               // missing action
		{"POST", "/v1/enrollments//revoke"},          // empty id segment
		{"GET", "/v1/%65nrollments"},                 // encoded literal
		{"GET", "/v1/enrollments?all=1"},             // stray query
		{"GET", "v1/enrollments"},                    // missing leading slash
		{"GET", ""},
		{"", "/v1/enrollments"},
	}
	for _, c := range deny {
		if consoleAdminAllows(c[0], c[1]) {
			t.Errorf("expected deny: %q %q", c[0], c[1])
		}
	}
}
