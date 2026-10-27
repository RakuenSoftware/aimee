package memory

import (
	"encoding/json"
	"strings"
	"time"

	"github.com/RakuenSoftware/ouroboros-ring/go/ring"
)

func handleReembedCommand(options handlerOptions, invocation ring.ModuleInvocation, verb string, args commandArgs) ([]byte, ring.ModuleStatus) {
	request := DataRequest{IncludeAll: true, Version: strings.TrimSpace(args.stringOr("version", ""))}
	if caller := options.commandContext; caller != nil && caller.ScopeKind != "" && caller.ScopeKind != ScopeGlobal {
		return commandResult(commandError("forbidden", "versioned embedding maintenance requires a global caller scope"))
	}
	commandScope(args, &request)
	if !request.IncludeAll {
		return commandResult(commandError("invalid_argument", "versioned embedding maintenance requires all scopes"))
	}
	if len(request.Version) > 256 || ((verb == "reembed_start" || verb == "reembed_rollback") && request.Version == "") {
		return commandResult(commandError("invalid_argument", "embedder version is required and must fit 256 bytes"))
	}
	call := func(operation string, target any) ring.ModuleStatus {
		request.Operation = operation
		body, _ := json.Marshal(request)
		raw, status := handleData(options, invocation, body)
		if status != ring.ModuleStatusOK {
			return status
		}
		var response DataResponse
		if json.Unmarshal(raw, &response) != nil || json.Unmarshal(response.Payload, target) != nil {
			return ring.ModuleStatusInternal
		}
		return ring.ModuleStatusOK
	}
	switch verb {
	case "reembed_status":
		var status reembedStatus
		if code := call("reembed-status", &status); code != ring.ModuleStatusOK {
			return nil, code
		}
		result := map[string]any{"status": "ok", "active_version": status.ActiveVersion, "has_job": status.HasJob, "active_generation": status.ActiveGeneration}
		if status.Job != nil {
			result["job"] = status.Job
		}
		return commandResult(result)
	case "reembed_cutover", "reembed_rollback":
		if verb == "reembed_cutover" {
			request.Version = ""
		}
		var result map[string]any
		if code := call("reembed-cutover", &result); code != ring.ModuleStatusOK {
			return nil, code
		}
		return commandResult(result)
	case "reembed_start":
		backend, ok := options.data.(*postgresDataStore)
		if !ok {
			return nil, ring.ModuleStatusCapabilityAbsent
		}
		command, err := backend.embeddingCommand(args.stringOr("embedding_command", ""))
		if err != nil {
			return nil, ring.ModuleStatusInternal
		}
		request.Command = command
		var prepared map[string]any
		if code := call("reembed-prepare", &prepared); code != ring.ModuleStatusOK {
			return nil, code
		}
		var initial reembedStatus
		if code := call("reembed-status", &initial); code != ring.ModuleStatusOK {
			return nil, code
		}
		if initial.Job == nil {
			return nil, ring.ModuleStatusInternal
		}
		embedded, failed := 0, 0
		// Each point commits independently. A deadline or restart leaves successful
		// drafts reusable, and a later invocation retries failures and changed inputs.
		for _, recordType := range []string{"memory", "assertion"} {
			request.RecordType = recordType
			request.AfterID = 0
			for !invocation.Cancelled() && invocation.Remaining(10*time.Minute) > time.Second {
				request.Limit = 64
				var batch struct {
					IDs []int64 `json:"ids"`
				}
				if code := call("reembed-next", &batch); code != ring.ModuleStatusOK {
					return nil, code
				}
				if len(batch.IDs) == 0 {
					break
				}
				for _, point := range batch.IDs {
					if invocation.Cancelled() || invocation.Remaining(10*time.Minute) <= time.Second {
						break
					}
					request.ID = point
					var reply EmbedResponse
					if code := call("reembed-point", &reply); code != ring.ModuleStatusOK {
						return nil, code
					}
					request.AfterID = point
					if reply.Embedded {
						embedded++
					} else {
						failed++
					}
				}
			}
		}
		if invocation.Cancelled() {
			return nil, ring.ModuleStatusCancelled
		}
		var status reembedStatus
		if code := call("reembed-status", &status); code != ring.ModuleStatusOK {
			return nil, code
		}
		if status.Job == nil || status.Job.TargetVersion != request.Version {
			return nil, ring.ModuleStatusInternal
		}
		return commandResult(map[string]any{"status": "ok", "version": request.Version, "total": status.Job.Total, "done": status.Job.Done,
			"resume_last_id": initial.Job.LastID, "embedded": embedded, "failed": failed, "last_id": status.Job.LastID, "ready": status.Job.Ready, "more": !status.Job.Ready})
	}
	return nil, ring.ModuleStatusInvalidRequest
}
