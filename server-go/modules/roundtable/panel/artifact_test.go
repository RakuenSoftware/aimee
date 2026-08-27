package panel

import (
	"bytes"
	"context"
	"io"
	"net/http"
	"strings"
	"testing"

	"github.com/JBailes/aimee/server-go/modules/egress"
)

type artifactEgress struct{ client *http.Client }

func (artifactEgress) Authorize(context.Context, uint64, egress.Request) (egress.Decision, error) {
	return egress.Decision{Allowed: true, PolicyRevision: egress.PolicyRevision}, nil
}

func (e artifactEgress) Do(ctx context.Context, _ uint64, request egress.HTTPRequest) (egress.HTTPResponse, error) {
	req, err := http.NewRequestWithContext(ctx, request.Method, request.TargetURL, bytes.NewReader(request.Body))
	if err != nil {
		return egress.HTTPResponse{}, err
	}
	for name, value := range request.Headers {
		req.Header.Set(name, value)
	}
	client := e.client
	if client == nil {
		client = http.DefaultClient
	}
	effective := *client
	effective.CheckRedirect = func(*http.Request, []*http.Request) error { return http.ErrUseLastResponse }
	response, err := effective.Do(req)
	if err != nil {
		return egress.HTTPResponse{}, err
	}
	defer response.Body.Close()
	body, err := io.ReadAll(io.LimitReader(response.Body, request.MaxResponseBytes+1))
	return egress.HTTPResponse{Status: response.StatusCode, Location: response.Header.Get("Location"), Body: body}, err
}

var allowEgress egress.Executor = artifactEgress{}

type artifactRoundTripFunc func(*http.Request) (*http.Response, error)

func (f artifactRoundTripFunc) RoundTrip(request *http.Request) (*http.Response, error) {
	return f(request)
}

func TestMaterializeArtifactFetchesGitHubPullRequestDiff(t *testing.T) {
	client := &http.Client{Transport: artifactRoundTripFunc(func(request *http.Request) (*http.Response, error) {
		if got := request.URL.String(); got != "https://github.com/RakuenSoftware/aimee/pull/1828.diff" {
			t.Fatalf("URL=%q", got)
		}
		return &http.Response{StatusCode: http.StatusOK, Body: io.NopCloser(strings.NewReader("diff --git a/a b/a\n")), Header: make(http.Header)}, nil
	})}
	artifact, err := MaterializeArtifact(t.Context(), "https://github.com/RakuenSoftware/aimee/pull/1828/files", artifactEgress{client}, 0)
	if err != nil || !strings.HasPrefix(artifact, "diff --git") {
		t.Fatalf("artifact=%q err=%v", artifact, err)
	}
}

func TestMaterializeArtifactRejectsArbitraryURLs(t *testing.T) {
	for _, raw := range []string{"http://github.com/a/b/pull/1", "https://example.com/a/b/pull/1", "https://github.com/a/b/issues/1"} {
		if _, err := MaterializeArtifact(t.Context(), raw, allowEgress, 0); err == nil {
			t.Fatalf("accepted %q", raw)
		}
	}
}

// An injected client supplies transport only. It must not be able to relax the
// GitHub-only redirect policy this boundary owns.
func TestMaterializeArtifactRefusesRedirectOffGitHubWithInjectedClient(t *testing.T) {
	hits := 0
	client := &http.Client{
		CheckRedirect: func(*http.Request, []*http.Request) error { return nil },
		Transport: artifactRoundTripFunc(func(request *http.Request) (*http.Response, error) {
			hits++
			if request.URL.Hostname() != "github.com" {
				t.Fatalf("artifact fetch left GitHub: %s", request.URL)
			}
			header := make(http.Header)
			header.Set("Location", "https://evil.example.com/leak")
			return &http.Response{StatusCode: http.StatusFound, Body: io.NopCloser(strings.NewReader("")), Header: header, Request: request}, nil
		}),
	}
	if _, err := MaterializeArtifact(t.Context(), "https://github.com/RakuenSoftware/aimee/pull/1828", artifactEgress{client}, 0); err == nil {
		t.Fatal("redirect off GitHub was followed")
	}
	if hits != 1 {
		t.Fatalf("transport hits=%d", hits)
	}
}

func TestMaterializeArtifactPreservesInlineBytes(t *testing.T) {
	raw := "diff --git a/a b/a\n+https://example.com is data\n"
	artifact, err := MaterializeArtifact(t.Context(), raw, allowEgress, 0)
	if err != nil || artifact != raw {
		t.Fatalf("artifact=%q err=%v", artifact, err)
	}
}
