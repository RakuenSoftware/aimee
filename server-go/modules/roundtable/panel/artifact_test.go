package panel

import (
	"io"
	"net/http"
	"strings"
	"testing"
)

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
	artifact, err := MaterializeArtifact(t.Context(), "https://github.com/RakuenSoftware/aimee/pull/1828/files", client)
	if err != nil || !strings.HasPrefix(artifact, "diff --git") {
		t.Fatalf("artifact=%q err=%v", artifact, err)
	}
}

func TestMaterializeArtifactRejectsArbitraryURLs(t *testing.T) {
	for _, raw := range []string{"http://github.com/a/b/pull/1", "https://example.com/a/b/pull/1", "https://github.com/a/b/issues/1"} {
		if _, err := MaterializeArtifact(t.Context(), raw, nil); err == nil {
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
	if _, err := MaterializeArtifact(t.Context(), "https://github.com/RakuenSoftware/aimee/pull/1828", client); err == nil {
		t.Fatal("redirect off GitHub was followed")
	}
	if hits != 1 {
		t.Fatalf("transport hits=%d", hits)
	}
}

func TestMaterializeArtifactPreservesInlineBytes(t *testing.T) {
	raw := "diff --git a/a b/a\n+https://example.com is data\n"
	artifact, err := MaterializeArtifact(t.Context(), raw, nil)
	if err != nil || artifact != raw {
		t.Fatalf("artifact=%q err=%v", artifact, err)
	}
}
