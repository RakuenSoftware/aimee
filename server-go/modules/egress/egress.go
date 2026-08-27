// Package egress owns authorization and transport for module-initiated network calls.
package egress

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"net"
	"net/url"
	"sort"
	"strings"
	"time"

	"github.com/JBailes/aimee/server-go/bus"
)

const (
	PrincipalRef       uint32 = 32
	EventAuthorize     uint32 = 12289 // 4096 + PrincipalRef*256 + StageAuthorize
	EventHTTP          uint32 = 12290
	EventSSEOpen       uint32 = 12291
	EventSSESend       uint32 = 12292
	EventSSERecv       uint32 = 12293
	EventSSEClose      uint32 = 12294
	EventCredentialKey uint32 = 12295
	StageAuthorize     uint32 = 1
	StageHTTP          uint32 = 2
	StageSSEOpen       uint32 = 3
	StageSSESend       uint32 = 4
	StageSSERecv       uint32 = 5
	StageSSEClose      uint32 = 6
	StageCredentialKey uint32 = 7
	PolicyRevision            = "module-egress-v1"

	MemoryClientRef     uint32 = 70
	GitClientRef        uint32 = 71
	RoundtableClientRef uint32 = 72
	PluginClientOffset  uint32 = 512
)

const decisionTimeout = 5 * time.Second

type Request struct {
	TargetURL         string `json:"target_url"`
	Purpose           string `json:"purpose"`
	Method            string `json:"method"`
	RequestSHA256     string `json:"request_sha256"`
	CredentialPresent bool   `json:"credential_present,omitempty"`
}

type Decision struct {
	Allowed        bool     `json:"allowed"`
	Reason         string   `json:"reason"`
	PolicyRevision string   `json:"policy_revision"`
	Target         string   `json:"target,omitempty"`
	ResolvedIPs    []string `json:"resolved_ips,omitempty"`
}

type Authorizer interface {
	Authorize(context.Context, uint64, Request) (Decision, error)
}

// Executor is the only production HTTP transport exposed to module callers.
// Implementations reached by modules are bus clients; the network-owning
// implementation lives in this package's separately registered process.
type Executor interface {
	Do(context.Context, uint64, HTTPRequest) (HTTPResponse, error)
}

type Client interface {
	Executor
	Streamer
}

type AuthorizeFunc func(context.Context, uint64, Request) (Decision, error)

func (f AuthorizeFunc) Authorize(ctx context.Context, traceID uint64, request Request) (Decision, error) {
	return f(ctx, traceID, request)
}

type moduleCaller interface {
	Call(context.Context, uint32, uint32, uint64, time.Duration, []byte) ([]byte, error)
}

type BusAuthorizer struct{ caller moduleCaller }

func NewBusAuthorizer(caller moduleCaller) (*BusAuthorizer, error) {
	if caller == nil {
		return nil, errors.New("egress: no module caller")
	}
	return &BusAuthorizer{caller: caller}, nil
}

func (a *BusAuthorizer) Authorize(ctx context.Context, traceID uint64, request Request) (Decision, error) {
	if a == nil || a.caller == nil {
		return Decision{}, errors.New("egress: authorization service is not configured")
	}
	body, err := json.Marshal(request)
	if err != nil {
		return Decision{}, err
	}
	reply, err := a.caller.Call(ctx, EventAuthorize, StageAuthorize, traceID, decisionTimeout, body)
	if err != nil {
		return Decision{}, fmt.Errorf("egress authorization: %w", err)
	}
	var decision Decision
	if err := json.Unmarshal(reply, &decision); err != nil {
		return Decision{}, fmt.Errorf("egress authorization response: %w", err)
	}
	if decision.PolicyRevision != PolicyRevision {
		return Decision{}, errors.New("egress authorization returned an unknown policy revision")
	}
	if !decision.Allowed {
		return decision, fmt.Errorf("egress denied: %s", decision.Reason)
	}
	return decision, nil
}

type resolver interface {
	LookupIPAddr(context.Context, string) ([]net.IPAddr, error)
}

type policy struct {
	resolver        resolver
	credentials     *credentialBroker
	allowPrivateMCP bool // tests only; production constructors leave this false
}

func NewHandler() bus.ModuleHandler {
	return newHandler(net.DefaultResolver)
}

func newHandler(r resolver) bus.ModuleHandler {
	broker, err := newCredentialBroker()
	if err != nil {
		return func(bus.ModuleInvocation, []byte) ([]byte, bus.ModuleStatus) {
			return nil, bus.ModuleStatusInternal
		}
	}
	p := policy{resolver: r, credentials: broker}
	s := newStreamService(p, newVaultCredentialResolver())
	return func(invocation bus.ModuleInvocation, body []byte) ([]byte, bus.ModuleStatus) {
		if invocation.PrincipalClass != 1 {
			return nil, bus.ModuleStatusInvalidRequest
		}
		if invocation.StageID == StageHTTP {
			return p.handleHTTP(invocation, body)
		}
		if invocation.StageID >= StageSSEOpen && invocation.StageID <= StageSSEClose {
			return s.handle(invocation, body)
		}
		if invocation.StageID == StageCredentialKey {
			if len(body) != 0 || invocation.Cancelled() {
				return nil, bus.ModuleStatusInvalidRequest
			}
			return broker.publicReply()
		}
		if invocation.StageID != StageAuthorize {
			return nil, bus.ModuleStatusInvalidRequest
		}
		var request Request
		if json.Unmarshal(body, &request) != nil {
			return nil, bus.ModuleStatusInvalidRequest
		}
		decision := p.decide(invocation, request)
		encoded, err := json.Marshal(decision)
		if err != nil || uint32(len(encoded)) > bus.ModuleMessageMaxBody {
			return nil, bus.ModuleStatusInternal
		}
		return encoded, bus.ModuleStatusOK
	}
}

func (p policy) decide(invocation bus.ModuleInvocation, request Request) Decision {
	deny := func(reason string) Decision {
		return Decision{Reason: reason, PolicyRevision: PolicyRevision}
	}
	if invocation.Cancelled() {
		return deny("cancelled")
	}
	if request.Method == "" || request.Purpose == "" || !validDigest(request.RequestSHA256) {
		return deny("incomplete request metadata")
	}
	parsed, err := url.Parse(request.TargetURL)
	if err != nil || parsed.User != nil || parsed.Hostname() == "" ||
		(parsed.Scheme != "http" && parsed.Scheme != "https") {
		return deny("invalid http(s) target")
	}
	if !callerPurposeAllowed(invocation.PrincipalRef, request, parsed) {
		return deny("caller is not allowed for this purpose or destination")
	}
	ctx, cancel := context.WithTimeout(context.Background(), invocation.Remaining(decisionTimeout))
	defer cancel()
	addresses, err := p.resolver.LookupIPAddr(ctx, parsed.Hostname())
	if err != nil || len(addresses) == 0 {
		return deny("target did not resolve")
	}
	ips := make([]string, 0, len(addresses))
	for _, address := range addresses {
		if address.IP == nil {
			continue
		}
		if request.Purpose != "embedding" && !(request.Purpose == "mcp_sse" && p.allowPrivateMCP) &&
			!publicIP(address.IP) {
			return deny("target resolved to a non-public address")
		}
		ips = append(ips, address.IP.String())
	}
	if len(ips) == 0 {
		return deny("target resolved to no usable address")
	}
	sort.Strings(ips)
	parsed.Fragment = ""
	return Decision{Allowed: true, Reason: "policy_allow", PolicyRevision: PolicyRevision,
		Target: parsed.String(), ResolvedIPs: ips}
}

func callerPurposeAllowed(ref uint32, request Request, target *url.URL) bool {
	host := strings.ToLower(target.Hostname())
	switch ref {
	case MemoryClientRef:
		return request.Purpose == "embedding" && request.Method == "POST" &&
			strings.HasSuffix(target.EscapedPath(), "/embed")
	case GitClientRef:
		return request.Purpose == "forge" && target.Scheme == "https" && host == "api.github.com" &&
			forgeTargetAllowed(request.Method, target.EscapedPath())
	case RoundtableClientRef:
		return request.Purpose == "review_artifact" && request.Method == "GET" && target.Scheme == "https" &&
			(host == "github.com" || host == "patch-diff.githubusercontent.com")
	default:
		// Dynamically provisioned MCP egress identities are derived from the
		// instance identity, so they remain unique without a second allocator.
		return ref >= 200+PluginClientOffset && ref < 456+PluginClientOffset &&
			request.Purpose == "mcp_sse" && request.Method == "GET"
	}
}

func forgeTargetAllowed(method, path string) bool {
	parts := strings.Split(strings.Trim(path, "/"), "/")
	if len(parts) < 3 || parts[0] != "repos" || parts[1] == "" || parts[2] == "" {
		return false
	}
	if len(parts) == 3 {
		return method == "GET"
	}
	if parts[3] != "pulls" {
		return false
	}
	if len(parts) == 4 {
		return method == "GET" || method == "POST"
	}
	if len(parts) == 5 {
		return method == "GET" || method == "PATCH"
	}
	return len(parts) == 6 && parts[5] == "merge" && method == "PUT"
}

func validDigest(value string) bool {
	if len(value) != 64 {
		return false
	}
	for _, char := range value {
		if !(char >= '0' && char <= '9' || char >= 'a' && char <= 'f') {
			return false
		}
	}
	return true
}

func publicIP(ip net.IP) bool {
	return !ip.IsLoopback() && !ip.IsPrivate() && !ip.IsUnspecified() &&
		!ip.IsLinkLocalMulticast() && !ip.IsLinkLocalUnicast() && !ip.IsMulticast()
}
