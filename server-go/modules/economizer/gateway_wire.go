package economizer

// Buffered request-path ORCHESTRATION for gateway mutation.
//
// Ported from src/modules/economizer/gateway_mutate_wire.c.
//
// This sits above the pure decision helpers in gateway.go: it honours the
// per-session circuit breaker, snapshots then reduces then replaces the messages
// array, and after the upstream status handles the 4xx-restore-resend /
// 5xx-disable contract.
//
// Provider-agnostic: it operates on a container plus the message-array key
// within it, so restore and replace touch exactly the array the provider body is
// built from.
//
// The genuinely EXTERNAL pieces — the session-disable store, the stats sink, the
// resolved config, and the structural check — arrive as injected ports. The
// orchestration itself is economizer logic and lives here.

// SessionStore is the per-identity circuit breaker.
type SessionStore interface {
	// ResolveKey maps a request identity to a session key. ok=false means the
	// request is identity-less, which is a pristine passthrough with NO disable
	// state written — otherwise one anonymous request could disable another's
	// session.
	ResolveKey(sessionHeader, bearer, authIdentity string) (key string, ok bool)
	IsDisabled(key string) bool
	Disable(key string, ttlMS int, reason string)
}

// GatewayStats receives the counters an operator measures the lever by.
type GatewayStats interface {
	Inc(counter string)
	IncReason(counter, reason string)
	RecordTokenDelta(baseline, reduced int)
}

// Counter names, kept stable because dashboards key on them.
const (
	StatSessionDisabledBlocks = "session_disabled_blocks"
	StatMutateAttempted       = "mutate_attempted"
	StatMutateApplied         = "mutate_applied"
	Stat4xxRestoreResend      = "4xx_restore_resend"
	Stat5xxDisable            = "5xx_disable"
	StatStreamErrorDisable    = "stream_error_disable"
	StatHardBypass            = "hard_bypass"
)

// GatewayConfig is the resolved configuration for one request.
type GatewayConfig struct {
	// MutateOn is the aggressive-tier gate. Default OFF: the gateway stays dark
	// unless an operator turned the tier on.
	MutateOn bool
	// SessionDisableTTLMS is how long a tripped breaker stays tripped.
	SessionDisableTTLMS int
	FoldRetainedMsgs    int
}

// GatewayDeps are the injected collaborators.
type GatewayDeps struct {
	Sessions SessionStore
	Stats    GatewayStats
	// Repair is the structural check; see StructuralCheck in gateway.go for why
	// it is injected rather than ported.
	Repair StructuralCheck
}

// RequestIdentity is the raw identity material a session key is derived from.
type RequestIdentity struct {
	SessionHeader string
	Bearer        string
	AuthIdentity  string
}

// MutateCtx carries state from the pre-send attempt to post-send handling.
type MutateCtx struct {
	MutateOn bool // the feature flag was on for this request
	HaveKey  bool // a per-identity session key was resolvable
	Mutated  bool // the reduced payload was installed and dispatched
	Key      string
	// Pristine is the owned deep copy of the original array, nil once restored.
	Pristine *JSONValue
	State    ReduceState
	TTLMS    int
}

// PostAction is what the caller must do after the upstream status.
type PostAction int

const (
	PostNone PostAction = iota
	// PostResend means the request was restored to pristine and must be sent
	// once more.
	PostResend
)

func statsInc(d GatewayDeps, c string) {
	if d.Stats != nil {
		d.Stats.Inc(c)
	}
}

func statsIncReason(d GatewayDeps, c, reason string) {
	if d.Stats != nil {
		d.Stats.IncReason(c, reason)
	}
}

// BufferedMutate attempts the reduction for one buffered request.
//
// Every early return leaves the container PRISTINE. The ordering is the safety
// design: flag, then identity, then breaker, then snapshot, and only then
// reduce — so nothing is ever replaced before a restorable copy exists.
func BufferedMutate(container *JSONValue, key, model, systemPrompt string,
	ident RequestIdentity, cfg GatewayConfig, deps GatewayDeps, ctx *MutateCtx) {

	if ctx == nil {
		return
	}
	*ctx = MutateCtx{}
	if container == nil || key == "" {
		return
	}
	if !cfg.MutateOn {
		return // dark unless the economizer tier is aggressive
	}
	ctx.MutateOn = true
	ctx.TTLMS = cfg.SessionDisableTTLMS

	// An identity-less request is a pristine passthrough with NO disable state
	// written: attributing a failure to a session we cannot name would let one
	// caller trip another caller's breaker.
	if deps.Sessions == nil {
		return
	}
	sessionKey, ok := deps.Sessions.ResolveKey(ident.SessionHeader, ident.Bearer, ident.AuthIdentity)
	if !ok {
		return
	}
	ctx.Key = sessionKey
	ctx.HaveKey = true

	if deps.Sessions.IsDisabled(sessionKey) {
		statsInc(deps, StatSessionDisabledBlocks)
		return
	}

	msgs := container.Get(key)
	if !msgs.IsArray() {
		return
	}
	statsInc(deps, StatMutateAttempted)

	// SNAPSHOT FIRST: never send a reduced payload we cannot restore.
	ctx.Pristine = GWSnapshotMessages(msgs)
	if ctx.Pristine == nil {
		statsIncReason(deps, StatHardBypass, "snapshot_oom")
		return
	}

	rc := &ReduceConfig{
		GatewaySeam: true,
		// Compress-only at the gateway: the fold is deferred here because the
		// gateway has no per-conversation state to hold a freeze boundary.
		Compress:    true,
		MeasureOnly: false,
		Fold:        FoldConfig{RetainedMsgs: cfg.FoldRetainedMsgs},
	}
	res := Reduce(msgs, systemPrompt, SeamGateway, rc, nil)

	if bypass := GWShouldApply(true, &res, ReduceErrNone, deps.Repair); bypass != GWBypassNone {
		statsIncReason(deps, StatHardBypass, bypass.String())
		GWProvenanceClear(&ctx.State)
		return // pristine is kept; the container was never touched
	}

	if !GWReplaceMessages(container, key, res.Messages) {
		statsIncReason(deps, StatHardBypass, "replace_failed")
		GWProvenanceClear(&ctx.State)
		return
	}

	// Provenance is marked ONLY after the replace succeeds, so a failed install
	// never leaves the request looking reduced.
	GWProvenanceMarkReduced(&ctx.State)
	ctx.Mutated = true
	statsInc(deps, StatMutateApplied)
	if deps.Stats != nil {
		deps.Stats.RecordTokenDelta(res.BaselineTokens, res.ReducedTokens)
	}
}

// BufferedAfterStatus applies the post-dispatch contract.
//
// 4xx: the reduced payload may be why the provider refused, so restore the
// pristine original, repair defensively, trip the breaker and resend ONCE.
//
// 5xx: provider state is uncertain — the request may have been partially
// processed — so trip the breaker but do NOT resend. Resending after an
// ambiguous server error risks duplicating work the provider already did.
func BufferedAfterStatus(container *JSONValue, key string, httpStatus int,
	deps GatewayDeps, ctx *MutateCtx) PostAction {

	if ctx == nil || !ctx.Mutated || container == nil || key == "" {
		return PostNone
	}
	switch httpStatus / 100 {
	case 4:
		if ctx.Pristine != nil {
			if GWReplaceMessages(container, key, ctx.Pristine) {
				ctx.Pristine = nil // ownership moved back into the container
				if restored := container.Get(key); restored != nil && deps.Repair != nil {
					deps.Repair(restored)
				}
			}
		}
		if deps.Sessions != nil {
			deps.Sessions.Disable(ctx.Key, ctx.TTLMS, "4xx")
		}
		GWProvenanceClear(&ctx.State)
		statsInc(deps, Stat4xxRestoreResend)
		ctx.Mutated = false // the request is pristine now; no double handling
		return PostResend
	case 5:
		if deps.Sessions != nil {
			deps.Sessions.Disable(ctx.Key, ctx.TTLMS, "5xx")
		}
		GWProvenanceClear(&ctx.State)
		statsInc(deps, Stat5xxDisable)
		ctx.Mutated = false
		return PostNone
	}
	return PostNone
}

// StreamDisable trips the breaker for a streaming turn that failed after
// dispatch. One disable per turn: a later frame no-ops.
func StreamDisable(deps GatewayDeps, ctx *MutateCtx, reason string) {
	if ctx == nil || !ctx.Mutated || !ctx.HaveKey {
		return
	}
	if reason == "" {
		reason = "stream"
	}
	if deps.Sessions != nil {
		deps.Sessions.Disable(ctx.Key, ctx.TTLMS, reason)
	}
	GWProvenanceClear(&ctx.State)
	statsInc(deps, StatStreamErrorDisable)
	ctx.Mutated = false
}

// AnthropicStreamErrorIsInvalidRequest reports whether a streamed Anthropic
// error indicates the REQUEST was bad — i.e. possibly our reduction.
//
// EXACT type match, never substring, so a future error type that merely contains
// these words does not false-trip. rate_limit_error, overloaded_error, api_error
// and authentication_error are NOT reduction bugs and must not disable the lever.
func AnthropicStreamErrorIsInvalidRequest(data string) bool {
	if data == "" {
		return false
	}
	root := ParseJSON(data)
	if root == nil || root.Kind != JSONObject {
		return false
	}
	err := root.Get("error")
	if err == nil || err.Kind != JSONObject {
		return false
	}
	switch err.GetString("type") {
	case "invalid_request_error", "request_too_large":
		return true
	}
	return false
}

// StatusIsInvalidRequest reports whether an HTTP status is one a bad reduced
// serialization can produce.
//
// 400 invalid_request, 413 request_too_large, 422 unprocessable. 401/403/404/429
// are auth, not-found and rate-limit — a streaming path must not disable the
// lever on those, or ordinary throttling would switch the economizer off.
func StatusIsInvalidRequest(httpStatus int) bool {
	return httpStatus == 400 || httpStatus == 413 || httpStatus == 422
}
