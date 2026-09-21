package memory

import (
	"bytes"
	"crypto/rand"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"sync"
	"time"

	"github.com/JBailes/aimee/server-go/bus"
)

const sourceReleaseTTL = 15 * time.Minute
const sourceReleaseMaxBytes = 16 << 20
const sourceReleaseMaxEntries = 1024

// Opaque process-local handles keep memory policy and source arrays out of the
// C host. Losing the process or expiring an entry refuses release. These are
// neither durable prepared receipts nor acknowledgements of provider dispatch.
type sourceReleaseState struct {
	mu      sync.Mutex
	entries map[string]*sourceReleaseEntry
	bytes   int
}
type sourceReleaseEntry struct {
	sources                                      json.RawMessage
	workspace, project, binding, digest, pending string
	previous                                     string
	expires                                      time.Time
}

func releaseDigest(value any) string {
	raw, _ := json.Marshal(value)
	digest := sha256.Sum256(raw)
	return hex.EncodeToString(digest[:])
}
func releaseToken() (string, error) {
	var token [16]byte
	if _, err := rand.Read(token[:]); err != nil {
		return "", err
	}
	return hex.EncodeToString(token[:]), nil
}
func releaseTokenValid(token string) bool {
	if len(token) != 32 {
		return false
	}
	for _, c := range token {
		if !(c >= '0' && c <= '9' || c >= 'a' && c <= 'f') {
			return false
		}
	}
	return true
}
func releaseBinding(args commandArgs) string {
	return releaseDigest([]string{args.stringOr("request_id", ""), args.stringOr("principal", ""), args.stringOr("caller_subject", "")})
}
func (s *sourceReleaseState) expire(now time.Time) {
	for token, e := range s.entries {
		if !now.Before(e.expires) {
			delete(s.entries, token)
			s.bytes -= len(e.sources)
		}
	}
}

func (s *sourceReleaseState) prepare(args commandArgs, assembly map[string]any) (string, error) {
	refs := []typedProjectionRef{}
	for _, name := range []string{"facts_projection", "typed_projection", "memory_projection"} {
		if projection, ok := assembly[name].(map[string]any); ok {
			for _, ref := range projection["retained_items"].([]typedProjectionRef) {
				// Unversioned channels remain explicitly outside this source check.
				if ref.Source != nil {
					refs = append(refs, ref)
				}
			}
		}
	}
	previous := args.stringOr("source_release_ticket", "")
	if len(refs) == 0 {
		return previous, nil
	}
	token, err := releaseToken()
	if err != nil {
		return "", err
	}
	workspace, project := args.stringOr("workspace", ""), args.stringOr("project", "")
	if len(workspace) > 1024 || len(project) > 1024 {
		return "", errors.New("scope too large")
	}
	binding := releaseBinding(args)
	s.mu.Lock()
	defer s.mu.Unlock()
	now := time.Now()
	s.expire(now)
	var prior *sourceReleaseEntry
	if previous != "" {
		prior = s.entries[previous]
		if prior == nil || prior.binding != binding || prior.workspace != workspace || prior.project != project {
			return "", errors.New("previous source release unavailable")
		}
		var old []typedProjectionRef
		if json.Unmarshal(prior.sources, &old) != nil {
			return "", errors.New("invalid previous release")
		}
		refs = append(old, refs...)
	}
	unique := make([]typedProjectionRef, 0, len(refs))
	seen := map[string]bool{}
	for _, ref := range refs {
		key := releaseDigest(ref)
		if !seen[key] {
			seen[key] = true
			unique = append(unique, ref)
		}
	}
	request := sourceRevalidation{SchemaVersion: 1, CheckID: token, Sources: unique}
	if !request.valid() {
		return "", errors.New("unavailable source contract")
	}
	raw, err := json.Marshal(unique)
	if err != nil {
		return "", err
	}
	if prior != nil && bytes.Equal(raw, prior.sources) {
		return previous, nil
	}
	if len(s.entries) >= sourceReleaseMaxEntries || s.bytes+len(raw) > sourceReleaseMaxBytes || len(raw) > maxDataBody/2 {
		return "", errors.New("source release capacity")
	}
	if s.entries == nil {
		s.entries = map[string]*sourceReleaseEntry{}
	}
	// Assembly is not integrity acceptance. Keep the old handle immutable until
	// the host either discards this candidate or uses it at the provider fence.
	s.entries[token] = &sourceReleaseEntry{sources: raw, workspace: workspace, project: project, binding: binding, digest: releaseDigest(unique), previous: previous, expires: now.Add(sourceReleaseTTL)}
	s.bytes += len(raw)
	return token, nil
}

func (s *sourceReleaseState) drop(token, binding string, ancestors bool) {
	for token != "" {
		entry := s.entries[token]
		if entry == nil || entry.binding != binding {
			return
		}
		delete(s.entries, token)
		s.bytes -= len(entry.sources)
		if !ancestors {
			return
		}
		token = entry.previous
	}
}

func handleSourceRelease(s *sourceReleaseState, args commandArgs) ([]byte, bus.ModuleStatus) {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.expire(time.Now())
	ticket := args.stringOr("source_release_ticket", "")
	entry := s.entries[ticket]
	operation := args.stringOr("operation", "")
	if (operation == "source-release-finish" || operation == "source-release-discard") && (entry == nil || entry.binding == releaseBinding(args)) {
		s.drop(ticket, releaseBinding(args), operation == "source-release-finish")
		return commandResult(map[string]any{"status": "ok"})
	}
	if entry == nil || entry.binding != releaseBinding(args) {
		return commandResult(commandError("unavailable", "source release handle unavailable"))
	}
	if args.stringOr("operation", "") == "source-release-plan" {
		// Reaching the fence confirms host integrity acceptance. The cumulative
		// source set supersedes its earlier handles; a rejected candidate never does.
		s.drop(entry.previous, entry.binding, true)
		entry.previous = ""
		check, err := releaseToken()
		if err != nil {
			return commandResult(commandError("unavailable", "source check unavailable"))
		}
		entry.pending = check
		return commandResult(map[string]any{"status": "ok", "request": map[string]any{
			"scope_context": true, "include_all": false, "workspace": entry.workspace, "project": entry.project,
			"revalidation": map[string]any{"schema_version": 1, "check_id": check, "sources": entry.sources}}})
	}
	var reply struct {
		Status   string `json:"status"`
		Eligible bool   `json:"eligible"`
		CheckID  string `json:"check_id"`
		Digest   string `json:"sources_digest"`
	}
	check := entry.pending
	entry.pending = "" // one owner answer per attempt; every retry rechecks the owner
	if json.Unmarshal(args["owner_response"], &reply) != nil || check == "" || reply.Status != "ok" || reply.CheckID != check || reply.Digest != entry.digest {
		return commandResult(commandError("unavailable", "source owner answer unavailable"))
	}
	if !reply.Eligible {
		return commandResult(commandError("stale_context", "retained memory source changed or is no longer visible"))
	}
	return commandResult(map[string]any{"status": "ok", "admitted": true, "boundary": "source_revalidation"})
}
