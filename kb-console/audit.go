package main

import (
	"encoding/json"
	"log"
	"time"
)

// auditEvent records a console-admin action (login, break-glass, and — added
// alongside the backend routes in S2a/S4 — mint/revoke/decision writes).
type auditEvent struct {
	Time     string `json:"time"`
	Actor    string `json:"actor"`
	Iss      string `json:"iss,omitempty"`
	Action   string `json:"action"`
	Route    string `json:"route,omitempty"`
	SourceIP string `json:"source_ip,omitempty"`
}

// recordAudit writes an action to the console-local audit sink (a structured
// stderr line captured by the container's log driver). Break-glass logins are
// dual-sinked: this local record plus a kb-side audit row written by the auth
// endpoints added in S2a. S0 keeps the local record so break-glass use is never
// silent.
func recordAudit(ev auditEvent) {
	if ev.Time == "" {
		ev.Time = time.Now().UTC().Format(time.RFC3339)
	}
	if b, err := json.Marshal(ev); err == nil {
		log.Printf("audit %s", b)
	}
}
