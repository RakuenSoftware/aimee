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
	"sync"
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

	ProvidersClientRef  uint32 = 73
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
	var once sync.Once
	var initialized bus.ModuleHandler
	return func(invocation bus.ModuleInvocation, body []byte) ([]byte, bus.ModuleStatus) {
		once.Do(func() {
			broker, err := newCredentialBroker()
			if err != nil {
				return
			}
			p := policy{resolver: r, credentials: broker}
			s := newStreamService(p, newVaultCredentialResolver())
			initialized = func(invocation bus.ModuleInvocation, body []byte) ([]byte, bus.ModuleStatus) {
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
		})
		if initialized == nil {
			return nil, bus.ModuleStatusInternal
		}
		return initialized(invocation, body)
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
		if request.Purpose != "embedding" && request.Purpose != "provider" && !(request.Purpose == "mcp_sse" && p.allowPrivateMCP) &&
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
	case ProvidersClientRef:
		return request.Purpose == "provider" && (request.Method == "GET" || request.Method == "POST")
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

// egressBlockedIPv4 are the ranges a module-initiated call must not reach.
// Deliberately in step with server-go/modules/delegates/proxyguard.go and
// server-go/modules/sandbox/proxy_policy.go: the same destination must not be
// refused on one plane and dialed on another. The stdlib predicates alone are
// not enough -- IsPrivate covers RFC1918 and fc00::/7 but not CGNAT, the
// TEST-NETs, or the reserved space.
var egressBlockedIPv4 = func() []*net.IPNet {
	cidrs := []string{
		"0.0.0.0/8", "10.0.0.0/8", "100.64.0.0/10", "127.0.0.0/8", "169.254.0.0/16",
		"172.16.0.0/12", "192.0.0.0/24", "192.0.2.0/24", "192.88.99.0/24", "192.168.0.0/16",
		"198.18.0.0/15", "198.51.100.0/24", "203.0.113.0/24", "224.0.0.0/4", "240.0.0.0/4",
	}
	nets := make([]*net.IPNet, 0, len(cidrs))
	for _, cidr := range cidrs {
		if _, n, err := net.ParseCIDR(cidr); err == nil {
			nets = append(nets, n)
		}
	}
	return nets
}()

// egressEmbeddedIPv4 returns the IPv4 an IPv6 address translates or tunnels to.
// Covers v4-mapped, v4-compatible, NAT64 (both prefixes), 6to4 and Teredo: each
// spells an IPv4 destination as an IPv6 literal that matches no blocked v6
// prefix, so a guard that skips this step admits 169.254.169.254 by another name.
func egressEmbeddedIPv4(ip net.IP) net.IP {
	v6 := ip.To16()
	if v6 == nil || ip.To4() != nil {
		return nil
	}
	hasPrefix := func(prefix []byte) bool {
		for i, b := range prefix {
			if v6[i] != b {
				return false
			}
		}
		return true
	}
	switch {
	case hasPrefix([]byte{0x00, 0x64, 0xFF, 0x9B, 0, 0, 0, 0, 0, 0, 0, 0}),
		hasPrefix([]byte{0x00, 0x64, 0xFF, 0x9B, 0x00, 0x01, 0, 0, 0, 0, 0, 0}):
		return net.IPv4(v6[12], v6[13], v6[14], v6[15]) // NAT64
	case hasPrefix([]byte{0x20, 0x01, 0x00, 0x00}):
		return net.IPv4(^v6[12], ^v6[13], ^v6[14], ^v6[15]) // Teredo
	case v6[0] == 0x20 && v6[1] == 0x02:
		return net.IPv4(v6[2], v6[3], v6[4], v6[5]) // 6to4
	case hasPrefix(make([]byte, 12)) && (v6[12]|v6[13]|v6[14]|v6[15]) != 0:
		return net.IPv4(v6[12], v6[13], v6[14], v6[15]) // v4-compatible
	}
	return nil
}

func publicIP(ip net.IP) bool {
	if ip == nil {
		return false
	}
	if embedded := egressEmbeddedIPv4(ip); embedded != nil {
		ip = embedded
	}
	if v4 := ip.To4(); v4 != nil {
		for _, blocked := range egressBlockedIPv4 {
			if blocked.Contains(v4) {
				return false
			}
		}
		return true
	}
	return !ip.IsLoopback() && !ip.IsPrivate() && !ip.IsUnspecified() &&
		!ip.IsLinkLocalMulticast() && !ip.IsLinkLocalUnicast() && !ip.IsMulticast()
}
