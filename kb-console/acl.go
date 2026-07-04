package main

import "strings"

// routeACL is the Go-side mirror of the kb's console-admin route allowlist
// (src/kb/http/kb_route_acl.c). It is defence-in-depth: the kb enforces the same
// allowlist server-side, but the console refuses to proxy anything outside it so a
// bug or a crafted request never even reaches the kb with the console-admin cred.
//
// Each entry is an HTTP method plus a route pattern. A "{id}" segment matches
// exactly one non-empty, slash-free segment; all other segments match literally,
// and segment counts must be equal — so encoded paths, extra/trailing segments,
// sibling paths, and wrong methods are all rejected.
type aclEntry struct {
	method  string
	pattern string
}

var consoleAdminACL = []aclEntry{
	{"GET", "/v1/console/overview"},
	{"POST", "/v1/enroll"},
	{"GET", "/v1/enrollments"},
	{"POST", "/v1/enrollments/{id}/revoke"},
	{"GET", "/v1/config/oidc"},
	{"PUT", "/v1/config/oidc"},
	{"GET", "/v1/scopes"},
	{"GET", "/v1/decisions"},
	{"GET", "/v1/decisions/{id}"},
	{"POST", "/v1/decisions"},
	{"POST", "/v1/decisions/{id}/supersede"},
	{"POST", "/v1/decisions/{id}/outcome"},
	{"POST", "/v1/decisions/{id}/status"},
	{"POST", "/v1/decisions/{id}/revisit"},
	{"GET", "/v1/audit/actions"},
}

// segMatches matches one path segment against one pattern segment.
func segMatches(pat, seg string) bool {
	if pat == "{id}" {
		return seg != "" // segments never contain '/'
	}
	return pat == seg
}

// pathMatches reports whether a route pattern matches a path, segment by segment.
// Both must be absolute (leading '/'); a single trailing slash on path is tolerated.
func pathMatches(pattern, path string) bool {
	if pattern == "" || path == "" || path[0] != '/' {
		return false
	}
	if len(path) > 1 && strings.HasSuffix(path, "/") {
		path = path[:len(path)-1]
	}
	ps := strings.Split(pattern[1:], "/")
	qs := strings.Split(path[1:], "/")
	if len(ps) != len(qs) {
		return false
	}
	for i := range ps {
		if !segMatches(ps[i], qs[i]) {
			return false
		}
	}
	return true
}

// consoleAdminAllows reports whether the console may proxy (method, path) with the
// console-admin credential. path must already have its query string stripped.
func consoleAdminAllows(method, path string) bool {
	if method == "" || path == "" || path[0] != '/' {
		return false
	}
	if len(path) >= 512 { // parity with kb_route_acl.c: overlong → deny, never truncate
		return false
	}
	for _, e := range consoleAdminACL {
		if e.method == method && pathMatches(e.pattern, path) {
			return true
		}
	}
	return false
}
