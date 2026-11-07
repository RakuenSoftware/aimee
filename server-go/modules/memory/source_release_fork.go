package memory

import (
	"encoding/json"
	"strings"
	"time"

	"github.com/RakuenSoftware/ouroboros-ring/go/ring"
)

// Forks transfer retained source provenance to one distinct host request. A
// pending fork is deliberately absent from entries: its opaque challenge can
// never authorize a model/tool/provider release before fresh owner validation.
// Parent handles and their admitted/guarded state are independently owned.
type sourceReleaseFork struct {
	parent, oldBinding, newRequest, binding string
	entry                                   *sourceReleaseEntry
}

func sourceReleaseBytes(entry *sourceReleaseEntry) int {
	return len(entry.sources) + len(entry.executionGrades) + len(entry.assemblyMetadata)
}
func (s *sourceReleaseState) dropFork(ticket string) {
	if fork := s.forks[ticket]; fork != nil {
		s.bytes -= sourceReleaseBytes(fork.entry)
		s.healthBytes -= len(fork.entry.healthMetadata)
		delete(s.forks, ticket)
	}
}
func (s *sourceReleaseState) expireForks(now time.Time) {
	for token, fork := range s.forks {
		if !now.Before(fork.entry.expires) {
			s.dropFork(token)
		}
	}
	for binding, expires := range s.forkBindings {
		if !now.Before(expires) {
			delete(s.forkBindings, binding)
		}
	}
}
func forkPolicyCurrent(entry *sourceReleaseEntry) bool {
	for _, part := range entry.assemblyParts {
		if part.SelectionPolicy != "" && (part.SelectionPolicy != typedSelectionPolicyVersion || part.SelectionPolicyDigest != rankingArtifactDigest(true)) {
			return false
		}
	}
	return true
}
func sourceForkArgs(args commandArgs, request, ticket, operation string) commandArgs {
	next := make(commandArgs, len(args))
	for k, v := range args {
		next[k] = append(json.RawMessage(nil), v...)
	}
	next["request_id"], _ = json.Marshal(request)
	next["source_release_ticket"], _ = json.Marshal(ticket)
	next["operation"], _ = json.Marshal(operation)
	return next
}
func forkResponse(encoded []byte, status ring.ModuleStatus, fields map[string]any) ([]byte, ring.ModuleStatus) {
	if status != ring.ModuleStatusOK {
		return encoded, status
	}
	body, err := ring.DecodeCommandResult(encoded)
	var result map[string]any
	if err != nil || json.Unmarshal(body, &result) != nil {
		return nil, ring.ModuleStatusInternal
	}
	if result["status"] != "ok" {
		return encoded, status
	}
	for key, value := range fields {
		result[key] = value
	}
	return commandResult(result)
}
func handleSourceReleaseFork(s *sourceReleaseState, args commandArgs) ([]byte, ring.ModuleStatus) {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.expire(time.Now())
	if _, present := args["send_guard"]; present {
		return commandResult(commandError("invalid_argument", "source release fork does not acquire send guards"))
	}
	parentToken := args.stringOr("source_release_ticket", "")
	parent := s.entries[parentToken]
	oldID, ok := args.stringValue("request_id")
	principal, pok := args.stringValue("principal")
	_, cok := args.stringValue("caller_subject")
	newID, nok := args.stringValue("new_request_id")
	if !ok || !pok || !cok || !nok || oldID == "" || principal == "" || len(oldID) > 256 || newID == "" || len(newID) > 256 || newID == oldID || strings.ContainsAny(newID, "\x00\r\n") || parent == nil || parent.binding != releaseBinding(args) || !forkPolicyCurrent(parent) {
		return commandResult(commandError("unavailable", "source release fork owner unavailable"))
	}
	newArgs := sourceForkArgs(args, newID, "", "")
	binding := releaseBinding(newArgs)
	if args.stringOr("operation", "") == "source-release-fork-plan" {
		if _, used := s.forkBindings[binding]; used {
			return commandResult(commandError("unavailable", "source release fork request already used"))
		}
		for _, entry := range s.entries {
			if entry.binding == binding {
				return commandResult(commandError("unavailable", "source release fork request already used"))
			}
		}
		bytes := sourceReleaseBytes(parent)
		if len(s.entries)+len(s.forks) >= sourceReleaseMaxEntries || len(s.forkBindings) >= sourceReleaseMaxEntries || s.bytes+bytes > sourceReleaseMaxBytes || s.healthBytes+len(parent.healthMetadata) > sourceReleaseHealthMaxBytes {
			return commandResult(commandError("unavailable", "source release fork capacity"))
		}
		token, err := releaseToken()
		if err != nil {
			return commandResult(commandError("unavailable", "source release fork identity unavailable"))
		}
		candidate := &sourceReleaseEntry{sources: append(json.RawMessage(nil), parent.sources...), executionGrades: append(json.RawMessage(nil), parent.executionGrades...), assemblyParts: append([]sourceReleasePart(nil), parent.assemblyParts...), assemblyDigest: parent.assemblyDigest, assemblyMetadata: append(json.RawMessage(nil), parent.assemblyMetadata...), healthMetadata: append(json.RawMessage(nil), parent.healthMetadata...), workspace: parent.workspace, project: parent.project, binding: binding, digest: parent.digest, expires: parent.expires}
		if s.forks == nil {
			s.forks = map[string]*sourceReleaseFork{}
		}
		if s.forkBindings == nil {
			s.forkBindings = map[string]time.Time{}
		}
		s.forks[token] = &sourceReleaseFork{parent: parentToken, oldBinding: parent.binding, newRequest: newID, binding: binding, entry: candidate}
		s.forkBindings[binding] = parent.expires
		s.bytes += bytes
		s.healthBytes += len(candidate.healthMetadata)
		encoded, status := s.handleSourceReleaseLocked(sourceForkArgs(args, newID, token, "source-release-plan"), candidate)
		body, decodeErr := ring.DecodeCommandResult(encoded)
		var result map[string]any
		if decodeErr != nil || json.Unmarshal(body, &result) != nil || status != ring.ModuleStatusOK || result["status"] != "ok" {
			s.dropFork(token)
			return encoded, status
		}
		return forkResponse(encoded, status, map[string]any{"fork_ticket": token, "new_request_id": newID})
	}
	token := args.stringOr("fork_ticket", "")
	fork := s.forks[token]
	if args.stringOr("operation", "") != "source-release-fork-result" || fork == nil || fork.parent != parentToken || fork.oldBinding != parent.binding || fork.newRequest != newID || fork.binding != binding || !forkPolicyCurrent(fork.entry) {
		return commandResult(commandError("unavailable", "source release fork challenge unavailable"))
	}
	encoded, status := s.handleSourceReleaseLocked(sourceForkArgs(args, newID, token, "source-release-result"), fork.entry)
	body, err := ring.DecodeCommandResult(encoded)
	var result map[string]any
	if err != nil || json.Unmarshal(body, &result) != nil || status != ring.ModuleStatusOK || result["status"] != "ok" || result["admitted"] != true {
		s.dropFork(token)
		return encoded, status
	}
	// A fork establishes identity and fresh source visibility, not a send guard or
	// reusable dispatch permit. The normal guarded provider fence runs again.
	fork.entry.admitted = ""
	fork.entry.guardedAdmission = ""
	fork.entry.guardedAdmissionAt = time.Time{}
	if s.entries == nil {
		s.entries = map[string]*sourceReleaseEntry{}
	}
	s.entries[token] = fork.entry
	delete(s.forks, token)
	return forkResponse(encoded, status, map[string]any{"source_release_ticket": token, "new_request_id": newID})
}
