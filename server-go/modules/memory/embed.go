package memory

import (
	"context"
	"encoding/json"
	"fmt"
	"net/url"
	"strings"
	"sync"
	"time"

	"github.com/JBailes/aimee/server-go/bus"
	"github.com/JBailes/aimee/server-go/modules/egress"
)

// Breaker defaults, matching DEPENDENCY_BREAKER_DEFAULT_* in C.
const (
	breakerThreshold = 3
	breakerBaseMS    = int64(1000)
	breakerMaxMS     = int64(30000)
	embedTimeout     = 20 * time.Second
)

// embedBreaker is a process-local circuit breaker for the embedder.
//
// It lives here because it is STATE, and state belongs to the module. It
// performs no retries: after `threshold` consecutive failures it suppresses
// calls for a bounded, jittered, exponential delay and then grants exactly ONE
// half-open probe. One, not "some": if every suppressed caller probed at once,
// a recovering embedder would be hit by the whole backlog at the moment it came
// back, which is the outage the breaker exists to prevent.
type embedBreaker struct {
	mu            sync.Mutex
	failureStreak uint32
	openCount     uint32
	probeInflight bool
	openedAtMS    int64
	retryAtMS     int64
	lastSuccessMS int64
	lastFailureMS int64
	suppressed    uint64
}

var breaker embedBreaker

// allow reports whether a call may proceed, and how long until the next chance
// when it may not.
func (b *embedBreaker) allow(nowMS int64) (bool, int64) {
	b.mu.Lock()
	defer b.mu.Unlock()
	if b.retryAtMS <= 0 {
		return true, 0
	}
	// A BACKWARD wall-clock jump must not extend a bounded outage forever: if
	// now precedes the moment the breaker opened, the clock moved under us and
	// the delay can never elapse on its own.
	eligible := nowMS >= b.retryAtMS || nowMS < b.openedAtMS
	if eligible && !b.probeInflight {
		b.probeInflight = true
		return true, 0
	}
	b.suppressed++
	if !eligible {
		return false, b.retryAtMS - nowMS
	}
	return false, 0
}

func (b *embedBreaker) reportSuccess(nowMS int64) {
	b.mu.Lock()
	defer b.mu.Unlock()
	b.failureStreak, b.openCount, b.probeInflight = 0, 0, false
	b.openedAtMS, b.retryAtMS = 0, 0
	b.lastSuccessMS = nowMS
}

// cancelProbe releases a claimed probe when a LOCAL error stopped the call from
// happening. Neither success nor failure may be inferred from that — counting it
// as either would let a local misconfiguration open the breaker against a
// dependency that was never contacted.
func (b *embedBreaker) cancelProbe() {
	b.mu.Lock()
	defer b.mu.Unlock()
	b.probeInflight = false
}

func breakerDelayMS(openCount uint32, nowMS, baseMS, maxMS int64) int64 {
	if baseMS < 1 {
		baseMS = 1
	}
	if maxMS < baseMS {
		maxMS = baseMS
	}
	delay := baseMS
	for i := uint32(0); i < openCount && delay < maxMS; i++ {
		if delay > maxMS/2 {
			delay = maxMS
		} else {
			delay *= 2
		}
	}
	jitterCap := delay / 4
	if jitterCap > maxMS-delay {
		jitterCap = maxMS - delay
	}
	if jitterCap > 0 {
		// Deterministic in (now, openCount) rather than random: a breaker whose
		// delay cannot be reproduced cannot be tested.
		mixed := uint64(nowMS) ^ (uint64(openCount+1) * 0x9e3779b97f4a7c15)
		delay += int64(mixed % uint64(jitterCap+1))
	}
	return delay
}

func (b *embedBreaker) reportFailure(nowMS int64) {
	b.mu.Lock()
	defer b.mu.Unlock()
	if b.failureStreak < ^uint32(0) {
		b.failureStreak++
	}
	b.lastFailureMS = nowMS
	b.probeInflight = false
	if b.failureStreak >= breakerThreshold {
		b.openedAtMS = nowMS
		b.retryAtMS = nowMS + breakerDelayMS(b.openCount, nowMS, breakerBaseMS, breakerMaxMS)
		if b.openCount < ^uint32(0) {
			b.openCount++
		}
	}
}

// EmbedRequest asks the module to embed one text.
//
// NowMS lets the caller supply the clock (the C side has an injectable clock for
// the same reason): a breaker tested against the wall clock can only be tested
// by sleeping.
type EmbedRequest struct {
	BaseURL   string `json:"base_url"`
	InputType string `json:"input_type"`
	Text      string `json:"text"`
	MaxDim    int    `json:"max_dim"`
	NowMS     int64  `json:"now_ms,omitempty"`
}

// EmbedResponse separates the ways this can decline, because they are different
// facts and a caller that conflates them misreports the embedder's health:
//
//	Unavailable  the breaker suppressed the call; nothing was sent
//	Unauthorized the service was REACHED and refused us (401/403)
//	Error        the call was attempted and failed
//
// Unauthorized is the subtle one: it proves reachability, so it must not count
// as a failure — and it closes an earlier outage, or a half-open breaker would
// turn the next authorization result back into "unavailable".
type EmbedResponse struct {
	Vector       []float32 `json:"vector,omitempty"`
	Dim          int       `json:"dim"`
	Truncated    bool      `json:"truncated,omitempty"`
	Unavailable  bool      `json:"unavailable,omitempty"`
	RetryAfterMS int64     `json:"retry_after_ms,omitempty"`
	Unauthorized bool      `json:"unauthorized,omitempty"`
	Error        string    `json:"error,omitempty"`
}

// EmbedIsHTTP reports whether a configured embedder command names an HTTP
// endpoint rather than a program to run.
func EmbedIsHTTP(command string) bool {
	return strings.HasPrefix(command, "http://") || strings.HasPrefix(command, "https://")
}

func nowOr(nowMS int64) int64 {
	if nowMS > 0 {
		return nowMS
	}
	return time.Now().UnixMilli()
}

// Embed performs one embedding, owning the breaker around it.
func Embed(ctx context.Context, traceID uint64, executor egress.Executor,
	request EmbedRequest) EmbedResponse {
	if ctx == nil {
		ctx = context.Background()
	}
	now := nowOr(request.NowMS)

	if !EmbedIsHTTP(request.BaseURL) {
		// A program-based embedder is still driven from C; declining here without
		// touching the breaker keeps that path's health accounting intact.
		return EmbedResponse{Error: "embed: base_url is not an http endpoint"}
	}
	if request.MaxDim <= 0 {
		return EmbedResponse{Error: "embed: max_dim must be positive"}
	}

	allowed, retryAfter := breaker.allow(now)
	if !allowed {
		return EmbedResponse{Unavailable: true, RetryAfterMS: retryAfter}
	}

	if request.Text == "" {
		// A local refusal, not a dependency verdict: release the probe rather
		// than blaming the embedder for it.
		breaker.cancelProbe()
		return EmbedResponse{Error: "embed: empty text"}
	}

	// The polarity rides in the query string because the body IS the raw text.
	endpoint := strings.TrimSuffix(request.BaseURL, "/") + "/embed"
	if request.InputType != "" {
		endpoint += "?input_type=" + url.QueryEscape(request.InputType)
	}
	const method = "POST"
	if executor == nil {
		breaker.cancelProbe()
		return EmbedResponse{Error: "embed: egress transport is not configured"}
	}
	body := []byte(request.Text)
	response, err := executor.Do(ctx, traceID, egress.HTTPRequest{Request: egress.Request{
		TargetURL: endpoint, Purpose: "embedding", Method: method,
		RequestSHA256: egress.RequestDigest(method, endpoint, body, false)},
		Headers: map[string]string{"Content-Type": "text/plain"}, Body: body,
		MaxResponseBytes: int64(bus.ModuleMessageMaxBody) - 12, TimeoutMS: embedTimeout.Milliseconds()})
	if err != nil {
		breaker.reportFailure(now)
		return EmbedResponse{Error: "embed: " + err.Error()}
	}

	if response.Status == 401 || response.Status == 403 {
		// Reached and refused. Non-retryable, and it CLOSES any earlier outage.
		breaker.reportSuccess(now)
		return EmbedResponse{Unauthorized: true,
			Error: fmt.Sprintf("embed: not authorized (HTTP %d)", response.Status)}
	}
	if response.Status < 200 || response.Status > 299 || len(response.Body) == 0 {
		breaker.reportFailure(now)
		return EmbedResponse{Error: fmt.Sprintf("embed: HTTP %d", response.Status)}
	}

	var raw []float64
	if json.Unmarshal(response.Body, &raw) != nil {
		breaker.reportFailure(now)
		return EmbedResponse{Error: "embed: response is not a JSON array of numbers"}
	}

	out := EmbedResponse{}
	// Truncation is REPORTED, never silent: keeping the first max_dim leaves the
	// stored and query vectors inconsistent with the model's real output and
	// quietly degrades recall, so an operator has to be told to raise the cap.
	if len(raw) > request.MaxDim {
		out.Truncated = true
		raw = raw[:request.MaxDim]
	}
	out.Vector = make([]float32, len(raw))
	for i, v := range raw {
		out.Vector[i] = float32(v)
	}
	out.Dim = len(out.Vector)
	if out.Dim == 0 {
		breaker.reportFailure(now)
		return EmbedResponse{Error: "embed: response carried no dimensions"}
	}
	breaker.reportSuccess(now)
	return out
}

// handleEmbed serves memory:3 embedding.
func handleEmbed(executor egress.Executor, invocation bus.ModuleInvocation, request []byte) ([]byte, bus.ModuleStatus) {
	var decoded EmbedRequest
	if err := json.Unmarshal(request, &decoded); err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	if invocation.Cancelled() {
		return nil, bus.ModuleStatusCancelled
	}
	encoded, err := json.Marshal(Embed(context.Background(), invocation.TraceID, executor, decoded))
	if err != nil || uint32(len(encoded)) > bus.ModuleMessageMaxBody {
		return nil, bus.ModuleStatusInternal
	}
	return encoded, bus.ModuleStatusOK
}
