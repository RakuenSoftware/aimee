package cognee

import (
	"context"
	"encoding/json"
	memory "github.com/JBailes/aimee/server-go/memory"
	"github.com/JBailes/aimee/server-go/modules/memory/backendstore"
	"io"
	"net/http"
	"net/url"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

// This opt-in test uses the real Cognee API, not the route fixture. Use a
// disposable account/database: it creates and deletes datasets for this run.
func TestCogneeLiveContract(t *testing.T) {
	endpoint := os.Getenv("AIMEE_COGNEE_TEST_URL")
	if endpoint == "" {
		t.Skip("set AIMEE_COGNEE_TEST_URL for a disposable Cognee server")
	}
	ctx, cancel := context.WithTimeout(context.Background(), 2*time.Minute)
	defer cancel()
	client := &http.Client{Timeout: time.Minute}
	form := url.Values{"username": {os.Getenv("AIMEE_COGNEE_TEST_USER")}, "password": {os.Getenv("AIMEE_COGNEE_TEST_PASSWORD")}}
	req, err := http.NewRequestWithContext(ctx, "POST", endpoint+"/api/v1/auth/login", strings.NewReader(form.Encode()))
	if err != nil {
		t.Fatal(err)
	}
	req.Header.Set("Content-Type", "application/x-www-form-urlencoded")
	resp, err := client.Do(req)
	if err != nil {
		t.Fatal(err)
	}
	var login struct {
		Token string `json:"access_token"`
	}
	err = json.NewDecoder(resp.Body).Decode(&login)
	resp.Body.Close()
	if err != nil || resp.StatusCode != 200 || login.Token == "" {
		t.Fatal("disposable Cognee login failed", resp.StatusCode, err)
	}
	transport := func(ctx context.Context, method, target, contentType string, body []byte) (int, []byte, error) {
		request, err := http.NewRequestWithContext(ctx, method, target, strings.NewReader(string(body)))
		if err != nil {
			return 0, nil, err
		}
		request.Header.Set("Authorization", "Bearer "+login.Token)
		if contentType != "" {
			request.Header.Set("Content-Type", contentType)
		}
		response, err := client.Do(request)
		if err != nil {
			return 0, nil, err
		}
		defer response.Body.Close()
		raw, err := io.ReadAll(io.LimitReader(response.Body, MaxBody+1))
		if response.StatusCode >= 300 {
			t.Logf("Cognee route %s %s: status %d", method, request.URL.Path, response.StatusCode)
		}
		return response.StatusCode, raw, err
	}
	scope := memory.Scope{Type: memory.ScopeProject, Value: "contract-live"}
	dir := filepath.Join(t.TempDir(), "catalog")
	records, err := backendstore.New(dir, "11111111-1111-4111-8111-111111111111")
	if err != nil {
		t.Fatal(err)
	}
	if _, err = records.Put(ctx, scope, memory.Record{Kind: "fact", Tier: "L2", Key: "needle", Content: "needle canonical", Confidence: .8}); err != nil {
		t.Fatal(err)
	}
	records, err = backendstore.New(dir, "11111111-1111-4111-8111-111111111111")
	if err != nil {
		t.Fatal(err)
	}
	backend, err := New(records, transport, endpoint, "contract-live-"+time.Now().UTC().Format("20060102T150405.000000000"))
	if err != nil {
		t.Fatal(err)
	}
	defer func() {
		cleanup, cancel := context.WithTimeout(context.Background(), time.Minute)
		defer cancel()
		if err := backend.ResetDerived(cleanup); err != nil {
			t.Error("live test cleanup", err)
		}
	}()
	found, err := backend.Search(ctx, scope, "needle", "fact", "L2", 1)
	if err != nil || len(found) != 1 || found[0].ID != 1 || found[0].Content != "needle canonical" {
		t.Fatal("real Cognee retrieval contract", found, err)
	}
	if deleted, err := backend.Delete(ctx, scope, 1); err != nil || !deleted {
		t.Fatal("real Cognee deletion contract", deleted, err)
	}

	// Reproduce subject erasure outside Store.Delete: the existing canonical
	// owner has already removed the source row while Cognee retains revisions.
	private := memory.Scope{Type: memory.ScopeUser, Value: "_user"}
	subject := memory.Record{Scope: private, Kind: "fact", Tier: "L2", Key: "subject", Content: "needle private subject", Confidence: .8}
	if _, err := records.Put(ctx, private, subject); err != nil {
		t.Fatal(err)
	}
	if _, err := backend.Search(ctx, private, "needle", "", "", 1); err != nil {
		t.Fatal("index private subject", err)
	}
	if _, err := records.Delete(ctx, private, 2); err != nil {
		t.Fatal(err)
	}
	retained := memory.Record{Scope: scope, Kind: "fact", Tier: "L2", Key: "retained", Content: "needle retained canonical", Confidence: .8}
	if _, err := records.Put(ctx, scope, retained); err != nil {
		t.Fatal(err)
	}
	if _, err := backend.Search(ctx, scope, "needle", "", "", 1); err != nil {
		t.Fatal("index retained record", err)
	}
	if err := backend.ResetDerived(ctx); err != nil {
		t.Fatal("subject cleanup", err)
	}
	if _, err := records.Get(ctx, scope, 3); err != nil {
		t.Fatal("cleanup erased unrelated canonical record", err)
	}
	if err := backend.ResetDerived(ctx); err != nil {
		t.Fatal("cleanup retry", err)
	}
	datasets, err := backend.datasets(ctx)
	if err != nil {
		t.Fatal(err)
	}
	for _, d := range datasets {
		if strings.HasPrefix(d.Name, "aimee_"+backend.namespace+"_") {
			t.Fatal("derived dataset survived deletion")
		}
	}
}
