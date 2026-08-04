// Package policy contains the isolated runtime-web process's RPC-fault decision.
package policy

import "net/http"

// HTTPStatusForRPCFault maps a server-classified RPC fault onto the status that
// runtime-web exposes to its browser caller. Unknown and unclassified failures
// stay upstream failures rather than being guessed into a caller fault.
func HTTPStatusForRPCFault(kind string) int {
	switch kind {
	case "invalid_argument":
		return http.StatusBadRequest
	case "not_found":
		return http.StatusNotFound
	case "permission_denied":
		return http.StatusForbidden
	case "unavailable":
		return http.StatusServiceUnavailable
	default:
		return http.StatusBadGateway
	}
}
