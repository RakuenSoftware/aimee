package egress

import (
	"encoding/json"
	"net"
	"net/http"
	"net/http/httptest"
	"net/url"
	"sync/atomic"
	"testing"

	"github.com/RakuenSoftware/ouroboros-ring/go/ring"
)

func TestHTTPStageExecutesBoundBytesAndDoesNotFollowRedirects(t *testing.T) {
	var escaped atomic.Int32
	evil := httptest.NewServer(http.HandlerFunc(func(http.ResponseWriter, *http.Request) { escaped.Add(1) }))
	defer evil.Close()
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path == "/embed" && r.URL.Query().Has("redirect") {
			http.Redirect(w, r, evil.URL, http.StatusFound)
			return
		}
		w.WriteHeader(http.StatusCreated)
		_, _ = w.Write([]byte(`[1,2,3]`))
	}))
	defer server.Close()
	handler := newHandler(fixedResolver{{IP: net.ParseIP("127.0.0.1")}})
	call := func(target string) ([]byte, ring.ModuleStatus) {
		request := HTTPRequest{Request: Request{TargetURL: target, Purpose: "embedding", Method: "POST",
			RequestSHA256: RequestDigest("POST", target, []byte("text"), false)},
			Headers: map[string]string{"Content-Type": "text/plain"}, Body: []byte("text"),
			MaxResponseBytes: 1024, TimeoutMS: 1000}
		body, _ := json.Marshal(request)
		return handler(ring.ModuleInvocation{PrincipalClass: 1, PrincipalRef: MemoryClientRef,
			StageID: StageHTTP}, body)
	}
	reply, status := call(server.URL + "/embed")
	response, err := decodeHTTPResponse(reply)
	if status != ring.ModuleStatusOK || err != nil || response.Status != http.StatusCreated || string(response.Body) != `[1,2,3]` {
		t.Fatalf("status=%d response=%+v err=%v", status, response, err)
	}
	reply, status = call(server.URL + "/embed?redirect=1")
	response, err = decodeHTTPResponse(reply)
	if status != ring.ModuleStatusOK || err != nil || response.Status != http.StatusFound || response.Location != evil.URL {
		t.Fatalf("redirect status=%d response=%+v err=%v", status, response, err)
	}
	if escaped.Load() != 0 {
		t.Fatal("egress followed a redirect without a new governed request")
	}
}

func TestHTTPStageRejectsRequestDigestDrift(t *testing.T) {
	handler := newHandler(fixedResolver{{IP: net.ParseIP("127.0.0.1")}})
	request := HTTPRequest{Request: Request{TargetURL: "http://127.0.0.1:1/embed", Purpose: "embedding",
		Method: "POST", RequestSHA256: RequestDigest("POST", "http://127.0.0.1:1/embed", []byte("before"), false)},
		Body: []byte("after"), MaxResponseBytes: 32, TimeoutMS: 1000}
	body, _ := json.Marshal(request)
	if _, status := handler(ring.ModuleInvocation{PrincipalClass: 1, PrincipalRef: MemoryClientRef,
		StageID: StageHTTP}, body); status != ring.ModuleStatusInvalidRequest {
		t.Fatalf("tampered bytes status=%d", status)
	}
}

func TestProviderOAuthHeadersStayWithinProviderBoundary(t *testing.T) {
	for _, name := range []string{"ChatGPT-Account-ID", "originator", "anthropic-version"} {
		headers := map[string]string{name: "fixture"}
		if !validHTTPHeaders("provider", headers, true) {
			t.Fatal("provider header refused", name)
		}
		if validHTTPHeaders("embedding", headers, true) {
			t.Fatal("provider header escaped purpose", name)
		}
		headers[name] = "fixture\r\nAuthorization: secret"
		if validHTTPHeaders("provider", headers, true) {
			t.Fatal("header injection accepted", name)
		}
	}
	if validHTTPHeaders("provider", map[string]string{"Authorization": "Bearer secret"}, true) {
		t.Fatal("plaintext authorization accepted")
	}
}

func TestAttributionEvidenceReadsStayInExactMergeRepository(t *testing.T) {
	sha := "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
	paths := []string{"/repos/owner/repo/issues", "/repos/owner/repo/issues/3", "/repos/owner/repo/commits/" + sha + "/pulls", "/repos/owner/repo/commits/" + sha + "/check-runs", "/repos/owner/repo/compare/" + sha + "..." + sha, "/repos/owner/repo/check-runs/7", "/repos/owner/repo/check-runs/7/annotations", "/repos/owner/repo/actions/jobs/7", "/repos/owner/repo/actions/jobs/7/logs", "/repos/owner/repo/actions/runs/8"}
	for _, path := range paths {
		if !forgeTargetAllowed("GET", path) || !forgeCredentialTargetAllowed("pr_merge", "owner/repo", "GET", path) {
			t.Fatal("required attribution read refused", path)
		}
		if forgeTargetAllowed("POST", path) || forgeCredentialTargetAllowed("pr_info", "owner/repo", "GET", path) || forgeCredentialTargetAllowed("pr_merge", "other/repo", "GET", path) {
			t.Fatal("attribution read widened authority", path)
		}
	}
	for _, path := range []string{"/repos/owner/repo/actions/jobs/0/logs", "/repos/owner/repo/actions/jobs/7/cancel", "/repos/owner/repo/actions/runs/8/rerun", "/repos/owner/repo/check-runs/7/other", "/repos/owner/repo/compare/main...head", "/repos/owner/repo/commits/main/check-runs", "/repos/owner/repo/hooks", "/repos/owner/repo/actions/jobs/7/logs/extra"} {
		if forgeTargetAllowed("GET", path) || forgeCredentialTargetAllowed("pr_merge", "owner/repo", "GET", path) {
			t.Fatal("unregistered attribution endpoint allowed", path)
		}
	}
}
func TestOnlyOwnedGitHubLogResponseDerivesCredentialFreeDownload(t *testing.T) {
	p := policy{resolver: fixedResolver{{IP: net.ParseIP("8.8.8.8")}}}
	target, _ := url.Parse("https://api.github.com/repos/owner/repo/actions/jobs/7/logs")
	original := HTTPRequest{Request: Request{TargetURL: target.String(), Purpose: "forge", Method: "GET", CredentialPresent: true}, CredentialScope: "pr_merge", CredentialResource: "owner/repo", CredentialHandle: "forge", Credential: &CredentialEnvelope{Ciphertext: "fixture"}, Headers: map[string]string{"Accept": "application/vnd.github+json"}, MaxResponseBytes: 4096, TimeoutMS: 1000}
	inv := ring.ModuleInvocation{PrincipalClass: 1, PrincipalRef: GitClientRef}
	next, decision, ok := p.forgeLogDownload(inv, original, target, "https://logs.example.net/signed?sig=fixture")
	if !ok || !decision.Allowed || next.Credential != nil || next.CredentialPresent || next.CredentialHandle != "" || next.CredentialResource != "" || next.CredentialScope != "" || len(next.Headers) != 1 || next.Headers["Accept"] != "text/plain" || next.Body != nil || next.MaxResponseBytes != 4096 || next.TimeoutMS != 1000 {
		t.Fatalf("download identity/bounds changed: %+v %+v", next, decision)
	}
	if _, _, ok := p.forgeLogDownload(inv, next, target, "https://another.example.net/"); ok {
		t.Fatal("second redirect acquired authority")
	}
	for _, destination := range []string{"http://logs.example.net/", "https://user@logs.example.net/", "https://logs.example.net:8443/", "https://logs.example.net/#fragment", "file:///tmp/data"} {
		if _, _, ok := p.forgeLogDownload(inv, original, target, destination); ok {
			t.Fatal("unsafe download admitted", destination)
		}
	}
	private := policy{resolver: fixedResolver{{IP: net.ParseIP("127.0.0.1")}}}
	if _, _, ok := private.forgeLogDownload(inv, original, target, "https://private.example.net/"); ok {
		t.Fatal("signed download bypassed public DNS checks")
	}
	for _, change := range []func(*HTTPRequest){func(r *HTTPRequest) { r.CredentialScope = "pr_info" }, func(r *HTTPRequest) { r.CredentialResource = "other/repo" }, func(r *HTTPRequest) { r.CredentialPresent = false }, func(r *HTTPRequest) { r.Method = "POST" }, func(r *HTTPRequest) { r.Purpose = "provider" }} {
		copy := original
		change(&copy)
		if _, _, ok := p.forgeLogDownload(inv, copy, target, "https://logs.example.net/"); ok {
			t.Fatal("caller scope escaped original log response")
		}
	}
	other := inv
	other.PrincipalRef = ProvidersClientRef
	if _, _, ok := p.forgeLogDownload(other, original, target, "https://logs.example.net/"); ok {
		t.Fatal("other principal acquired forge download")
	}
	untrusted, _ := url.Parse("https://not-github.example/repos/owner/repo/actions/jobs/7/logs")
	if _, _, ok := p.forgeLogDownload(inv, original, untrusted, "https://logs.example.net/"); ok {
		t.Fatal("untrusted origin acquired download")
	}
}
