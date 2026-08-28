package panel

import (
	"context"
	"errors"
	"fmt"
	"net/url"
	"strconv"
	"strings"
	"time"

	"github.com/JBailes/aimee/server-go/bus"
	"github.com/JBailes/aimee/server-go/modules/egress"
)

const MaxArtifactBytes = int(bus.ModuleMessageMaxBody) - 13

// MaterializeArtifact converts an exact GitHub pull-request URL into immutable
// review bytes at the service boundary. Reviewers never need to spend their tool
// budget discovering or downloading the artifact themselves.
func MaterializeArtifact(ctx context.Context, raw string, executor egress.Executor,
	traceID uint64) (string, error) {
	trimmed := strings.TrimSpace(raw)
	parsed, err := url.Parse(trimmed)
	if err != nil || parsed.Scheme == "" || parsed.Host == "" {
		return raw, nil
	}
	if parsed.Scheme != "https" || !strings.EqualFold(parsed.Hostname(), "github.com") || parsed.User != nil {
		return "", ValidationError{Message: "URL review artifacts must be HTTPS GitHub pull-request URLs"}
	}
	parts := strings.Split(strings.Trim(parsed.EscapedPath(), "/"), "/")
	if len(parts) < 4 || parts[0] == "" || parts[1] == "" || parts[2] != "pull" {
		return "", ValidationError{Message: "GitHub artifact URL must identify a pull request"}
	}
	number := strings.TrimSuffix(parts[3], ".diff")
	if _, err := strconv.ParseUint(number, 10, 64); err != nil {
		return "", ValidationError{Message: "GitHub artifact URL has an invalid pull-request number"}
	}
	diffURL := "https://github.com/" + parts[0] + "/" + parts[1] + "/pull/" + number + ".diff"
	if executor == nil {
		return "", errors.New("fetch GitHub pull-request artifact: egress transport is not configured")
	}
	target := diffURL
	for redirects := 0; redirects < 5; redirects++ {
		response, callErr := executor.Do(ctx, traceID, egress.HTTPRequest{Request: egress.Request{
			TargetURL: target, Purpose: "review_artifact", Method: "GET",
			RequestSHA256: egress.RequestDigest("GET", target, nil, false)},
			Headers:          map[string]string{"Accept": "text/plain"},
			MaxResponseBytes: int64(MaxArtifactBytes + 1), TimeoutMS: (30 * time.Second).Milliseconds()})
		if callErr != nil {
			return "", fmt.Errorf("fetch GitHub pull-request artifact: %w", callErr)
		}
		if response.Status >= 300 && response.Status < 400 {
			base, baseErr := url.Parse(target)
			next, nextErr := url.Parse(response.Location)
			if baseErr != nil || nextErr != nil || response.Location == "" {
				return "", errors.New("fetch GitHub pull-request artifact: unsafe redirect")
			}
			resolved := base.ResolveReference(next)
			host := strings.ToLower(resolved.Hostname())
			if resolved.Scheme != "https" || resolved.User != nil ||
				(host != "github.com" && host != "patch-diff.githubusercontent.com") {
				return "", errors.New("fetch GitHub pull-request artifact: redirect left GitHub")
			}
			target = resolved.String()
			continue
		}
		if response.Status < 200 || response.Status >= 300 {
			return "", fmt.Errorf("fetch GitHub pull-request artifact: HTTP %d", response.Status)
		}
		if len(response.Body) == 0 {
			return "", errors.New("GitHub pull-request artifact is empty")
		}
		if len(response.Body) > MaxArtifactBytes {
			return "", ValidationError{Message: "roundtable artifact exceeds 16 MiB"}
		}
		return string(response.Body), nil
	}
	return "", errors.New("fetch GitHub pull-request artifact: too many redirects")
}
