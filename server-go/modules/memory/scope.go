package memory

import (
	"fmt"
	"strings"

	"github.com/JBailes/aimee/server-go/bus"
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

// The host uses this marker to restrict reads when no caller context exists.
// It is never a writable project or workspace.
const missingScopeValue = "__aimee_scope_missing__"

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

// bindVerifiedScope narrows a data request to the host-verified credential.
// Scope is a restriction even when a service credential has no human actor;
// it never confers user authority. Unscoped host callers retain their existing
// contract, and a named service identity retains deployment-wide data access.
func bindVerifiedScope(request *DataRequest, kind, id string) error {
	if kind == "" {
		return nil
	}
	if kind == "service" && id != "" {
		return nil
	}
	authorized, err := normalizeScope(PlacementKB, Scope{Type: kind, Value: id})
	if err != nil {
		return err
	}
	if request.Scope.Type != "" || request.Scope.Value != "" {
		target, err := normalizeScope(PlacementKB, request.Scope)
		if err != nil || target != authorized {
			return fmt.Errorf("memory: requested scope exceeds verified scope")
		}
	}
	if (request.Project != "" && (authorized.Type != ScopeProject || request.Project != authorized.Value)) ||
		(request.Workspace != "" && (authorized.Type != ScopeWorkspace || request.Workspace != authorized.Value)) {
		return fmt.Errorf("memory: requested audience exceeds verified scope")
	}
	request.Scope = authorized
	request.IncludeAll = false
	if authorized.Type == ScopeProject {
		request.Project = authorized.Value
	} else if authorized.Type == ScopeWorkspace {
		request.Workspace = authorized.Value
	}
	return nil
}

// EligibilityContext is constructed after credential narrowing, never decoded
// from model arguments. Purpose is the selected owner operation. The database
// transaction owns the request clock; temporal anchors cannot change authority.
// RevocationGenerations are the exact observed root/parent revisions when a
// retained selection is checked. Selection reads obtain them with their rows;
// there is deliberately no invented global generation for unrelated records.
type EligibilityContext struct {
	Principal, TransportIdentity, Authority string
	AuthorizedAudience, Scope               Scope
	Workspace, Project, Purpose, QueryMode  string
	ValidAt, BelievedAt, PolicyVersion      string
	IncludeAll                              bool
	RevocationGenerations                   []MemoryRecordVersion
}

func eligibilityContext(request DataRequest, scope Scope, caller *bus.CommandContext) EligibilityContext {
	c := EligibilityContext{Principal: "system:model-inference", TransportIdentity: "internal", Authority: "model",
		Scope: scope, Workspace: request.Workspace, Project: request.Project, Purpose: request.Operation,
		QueryMode: "current", PolicyVersion: currentEligibilityPolicy, IncludeAll: request.IncludeAll}
	if caller != nil && caller.Authenticated {
		c.Principal, c.TransportIdentity = caller.Principal, caller.TransportIdentity
		c.AuthorizedAudience = Scope{Type: caller.ScopeKind, Value: caller.ScopeID}
		if c.TransportIdentity == "" {
			c.TransportIdentity = c.Principal
		}
		if (request.Authority == AuthorityUser && caller.UserAuthority) || request.Operation == "restore" {
			c.Authority = "user"
		}
	}
	if request.ReadPolicy != nil {
		c.QueryMode, c.ValidAt, c.BelievedAt = request.ReadPolicy.Mode, request.ReadPolicy.ValidAt, request.ReadPolicy.BelievedAt
	} else if request.AsOf != "" {
		c.QueryMode, c.ValidAt = "inspection", request.AsOf
	} else if a := request.Assertions; a != nil {
		c.ValidAt, c.BelievedAt = a.ValidAt, a.BelievedAt
		if a.Historical || a.ValidAt != "" || a.BelievedAt != "" {
			c.QueryMode = "historical"
		}
	}
	if request.AtVersion != nil {
		c.QueryMode = "exact_version"
	}
	if r := request.Revalidation; r != nil {
		for _, ref := range r.Sources {
			if ref.Source != nil {
				c.RevocationGenerations = append(c.RevocationGenerations, ref.Source.Version)
				c.RevocationGenerations = append(c.RevocationGenerations, ref.Source.MemoryParents...)
			}
		}
	}
	return c
}
