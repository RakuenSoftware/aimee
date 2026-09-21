package memory

import (
	"fmt"
	"strings"
)

// Placement is the data-ownership role of this instance of the shared memory
// process.  It is deliberately not inferred from which tables happen to exist:
// doing so would turn a deployment mistake into a privacy-boundary mistake.
type Placement string

const (
	PlacementServer Placement = "server"
	PlacementKB     Placement = "kb"
)

func ParsePlacement(value string) (Placement, error) {
	switch Placement(strings.ToLower(strings.TrimSpace(value))) {
	case PlacementServer:
		return PlacementServer, nil
	case PlacementKB:
		return PlacementKB, nil
	default:
		return "", fmt.Errorf("memory: AIMEE_MODULE_PLACEMENT must be server or kb, got %q", value)
	}
}

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

// normalizeScope is the hard boundary between the two deployments of the same
// module.  The server instance can never address KB rows, and the KB instance
// can never address user rows.
func normalizeScope(placement Placement, scope Scope) (Scope, error) {
	scope.Type = strings.ToLower(strings.TrimSpace(scope.Type))
	scope.Value = strings.TrimSpace(scope.Value)

	switch placement {
	case PlacementServer:
		if scope.Type == "" {
			scope.Type = ScopeUser
		}
		if scope.Type != ScopeUser {
			return Scope{}, fmt.Errorf("memory: server placement only accepts user scope")
		}
		if scope.Value != "" && scope.Value != "_user" {
			return Scope{}, fmt.Errorf("memory: server user scope is instance-local")
		}
		scope.Value = "_user"
		return scope, nil

	case PlacementKB:
		if scope.Type == "" {
			scope.Type = ScopeGlobal
		}
		switch scope.Type {
		case ScopeGlobal:
			if scope.Value != "" && scope.Value != "_global" {
				return Scope{}, fmt.Errorf("memory: global scope cannot carry an arbitrary value")
			}
			scope.Value = "_global"
		case ScopeWorkspace, ScopeProject:
			if scope.Value == "" {
				return Scope{}, fmt.Errorf("memory: %s scope requires a value", scope.Type)
			}
		default:
			return Scope{}, fmt.Errorf("memory: kb placement accepts global, workspace, or project scope")
		}
		if len(scope.Value) > 1024 {
			return Scope{}, fmt.Errorf("memory: scope value is too long")
		}
		return scope, nil
	default:
		return Scope{}, fmt.Errorf("memory: invalid placement %q", placement)
	}
}
