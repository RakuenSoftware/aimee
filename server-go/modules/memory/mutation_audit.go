package memory

import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"fmt"
	"log"
	"time"

	"github.com/JBailes/aimee/server-go/bus"
	"github.com/JBailes/aimee/server-go/modules/audit"
)

// The wire contains no content, prose reason or raw memory identity. This is
// operation telemetry alongside the KB's transactional SQL WORM record, not a
// replacement for that durable write. Server composition hooks remain until
// their pre-dispatch refusal policy also migrates.
func mutationAudit(request DataRequest, response DataResponse, status bus.ModuleStatus) (audit.Action, bool) {
	a := audit.Action{Actor: "memory", ArgsHash: "v1-", TaskID: request.ID, Verdict: "fail"}
	success := status == bus.ModuleStatusOK
	switch request.Operation {
	case "store", "insert-epistemic", "upsert-workflow", "supersede":
		a.Tool = "memory.insert"
		if request.Operation == "supersede" {
			a.Tool = "memory.supersede"
		}
		success = success && len(response.Records) == 1 && response.Records[0].ID > 0 && (response.Code == nil || *response.Code == MutationOK)
		if success {
			r := response.Records[0]
			a.TaskID, a.Mode = r.ID, r.Tier
			if r.Kind != "" || r.Key != "" {
				digest := sha256.Sum256([]byte(r.Kind + "\x1f" + r.Key))
				a.Command = "mk:" + hex.EncodeToString(digest[:6])
			}
			if r.Confidence > 0 {
				a.Reason = fmt.Sprintf("conf=%.2f", r.Confidence)
			}
		}
	case "update-as":
		a.Tool = "memory.update"
		success = success && response.Code != nil && *response.Code == MutationOK && len(response.IDs) == 1 && response.IDs[0] > 0
		if success {
			a.TaskID = response.IDs[0]
		}
	case "update-content", "reject", "restore":
		a.Tool = map[string]string{"update-content": "memory.update", "reject": "memory.reject", "restore": "memory.restore"}[request.Operation]
		success = success && response.Updated
	case "delete", "delete-as":
		a.Tool = "memory.delete"
		if request.Operation == "delete-as" && request.Authority == AuthorityModel {
			a.Tool = "memory.retire"
		}
		success = success && response.Deleted
	default:
		return audit.Action{}, false
	}
	if success {
		a.Verdict = "ok"
	}
	if request.SessionID != "" {
		a.Actor = request.SessionID
	}
	return a, true
}

func publishMutationAudit(publish func(context.Context, audit.Action) error, request DataRequest, response DataResponse, status bus.ModuleStatus) {
	action, ok := mutationAudit(request, response, status)
	if !ok || publish == nil {
		return
	}
	// Commit may exhaust the request deadline. Give the off-path observation its
	// own small bound and keep a transport failure visible without changing the
	// result of a mutation that has already committed.
	ctx, cancel := context.WithTimeout(context.Background(), 250*time.Millisecond)
	defer cancel()
	if err := publish(ctx, action); err != nil {
		log.Printf("memory audit: action=%s id=%d delivery failed", action.Tool, action.TaskID)
	}
}
