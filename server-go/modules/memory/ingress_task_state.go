package memory

import (
	"encoding/json"
	"math/bits"
	"sync"

	"github.com/JBailes/aimee/server-go/bus"
)

type ingressTaskSession struct {
	session, project string
	tokens, usedAt   uint64
}

// One bounded task history per shared memory handler. The claim is atomic so
// concurrent related turns cannot both inject the first-task packet.
type ingressTaskState struct {
	mu       sync.Mutex
	sessions [64]ingressTaskSession
	clock    uint64
}

func ingressTaskTokens(query string) uint64 {
	alnum := func(c byte) bool {
		return c >= 'a' && c <= 'z' || c >= 'A' && c <= 'Z' || c >= '0' && c <= '9' || c == '_'
	}
	var tokens uint64
	for i := 0; i < len(query); {
		if !alnum(query[i]) {
			i++
			continue
		}
		h, n := uint64(1469598103934665603), 0
		for i < len(query) && alnum(query[i]) {
			c := query[i]
			if c >= 'A' && c <= 'Z' {
				c += 'a' - 'A'
			}
			h = (h ^ uint64(c)) * 1099511628211
			i++
			n++
		}
		if n >= 2 {
			tokens |= uint64(1) << (h & 63)
		}
	}
	if tokens == 0 {
		return 1
	}
	return tokens
}

func (s *ingressTaskState) claim(session, project, query string) bool {
	if session == "" || project == "" {
		return false
	}
	tokens := ingressTaskTokens(query)
	s.mu.Lock()
	defer s.mu.Unlock()
	slot, oldest := -1, 0
	for i, candidate := range s.sessions {
		if candidate.session == "" {
			if slot < 0 {
				slot = i
			}
			continue
		}
		if candidate.session == session && candidate.project == project {
			slot = i
			break
		}
		if candidate.usedAt < s.sessions[oldest].usedAt {
			oldest = i
		}
	}
	if slot < 0 {
		slot = oldest
	}
	previous := s.sessions[slot]
	common, total := bits.OnesCount64(previous.tokens&tokens), bits.OnesCount64(previous.tokens|tokens)
	fetch := previous.session != session || previous.project != project || total == 0 || common*3 < total
	s.clock++
	s.sessions[slot] = ingressTaskSession{session, project, tokens, s.clock}
	return fetch
}

func (s *ingressTaskState) rearm(session, project string) {
	s.mu.Lock()
	defer s.mu.Unlock()
	for i, candidate := range s.sessions {
		if candidate.session == session && candidate.project == project {
			s.sessions[i] = ingressTaskSession{}
			return
		}
	}
}

func handleIngressTaskState(state *ingressTaskState, args commandArgs) ([]byte, bus.ModuleStatus) {
	operation := args.stringOr("operation", "")
	if operation == "ingress-task-reset" {
		state.mu.Lock()
		state.sessions = [64]ingressTaskSession{}
		state.clock = 0
		state.mu.Unlock()
		return commandResult(map[string]any{"status": "ok"})
	}
	var session, project, query *string
	if json.Unmarshal(args["session"], &session) != nil || session == nil ||
		json.Unmarshal(args["project"], &project) != nil || project == nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	if operation == "ingress-task-rearm" {
		state.rearm(*session, *project)
		return commandResult(map[string]any{"status": "ok"})
	}
	if json.Unmarshal(args["query"], &query) != nil || query == nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	return commandResult(map[string]any{"status": "ok", "fetch": state.claim(*session, *project, *query)})
}
