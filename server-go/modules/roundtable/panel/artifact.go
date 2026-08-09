package panel

import (
	"context"
	"errors"
	"fmt"
	"io"
	"net/http"
	"net/url"
	"strconv"
	"strings"
	"time"
)

const MaxArtifactBytes = 16 << 20

func checkArtifactRedirect(req *http.Request, via []*http.Request) error {
	if len(via) >= 5 || req.URL.Scheme != "https" {
		return errors.New("unsafe artifact redirect")
	}
	host := strings.ToLower(req.URL.Hostname())
	if host != "github.com" && host != "patch-diff.githubusercontent.com" {
		return errors.New("artifact redirect left GitHub")
	}
	return nil
}

var artifactHTTPClient = &http.Client{
	Timeout:       30 * time.Second,
	CheckRedirect: checkArtifactRedirect,
}

// artifactClient imposes the redirect and timeout policy of this boundary on
// every fetch. An injected client contributes its transport, never its ability
// to follow a redirect off GitHub.
func artifactClient(client *http.Client) *http.Client {
	if client == nil {
		return artifactHTTPClient
	}
	effective := *client
	effective.CheckRedirect = checkArtifactRedirect
	if effective.Timeout <= 0 {
		effective.Timeout = 30 * time.Second
	}
	return &effective
}

// MaterializeArtifact converts an exact GitHub pull-request URL into immutable
// review bytes at the service boundary. Reviewers never need to spend their tool
// budget discovering or downloading the artifact themselves.
func MaterializeArtifact(ctx context.Context, raw string, client *http.Client) (string, error) {
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
	request, err := http.NewRequestWithContext(ctx, http.MethodGet, diffURL, nil)
	if err != nil {
		return "", err
	}
	request.Header.Set("Accept", "text/plain")
	response, err := artifactClient(client).Do(request)
	if err != nil {
		return "", fmt.Errorf("fetch GitHub pull-request artifact: %w", err)
	}
	defer response.Body.Close()
	if response.StatusCode < 200 || response.StatusCode >= 300 {
		return "", fmt.Errorf("fetch GitHub pull-request artifact: HTTP %d", response.StatusCode)
	}
	content, err := io.ReadAll(io.LimitReader(response.Body, MaxArtifactBytes+1))
	if err != nil {
		return "", fmt.Errorf("read GitHub pull-request artifact: %w", err)
	}
	if len(content) == 0 {
		return "", errors.New("GitHub pull-request artifact is empty")
	}
	if len(content) > MaxArtifactBytes {
		return "", ValidationError{Message: "roundtable artifact exceeds 16 MiB"}
	}
	return string(content), nil
}
