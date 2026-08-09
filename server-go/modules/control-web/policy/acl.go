// Package policy contains proxy authorization decisions shared by the physical
// control-web provider and its isolated module process.
package policy

import "strings"

// entry is one exact HTTP-method and route-pattern authorization rule.
type entry struct {
	Method  string
	Pattern string
}

var consoleAdminACL = []entry{
	{"GET", "/v1/console/overview"},
	{"GET", "/v1/console/typed_facts"},
	{"POST", "/v1/console/typed_facts/config"},
	{"POST", "/v1/console/typed_facts/relation"},
	{"GET", "/v1/console/pipeline"},
	{"GET", "/v1/console/settings"},
	{"POST", "/v1/console/settings/config"},
	{"POST", "/v1/console/pipeline/config"},
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

var fleetACL = []entry{
	{"GET", "/v1/servers"},
	{"GET", "/v1/servers/{id}/health"},
	{"GET", "/v1/servers/{id}/agents"},
	{"GET", "/v1/servers/{id}/config"},
	{"POST", "/v1/servers/{id}/actions"},
}

func segmentMatches(pattern, segment string) bool {
	if pattern == "{id}" {
		return segment != ""
	}
	return pattern == segment
}

func pathMatches(pattern, path string) bool {
	if pattern == "" || path == "" || path[0] != '/' {
		return false
	}
	if len(path) > 1 && strings.HasSuffix(path, "/") {
		path = path[:len(path)-1]
	}
	patterns := strings.Split(pattern[1:], "/")
	segments := strings.Split(path[1:], "/")
	if len(patterns) != len(segments) {
		return false
	}
	for index := range patterns {
		if !segmentMatches(patterns[index], segments[index]) {
			return false
		}
	}
	return true
}

func allows(entries []entry, method, path string) bool {
	if method == "" || path == "" || path[0] != '/' || len(path) >= 512 {
		return false
	}
	for _, entry := range entries {
		if entry.Method == method && pathMatches(entry.Pattern, path) {
			return true
		}
	}
	return false
}

// ConsoleAdminAllows reports whether control-web may proxy the request with its
// scoped console-admin credential. One trailing slash is normalized before the
// decision is returned over the event bus.
func ConsoleAdminAllows(method, path string) bool {
	return allows(consoleAdminACL, method, path)
}

// FleetAllows reports whether the request belongs to the OIDC fleet route
// family. Fleet routes deliberately reject trailing slashes.
func FleetAllows(method, path string) bool {
	if len(path) > 1 && strings.HasSuffix(path, "/") {
		return false
	}
	return allows(fleetACL, method, path)
}
