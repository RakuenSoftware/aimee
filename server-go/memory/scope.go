package memory

import "strings"

type Scope struct {
	Type  string `json:"type"`
	Value string `json:"value,omitempty"`
}

const (
	ScopeUser      = "user"
	ScopeGlobal    = "global"
	ScopeWorkspace = "workspace"
	ScopeProject   = "project"
)

// Canonical normalizes an explicit scope. It does not grant access or choose a
// deployment's default; placement and caller admission remain host decisions.
func (s Scope) Canonical() (Scope, error) {
	s.Type = strings.ToLower(strings.TrimSpace(s.Type))
	s.Value = strings.TrimSpace(s.Value)
	switch s.Type {
	case ScopeUser:
		if s.Value != "" && s.Value != "_user" {
			return Scope{}, ErrClientRequest
		}
		s.Value = "_user"
	case ScopeGlobal:
		if s.Value != "" && s.Value != "_global" {
			return Scope{}, ErrClientRequest
		}
		s.Value = "_global"
	case ScopeWorkspace, ScopeProject:
		if s.Value == "" {
			return Scope{}, ErrClientRequest
		}
	default:
		return Scope{}, ErrClientRequest
	}
	if len(s.Value) > 1024 {
		return Scope{}, ErrClientRequest
	}
	return s, nil
}
