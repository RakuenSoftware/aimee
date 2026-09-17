package memory

import (
	"context"
	"encoding/json"
	"fmt"
	"math"
	"net/url"
	"os"
	"os/exec"
	"strconv"
	"strings"
	"time"

	"github.com/JBailes/aimee/server-go/bus"
	"github.com/JBailes/aimee/server-go/modules/egress"
)

// Loaded batch embedders need more headroom than single-text inference.
func embedHTTPTimeout() time.Duration {
	ms, err := strconv.ParseInt(os.Getenv("AIMEE_EMBED_HTTP_TIMEOUT_MS"), 10, 64)
	if err != nil || ms <= 0 || ms > 120000 {
		return 120 * time.Second
	}
	return time.Duration(ms) * time.Millisecond
}

// EmbedBatch makes one request and publishes no vectors unless every row has
// the requested width. Non-HTTP embedders use the caller's single-text fallback.
func EmbedBatch(ctx context.Context, trace uint64, executor egress.Executor, request EmbedRequest) EmbedResponse {
	if !EmbedIsHTTP(request.BaseURL) || request.MaxDim <= 0 || len(request.Texts) == 0 {
		return EmbedResponse{Error: "embed: invalid batch request"}
	}
	for _, text := range request.Texts {
		if text == "" {
			return EmbedResponse{Error: "embed: empty text"}
		}
	}
	if executor == nil {
		return EmbedResponse{Error: "embed: egress transport is not configured"}
	}
	now := nowOr(request.NowMS)
	if allowed, retry := breaker.allow(now); !allowed {
		return EmbedResponse{Unavailable: true, RetryAfterMS: retry}
	}
	endpoint := strings.TrimSuffix(request.BaseURL, "/") + "/embed_batch"
	if request.InputType != "" {
		endpoint += "?input_type=" + url.QueryEscape(request.InputType)
	}
	body, _ := json.Marshal(request.Texts)
	response, err := executor.Do(ctx, trace, egress.HTTPRequest{Request: egress.Request{
		TargetURL: endpoint, Purpose: "embedding", Method: "POST", RequestSHA256: egress.RequestDigest("POST", endpoint, body, false)},
		Headers: map[string]string{"Content-Type": "application/json"}, Body: body,
		MaxResponseBytes: int64(bus.ModuleMessageMaxBody) - 12, TimeoutMS: embedHTTPTimeout().Milliseconds()})
	fail := func(message string) EmbedResponse { breaker.reportFailure(now); return EmbedResponse{Error: message} }
	if err != nil {
		return fail("embed: " + err.Error())
	}
	if response.Status == 401 || response.Status == 403 {
		breaker.reportSuccess(now)
		return EmbedResponse{Unauthorized: true, Error: fmt.Sprintf("embed: HTTP %d", response.Status)}
	}
	if response.Status < 200 || response.Status >= 300 {
		return fail(fmt.Sprintf("embed: HTTP %d", response.Status))
	}
	var raw [][]*float64
	if json.Unmarshal(response.Body, &raw) != nil || len(raw) != len(request.Texts) {
		return fail("embed: invalid batch row count")
	}
	vectors := make([][]float32, len(raw))
	for i, row := range raw {
		if len(row) != request.MaxDim {
			return fail("embed: invalid batch vector width")
		}
		vectors[i] = make([]float32, len(row))
		for j, component := range row {
			if component == nil {
				return fail("embed: null vector component")
			}
			value := *component
			if math.IsNaN(value) || math.IsInf(value, 0) || math.Abs(value) > math.MaxFloat32 {
				return fail("embed: non-finite vector component")
			}
			vectors[i][j] = float32(value)
		}
	}
	breaker.reportSuccess(now)
	return EmbedResponse{Vectors: vectors, Dim: request.MaxDim}
}

// EmbedDimension owns the configured program's --dim protocol as well as the
// HTTP probe, so both native hosts use the same dimension decision.
func EmbedDimension(ctx context.Context, trace uint64, executor egress.Executor, request EmbedRequest) EmbedResponse {
	if request.BaseURL == "" || request.MaxDim <= 0 {
		return EmbedResponse{Error: "embed: invalid dimension request"}
	}
	if EmbedIsHTTP(request.BaseURL) {
		request.Text = "dim probe"
		request.InputType = "document"
		return Embed(ctx, trace, executor, request)
	}
	output, err := exec.CommandContext(ctx, "sh", "-c", request.BaseURL+" --dim").Output()
	if err != nil {
		return EmbedResponse{Error: "embed: dimension command failed"}
	}
	dim, err := strconv.Atoi(strings.TrimSpace(string(output)))
	if err != nil || dim <= 0 || dim > request.MaxDim {
		return EmbedResponse{Error: "embed: invalid dimension"}
	}
	return EmbedResponse{Dim: dim}
}
