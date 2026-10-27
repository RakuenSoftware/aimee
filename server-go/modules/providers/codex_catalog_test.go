package providers

import (
	"context"
	"encoding/json"
	"errors"
	"os"
	"path/filepath"
	"regexp"
	"runtime"
	"strings"
	"testing"
	"time"

	"github.com/JBailes/aimee/server-go/modules/egress"
)

const accountCatalog = `{"models":[{"slug":"hidden","visibility":"hide"},{"slug":"dynamic-first","display_name":"Account first","visibility":"list","supported_in_api":false,"context_window":90000,"priority":1,"model_messages":{"instructions_template":"PRIVATE_PROMPT"}},{"slug":"dynamic-second","visibility":"list","supported_in_api":true}]}`

func codexCatalogManager(t *testing.T) (*Manager, *testVault) {
	m, _, v := manager(t)
	_, err := m.store.transaction(true, func(root object) (object, error) {
		root["providers"] = []object{{"name": "codex", "provider": "chatgpt", "auth_type": "codex-oauth", "endpoint": "https://chatgpt.com/backend-api/codex"}}
		return nil, nil
	})
	if err != nil {
		t.Fatal(err)
	}
	var pair object
	if json.Unmarshal([]byte(codexPair("selected-token", "selected-account")), &pair) != nil {
		t.Fatal("pair")
	}
	pair["client_version"] = "0.139.0"
	b, _ := json.Marshal(pair)
	v.values["codex:codex_oauth_credentials"] = string(b)
	v.values["codex:api_key"] = "wrong-api-key"
	return m, v
}
func TestCodexAccountCatalogUsesDeclaredVersionAndSelectedOAuth(t *testing.T) {
	m, _ := codexCatalogManager(t)
	network := &fixtureNetwork{replies: []egress.HTTPResponse{response(200, accountCatalog)}}
	m.SetEgress(network)
	out := call(t, m, "provider.connection_models", object{"name": "codex"})
	ids := out["models"].([]string)
	details := rows(out, "details")
	if len(ids) != 2 || ids[0] != "dynamic-first" || ids[1] != "dynamic-second" || str(details[0], "id") != "dynamic-first" || number(details[0], "context_window") != 90000 || details[0]["supported_in_api"] != false {
		t.Fatal(out)
	}
	encoded, _ := json.Marshal(out)
	for _, private := range []string{"selected-token", "selected-account", "PRIVATE_PROMPT", "source_digest", "client_version"} {
		if strings.Contains(string(encoded), private) {
			t.Fatal("private catalog material exposed", private)
		}
	}
	if len(network.requests) != 1 || len(network.secrets) != 1 || network.secrets[0] != "selected-token" {
		t.Fatal("wrong credential selection")
	}
	r := network.requests[0]
	if r.Method != "GET" || r.TargetURL != "https://chatgpt.com/backend-api/codex/models?client_version=0.139.0" || r.Headers["Accept"] != "application/json" || r.Headers["ChatGPT-Account-ID"] != "selected-account" || r.Headers["Authorization"] != "" || r.CredentialResource != "codex" || r.Credential == nil || r.Credential.Host != r.TargetURL || r.RequestSHA256 != egress.RequestDigest("GET", r.TargetURL, nil, true) || r.TimeoutMS != 5000 || r.MaxResponseBytes != 1<<20 {
		t.Fatalf("wrong catalog request: %+v", r)
	}
}
func TestCodexCatalogMissingVersionAndMalformedRepliesFailClosed(t *testing.T) {
	for _, body := range []string{`{}`, `{"models":[{"visibility":"list"}]}`, `{"models":[{"slug":"bad\nslug","visibility":"list"}]}`, `not json`} {
		m, _ := codexCatalogManager(t)
		m.SetEgress(&fixtureNetwork{replies: []egress.HTTPResponse{response(200, body)}})
		if _, err := m.Manage(context.Background(), Request{Operation: "provider.connection_models", Arguments: object{"name": "codex"}}); err == nil {
			t.Fatal("accepted malformed account catalog", body)
		}
	}
	m, v := codexCatalogManager(t)
	v.values["codex:codex_oauth_credentials"] = codexPair("selected-token", "selected-account")
	network := &fixtureNetwork{}
	m.SetEgress(network)
	if _, err := m.Manage(context.Background(), Request{Operation: "provider.connection_models", Arguments: object{"name": "codex"}}); err == nil || !strings.Contains(err.Error(), "version") || len(network.requests) != 0 {
		t.Fatal("missing version must fail before egress", err)
	}
}
func TestCodexReadOnlyProbeListsCatalogWithoutInference(t *testing.T) {
	m, _ := codexCatalogManager(t)
	call(t, m, "model.add", object{"args": []string{"seat", "ignored", "dynamic-first", "--registration", "codex"}})
	n := &fixtureNetwork{replies: []egress.HTTPResponse{response(200, accountCatalog)}}
	m.SetEgress(n)
	out := call(t, m, "model.probe", object{"args": []string{"seat", "--no-run"}})
	if !boolean(out, "model_available", false) || boolean(out, "execution_tested", true) || len(n.requests) != 1 || n.requests[0].Method != "GET" {
		t.Fatal(out)
	}
}
func TestCodexSubscriptionSelectsAccountOrderAndPreservesOperatorModel(t *testing.T) {
	m, _ := codexCatalogManager(t)
	n := &fixtureNetwork{replies: []egress.HTTPResponse{response(200, accountCatalog)}}
	m.SetEgress(n)
	call(t, m, "model.register_subscription", object{"vendor": "codex"})
	models := rows(call(t, m, "model.list", nil), "agents")
	if len(models) != 1 || str(models[0], "model") != "dynamic-first" || number(models[0], "context_window") != 90000 {
		t.Fatal(models)
	}
	call(t, m, "model.set", object{"args": []string{"codex", "--model", "operator-choice", "--max-parallel", "7"}})
	m.SetEgress(&fixtureNetwork{err: errors.New("offline")})
	call(t, m, "model.register_subscription", object{"vendor": "codex"})
	models = rows(call(t, m, "model.list", nil), "agents")
	if str(models[0], "model") != "operator-choice" || number(models[0], "max_parallel") != 7 {
		t.Fatal(models)
	}
}

type catalogHookNetwork struct {
	fixtureNetwork
	hook func() error
}

func (n *catalogHookNetwork) Do(ctx context.Context, id uint64, r egress.HTTPRequest) (egress.HTTPResponse, error) {
	if n.hook != nil {
		hook := n.hook
		n.hook = nil
		if err := hook(); err != nil {
			return egress.HTTPResponse{}, err
		}
	}
	return n.fixtureNetwork.Do(ctx, id, r)
}
func TestCodexSubscriptionFencesConcurrentRosterAndAccountChange(t *testing.T) {
	for _, changeAccount := range []bool{false, true} {
		m, v := codexCatalogManager(t)
		network := &catalogHookNetwork{fixtureNetwork: fixtureNetwork{replies: []egress.HTTPResponse{response(200, accountCatalog)}}}
		network.hook = func() error {
			if changeAccount {
				var pair object
				json.Unmarshal([]byte(v.values["codex:codex_oauth_credentials"]), &pair)
				pair["source_digest"] = strings.Repeat("b", 64)
				b, _ := json.Marshal(pair)
				v.values["codex:codex_oauth_credentials"] = string(b)
				return nil
			}
			// Completing this public management call while the catalog GET is in
			// flight proves discovery is outside the roster's exclusive transaction.
			done := make(chan error, 1)
			go func() {
				_, err := m.Manage(context.Background(), Request{Operation: "model.add", Arguments: object{"args": []string{"operator-model", "http://localhost/v1", "operator-choice"}}})
				done <- err
			}()
			select {
			case err := <-done:
				return err
			case <-time.After(time.Second):
				return errors.New("catalog request held roster lock")
			}
		}
		m.SetEgress(network)
		if _, err := m.Manage(context.Background(), Request{Operation: "model.register_subscription", Arguments: object{"vendor": "codex"}}); err == nil {
			t.Fatal("concurrent owner change accepted")
		}
		models := rows(call(t, m, "model.list", nil), "agents")
		if find(models, "codex") != nil {
			t.Fatal("stale default published")
		}
		if !changeAccount && find(models, "operator-model") == nil {
			t.Fatal("operator update lost")
		}
	}
}
func TestCodexSubscriptionNoCatalogDoesNotPublishGuessedDefault(t *testing.T) {
	for _, reply := range []egress.HTTPResponse{response(503, `{"error":"PRIVATE_UPSTREAM"}`), response(200, `{"models":[]}`)} {
		m, _ := codexCatalogManager(t)
		m.SetEgress(&fixtureNetwork{replies: []egress.HTTPResponse{reply}})
		if _, err := m.Manage(context.Background(), Request{Operation: "model.register_subscription", Arguments: object{"vendor": "codex"}}); err == nil || strings.Contains(err.Error(), "PRIVATE_UPSTREAM") {
			t.Fatal("catalog failure hidden/leaked", err)
		}
		if len(rows(call(t, m, "model.list", nil), "agents")) != 0 {
			t.Fatal("guessed default published")
		}
	}
}
func TestCodexCatalogPreservesConfiguredOriginAndQuery(t *testing.T) {
	m, _ := codexCatalogManager(t)
	_, err := m.store.transaction(true, func(root object) (object, error) {
		rows(root, "providers")[0]["models_url"] = "https://selected.example.invalid/account/models?region=one"
		return nil, nil
	})
	if err != nil {
		t.Fatal(err)
	}
	n := &fixtureNetwork{replies: []egress.HTTPResponse{response(200, accountCatalog)}}
	m.SetEgress(n)
	call(t, m, "provider.connection_models", object{"name": "codex"})
	if n.requests[0].TargetURL != "https://selected.example.invalid/account/models?client_version=0.139.0&region=one" || n.requests[0].Credential.Host != n.requests[0].TargetURL {
		t.Fatal("configured origin/query or seal binding changed")
	}
}

func TestCodexEmptyCatalogHasTypedSafeAggregate(t *testing.T) {
	for _, tc := range []struct {
		name, body                      string
		raw, list, hide, missing, other int
	}{
		{"empty", `{"models":[]}`, 0, 0, 0, 0, 0},
		{"hidden", `{"models":[{"slug":"PRIVATE_ONE","visibility":"hide"},{"slug":"PRIVATE_TWO","visibility":"hide"}]}`, 2, 0, 2, 0, 0},
		{"missing-and-other", `{"models":[{"slug":"PRIVATE_ONE"},{"slug":"PRIVATE_TWO","visibility":null},{"slug":"PRIVATE_THREE","visibility":"PRIVATE_VISIBILITY","model_messages":{"instructions_template":"PRIVATE_PROMPT"}}]}`, 3, 0, 0, 1, 2},
	} {
		t.Run(tc.name, func(t *testing.T) {
			m, _ := codexCatalogManager(t)
			m.SetEgress(&fixtureNetwork{replies: []egress.HTTPResponse{response(200, tc.body)}})
			_, err := m.Manage(context.Background(), Request{Operation: "provider.connection_models", Arguments: object{"name": "codex"}})
			var empty interface{ catalogCounts() [5]int }
			if !errors.As(err, &empty) {
				t.Fatalf("want typed empty catalog, got %v", err)
			}
			if got, want := empty.catalogCounts(), [5]int{tc.raw, tc.list, tc.hide, tc.missing, tc.other}; got != want {
				t.Fatalf("wrong aggregates: %v want %v", got, want)
			}
			for _, private := range []string{"PRIVATE_", "selected-token", "selected-account", "source_digest"} {
				if strings.Contains(err.Error(), private) {
					t.Fatal("private response leaked")
				}
			}
		})
	}
}
func TestCodexDeclaredCompatibilityMatchesQualifiedInstallerAndCI(t *testing.T) {
	_, file, _, ok := runtime.Caller(0)
	if !ok {
		t.Fatal("test source unavailable")
	}
	root := filepath.Clean(filepath.Join(filepath.Dir(file), "../../.."))
	header, err := os.ReadFile(filepath.Join(root, "src/headers/codex_client_version.h"))
	if err != nil {
		t.Fatal(err)
	}
	declared := regexp.MustCompile(`AIMEE_CODEX_CLIENT_VERSION "([0-9]+\.[0-9]+\.[0-9]+)"`).FindSubmatch(header)
	if len(declared) != 2 {
		t.Fatal("declared adapter release missing")
	}
	ci, err := os.ReadFile(filepath.Join(root, ".github/workflows/ci.yml"))
	if err != nil {
		t.Fatal(err)
	}
	pins := regexp.MustCompile(`@openai/codex@([0-9]+\.[0-9]+\.[0-9]+)`).FindAllSubmatch(ci, -1)
	if len(pins) == 0 {
		t.Fatal("actual SDK qualification pin missing")
	}
	for _, pin := range pins {
		if string(pin[1]) != string(declared[1]) {
			t.Fatalf("declared/installer compatibility %s differs from qualified SDK %s", declared[1], pin[1])
		}
	}
	for _, rel := range []string{"src/server/server_cli_oauth.c", "src/modules/vault/vault_env_bootstrap.c"} {
		src, err := os.ReadFile(filepath.Join(root, rel))
		if err != nil {
			t.Fatal(err)
		}
		if !strings.Contains(string(src), "AIMEE_CODEX_CLIENT_VERSION") {
			t.Fatalf("shared release is not used by %s", rel)
		}
	}
}

func TestCodexEmptyCatalogAggregatesAllPages(t *testing.T) {
	m, _ := codexCatalogManager(t)
	m.SetEgress(&fixtureNetwork{replies: []egress.HTTPResponse{
		response(200, `{"models":[{"slug":"PRIVATE_FIRST","visibility":"hide"}],"has_more":true,"last_id":"PRIVATE_CURSOR"}`),
		response(200, `{"models":[{"slug":"PRIVATE_LAST"}]}`),
	}})
	_, err := m.Manage(context.Background(), Request{Operation: "provider.connection_models", Arguments: object{"name": "codex"}})
	var empty interface{ catalogCounts() [5]int }
	if !errors.As(err, &empty) || empty.catalogCounts() != [5]int{2, 0, 1, 1, 0} || strings.Contains(err.Error(), "PRIVATE_") {
		t.Fatalf("invalid aggregate: %v", err)
	}
}
func TestNonCodexEmptyCatalogContractIsUnchanged(t *testing.T) {
	m, _ := codexCatalogManager(t)
	m.SetEgress(&fixtureNetwork{replies: []egress.HTTPResponse{response(200, `{"data":[]}`)}})
	out, err := m.discover(context.Background(), object{"name": "public", "provider": "openai", "auth_type": "none", "endpoint": "https://selected.example.invalid/v1"})
	if err != nil || number(out, "count") != 0 {
		t.Fatalf("generic empty catalog changed: %v %v", out, err)
	}
}
