package main

import (
	"bufio"
	"context"
	"encoding/json"
	"fmt"
	"io"
	"net"
	"net/http"
	"net/http/httptest"
	"net/url"
	"strconv"
	"strings"
	"testing"
	"time"
)

func editorProxyFixture(t *testing.T, handler http.HandlerFunc) (*httptest.Server, *httptest.Server) {
	t.Helper()
	upstream := httptest.NewServer(handler)
	t.Cleanup(upstream.Close)
	peer, _ := url.Parse(upstream.URL)
	port, _ := strconv.Atoi(peer.Port())
	s := &server{}
	front := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if !strings.HasPrefix(r.URL.Path, "/vscode/") {
			http.NotFound(w, r)
			return
		}
		s.proxyToEditor(w, r, "test-operator", port)
	}))
	t.Cleanup(front.Close)
	return front, upstream
}

func TestVSCodeProxyWorkerURLUsesEditorSubpath(t *testing.T) {
	// Pinned code-server4.96.4 src/node/http.ts relativeRoot counts slashes in
	// originalUrl before '?'. Its service-worker.diff appends this static path.
	// Unstripped /vscode/ produces ./.., which the browser resolves outside our
	// authenticated editor route. Exercise the resulting HTTP fetch, not just a
	// mocked path comparison.
	front, _ := editorProxyFixture(t, func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path == "/_static/out/browser/serviceWorker.js" {
			w.Header().Set("Service-Worker-Allowed", "/")
			_, _ = io.WriteString(w, "self.addEventListener('fetch',()=>{});")
			return
		}
		depth := strings.Count(strings.SplitN(r.RequestURI, "?", 2)[0], "/")
		base := "."
		if depth > 1 {
			base += strings.TrimSuffix("/"+strings.Repeat("../", depth-1), "/")
		}
		_ = json.NewEncoder(w).Encode(map[string]string{"worker": base + "/_static/out/browser/serviceWorker.js"})
	})
	page, err := http.Get(front.URL + "/vscode/?folder=file%3A%2F%2F%2Fworkspace%2FREADME")
	if err != nil {
		t.Fatal(err)
	}
	defer page.Body.Close()
	var config struct {
		Worker string `json:"worker"`
	}
	if err := json.NewDecoder(page.Body).Decode(&config); err != nil {
		t.Fatal(err)
	}
	base, _ := url.Parse(front.URL + "/vscode/")
	worker, _ := url.Parse(config.Worker)
	resolved := base.ResolveReference(worker)
	response, err := http.Get(resolved.String())
	if err != nil {
		t.Fatal(err)
	}
	defer response.Body.Close()
	if response.StatusCode != http.StatusOK {
		t.Fatalf("worker URL %s returned %d, want200 under /vscode/", resolved.Path, response.StatusCode)
	}
	if resolved.Path != "/vscode/_static/out/browser/serviceWorker.js" {
		t.Fatalf("worker escaped editor: %s", resolved.Path)
	}
	if got := response.Header.Get("Service-Worker-Allowed"); got != "/vscode/" {
		t.Fatalf("worker scope=%q, want /vscode/", got)
	}
}

func TestVSCodeProxyPreservesEncodedPathsQueryAndStripsCredentials(t *testing.T) {
	type seen struct{ URI, Cookie, Authorization, Host, ForwardedHost string }
	requests := make(chan seen, 4)
	front, upstream := editorProxyFixture(t, func(w http.ResponseWriter, r *http.Request) {
		requests <- seen{r.RequestURI, r.Header.Get("Cookie"), r.Header.Get("Authorization"), r.Host, r.Header.Get("X-Forwarded-Host")}
		w.WriteHeader(http.StatusNoContent)
	})
	for _, path := range []string{"/vscode/resource/file%3A%2f%2F%2Fwork%2FREADME", "/%76scode/resource/file%3A%2F%2F%2Fwork%2FREADME", "/vscode/resource/%E2%82%AC%2fREADME"} {
		query := "folder=file%3A%2F%2F%2Fwork%2FREADME&value=a%2Bb+space&again=1&again=2"
		req, _ := http.NewRequest(http.MethodGet, front.URL+path+"?"+query, nil)
		req.Host = "aimee.example:8443"
		req.Header.Set("Cookie", "session=fixture-private")
		req.Header.Set("Authorization", "Bearer fixture-private")
		response, err := http.DefaultClient.Do(req)
		if err != nil {
			t.Fatal(err)
		}
		_ = response.Body.Close()
		got := <-requests
		expected := strings.Replace(path, "/%76scode", "/vscode", 1)
		expected = strings.TrimPrefix(expected, "/vscode") + "?" + query
		if got.URI != expected {
			t.Errorf("URI=%q want%q", got.URI, expected)
		}
		peer, _ := url.Parse(upstream.URL)
		if got.Cookie != "" || got.Authorization != "" || got.Host != peer.Host || got.ForwardedHost != req.Host {
			t.Errorf("credential/authority forwarding mismatch: %#v", got)
		}
	}
}

func TestVSCodeProxyRedirectsAndWorkerScope(t *testing.T) {
	var loopback string
	front, upstream := editorProxyFixture(t, func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Location", r.URL.Query().Get("location"))
		if scope := r.URL.Query().Get("scope"); scope != "" {
			w.Header().Set("Service-Worker-Allowed", scope)
		}
		w.WriteHeader(http.StatusFound)
	})
	loopback = upstream.URL
	client := &http.Client{CheckRedirect: func(*http.Request, []*http.Request) error { return http.ErrUseLastResponse }}
	for _, tc := range []struct{ name, location, want string }{
		{"root", "/", "/vscode/"},
		{"root query encoded", "/login?folder=file%3A%2F%2F%2Fwork#one", "/vscode/login?folder=file%3A%2F%2F%2Fwork#one"},
		{"loopback", loopback + "/login%2Fcallback?q=a%2Bb", "/vscode/login%2Fcallback?q=a%2Bb"},
		{"loopback root", loopback + "?folder=file%3A%2Fwork", "/vscode/?folder=file%3A%2Fwork"},
		{"loopback network reference", strings.TrimPrefix(loopback, "http:") + "/login", "/vscode/login"},
		{"relative", "./login?next=a%2Fb", "./login?next=a%2Fb"},
		{"relative parent", "../login", "../login"},
		{"external", "https://other.example/login", "https://other.example/login"},
		{"external network reference", "//other.example/login", "//other.example/login"},
		{"external scheme", "ftp://" + strings.TrimPrefix(loopback, "http://") + "/file", "ftp://" + strings.TrimPrefix(loopback, "http://") + "/file"},
	} {
		t.Run(tc.name, func(t *testing.T) {
			response, err := client.Get(front.URL + "/vscode/redirect?location=" + url.QueryEscape(tc.location) + "&scope=%2F")
			if err != nil {
				t.Fatal(err)
			}
			defer response.Body.Close()
			if got := response.Header.Get("Location"); got != tc.want {
				t.Errorf("Location=%q want%q", got, tc.want)
			}
			if got := response.Header.Get("Service-Worker-Allowed"); got != "/vscode/" {
				t.Errorf("scope=%q", got)
			}
		})
	}
}

func TestVSCodeProxyWebSocketPathAndCredentials(t *testing.T) {
	seen := make(chan string, 1)
	front, _ := editorProxyFixture(t, func(w http.ResponseWriter, r *http.Request) {
		if r.Header.Get("Cookie") != "" || r.Header.Get("Authorization") != "" {
			seen <- "credential leak"
			http.Error(w, "bad credentials", 400)
			return
		}
		seen <- r.RequestURI
		conn, rw, err := w.(http.Hijacker).Hijack()
		if err != nil {
			return
		}
		defer conn.Close()
		_ = conn.SetDeadline(time.Now().Add(5 * time.Second))
		_, _ = rw.WriteString("HTTP/1.1 101 Switching Protocols\r\nConnection: Upgrade\r\nUpgrade: websocket\r\n\r\n")
		_ = rw.Flush()
		data := make([]byte, 4)
		if _, err = io.ReadFull(rw, data); err == nil && string(data) == "ping" {
			_, _ = rw.WriteString("pong")
			_ = rw.Flush()
		}
	})
	endpoint, _ := url.Parse(front.URL)
	conn, err := net.DialTimeout("tcp", endpoint.Host, 5*time.Second)
	if err != nil {
		t.Fatal(err)
	}
	defer conn.Close()
	_ = conn.SetDeadline(time.Now().Add(5 * time.Second))
	path := "/vscode/socket%2Ftransport?reconnectionToken=fixture%2Bopaque&skipWebSocketFrames=false"
	_, err = fmt.Fprintf(conn, "GET %s HTTP/1.1\r\nHost: %s\r\nOrigin: %s\r\nConnection: Upgrade\r\nUpgrade: websocket\r\nCookie: session=fixture-private\r\nAuthorization: Bearer fixture-private\r\n\r\n", path, endpoint.Host, front.URL)
	if err != nil {
		t.Fatal(err)
	}
	reader := bufio.NewReader(conn)
	response, err := http.ReadResponse(reader, nil)
	if err != nil {
		t.Fatal(err)
	}
	if response.StatusCode != http.StatusSwitchingProtocols {
		t.Fatalf("upgrade=%d", response.StatusCode)
	}
	if got := <-seen; got != strings.TrimPrefix(path, "/vscode") {
		t.Fatalf("WS URI=%q", got)
	}
	_, _ = conn.Write([]byte("ping"))
	reply := make([]byte, 4)
	if _, err = io.ReadFull(reader, reply); err != nil || string(reply) != "pong" {
		t.Fatalf("WS exchange=%q err=%v", reply, err)
	}
}

func TestVSCodeAccessGatesRunBeforeEditorResolution(t *testing.T) {
	s := &server{}
	for _, tc := range []struct {
		name, user, origin, method string
		want                       int
	}{
		{"unauthenticated", "", "", "GET", http.StatusUnauthorized},
		{"foreign origin", "operator", "https://foreign.example", "GET", http.StatusForbidden},
		{"unsafe without origin", "operator", "", "POST", http.StatusForbidden},
	} {
		t.Run(tc.name, func(t *testing.T) {
			req := httptest.NewRequest(tc.method, "https://aimee.example/vscode/", nil)
			req.Header.Set("Origin", tc.origin)
			if tc.user != "" {
				req = req.WithContext(context.WithValue(req.Context(), ctxUsername, tc.user))
			}
			response := httptest.NewRecorder()
			s.handleVSCode(response, req)
			if response.Code != tc.want {
				t.Fatalf("status=%d want%d", response.Code, tc.want)
			}
		})
	}
}

func TestVSCodeBareMountPreservesEncodedFolderQuery(t *testing.T) {
	s := &server{}
	query := "folder=file%3A%2F%2F%2Fwork%2FREADME&value=a%2Bb"
	req := httptest.NewRequest(http.MethodGet, "https://aimee.example/vscode?"+query, nil)
	req = req.WithContext(context.WithValue(req.Context(), ctxUsername, "operator"))
	response := httptest.NewRecorder()
	s.handleVSCode(response, req)
	if response.Code != http.StatusMovedPermanently || response.Header().Get("Location") != "/vscode/?"+query {
		t.Fatalf("redirect %d %q", response.Code, response.Header().Get("Location"))
	}
}

func TestVSCodeProxyRefusesRootRedirectTraversal(t *testing.T) {
	front, _ := editorProxyFixture(t, func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Location", r.URL.Query().Get("location"))
		w.WriteHeader(http.StatusFound)
	})
	client := &http.Client{CheckRedirect: func(*http.Request, []*http.Request) error { return http.ErrUseLastResponse }}
	for _, location := range []string{"/../api/secret", "/%2e%2e/api/secret", "/nested/../../api/secret", "/nested%5C..%5C..%5Capi/secret"} {
		response, err := client.Get(front.URL + "/vscode/redirect?location=" + url.QueryEscape(location))
		if err != nil {
			t.Fatal(err)
		}
		if response.StatusCode != http.StatusBadGateway || response.Header.Get("Location") != "" {
			t.Errorf("unsafe redirect %q status=%d Location=%q", location, response.StatusCode, response.Header.Get("Location"))
		}
		_ = response.Body.Close()
	}
}

func TestVSCodeProxyWorkerScopeCannotEscapeMount(t *testing.T) {
	front, _ := editorProxyFixture(t, func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Service-Worker-Allowed", r.URL.Query().Get("scope"))
		w.WriteHeader(http.StatusOK)
	})
	for _, tc := range []struct{ scope, want string }{
		{"/", "/vscode/"}, {"/nested%2Fscope/", "/vscode/nested%2Fscope/"},
		{"../..", ""}, {"/../", ""}, {"/%2e%2e/", ""}, {"/nested%5c..%5c../", ""}, {"//foreign.example/", ""}, {"https://foreign.example/", ""}, {"/scope?value=ignored", ""},
	} {
		response, err := http.Get(front.URL + "/vscode/_static/worker.js?scope=" + url.QueryEscape(tc.scope))
		if err != nil {
			t.Fatal(err)
		}
		if got := response.Header.Get("Service-Worker-Allowed"); got != tc.want {
			t.Errorf("scope%q ->%q want%q", tc.scope, got, tc.want)
		}
		_ = response.Body.Close()
	}
}
