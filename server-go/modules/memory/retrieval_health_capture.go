package memory

import (
	"context"
	"encoding/json"
	"os"
	"time"

	"github.com/JBailes/aimee/server-go/bus"
	store "github.com/JBailes/aimee/server-go/db"
)

type healthQueryContext struct {
	Namespace   string `json:"namespace"`
	Project     string `json:"project"`
	Workspace   string `json:"workspace"`
	Fingerprint string `json:"fingerprint"`
	Source      string `json:"source"`
}
type healthKeyCacheEntry struct {
	Namespace string
	Key       []byte
	Expires   time.Time
}

// Optional capture has a short independent deadline and never refuses serving.
// The key is persistent in the existing private store. A bounded process cache
// avoids a database transaction on every provider turn. Raw queries do not enter
// the journal, receipt metadata, metrics or public health response.
func captureHealthQuery(options handlerOptions, invocation bus.ModuleInvocation, args commandArgs, query, source string) *healthQueryContext {
	if os.Getenv("AIMEE_MEMORY_HEALTH_ENABLED") != "1" || options.placement != PlacementServer || invocation.PrincipalRef != 0 || query == "" || len(query) > 4096 {
		return nil
	}
	principal := args.stringOr("principal", "")
	if principal == "" || len(principal) > 128 {
		return nil
	}
	if caller := options.commandContext; caller != nil && (!caller.Authenticated || caller.Principal != principal) {
		return nil
	}
	s, ok := options.data.(*postgresDataStore)
	if !ok || s.health == nil {
		return nil
	}
	project, workspace := args.stringOr("project", ""), args.stringOr("workspace", "")
	if len(project) > 1024 || len(workspace) > 1024 {
		return nil
	}
	key := releaseDigest([]string{principal, project, workspace})
	now := time.Now().UTC()
	s.health.cacheMu.Lock()
	cached, ok := s.health.keys[key]
	s.health.cacheMu.Unlock()
	if !ok || !now.Before(cached.Expires) {
		budget := invocation.Remaining(50 * time.Millisecond)
		if budget <= 0 {
			return nil
		}
		parent := options.dataContext
		if parent == nil {
			parent = context.Background()
		}
		ctx, cancel := context.WithTimeout(parent, budget)
		defer cancel()
		journal, err := s.readHealthStore(ctx, principal, project, workspace)
		if store.IsNoRows(err) {
			journal, err = s.updateHealthStore(ctx, principal, project, workspace, now, func(*healthJournal) error { return nil })
		}
		if err != nil {
			return nil
		}
		cached = healthKeyCacheEntry{Namespace: journal.Namespace, Key: append([]byte(nil), journal.Key...), Expires: now.Add(time.Minute)}
		s.health.cacheMu.Lock()
		if s.health.keys == nil {
			s.health.keys = map[string]healthKeyCacheEntry{}
		}
		for k, entry := range s.health.keys {
			if !now.Before(entry.Expires) {
				delete(s.health.keys, k)
			}
		}
		if len(s.health.keys) < 128 {
			s.health.keys[key] = cached
		}
		s.health.cacheMu.Unlock()
	}
	return &healthQueryContext{Namespace: cached.Namespace, Project: project, Workspace: workspace, Fingerprint: healthQueryFingerprint(cached.Key, cached.Namespace, query), Source: source}
}

func prepareHealthQueryCapture(options handlerOptions, invocation bus.ModuleInvocation, args commandArgs, operation string) {
	// The only producer of this internal field is the authenticated Go owner.
	delete(args, "_health_context")
	query, source := "", ""
	if operation == "ingress-assemble" && options.gateway != nil {
		query, source = options.gateway.releases.consumeHealthQuery(args.stringOr("_health_query_token", "")), "ingress_query"
		delete(args, "_health_query_token")
	}
	if operation == "native-source-release" && options.gateway != nil {
		var p nativeRecallProjection
		if json.Unmarshal(args["native_projection"], &p) == nil {
			query, source = options.gateway.releases.consumeHealthQuery(p.HealthQueryToken), "native_task_hint"
		}
	}
	if capture := captureHealthQuery(options, invocation, args, query, source); capture != nil {
		args["_health_context"], _ = json.Marshal(capture)
	}
}

// A native projection transports only an opaque, short-lived token. This avoids
// echoing the task hint into memory output or exposing it to output screening.
// The actual query stays in this bounded owner cache until the source-release
// call supplies the authenticated principal and exact active scope.
type healthPendingQuery struct {
	Text    string
	Expires time.Time
}

func (s *sourceReleaseState) captureHealthQueryToken(args commandArgs) {
	delete(args, "_health_query_token")
	if os.Getenv("AIMEE_MEMORY_HEALTH_ENABLED") != "1" {
		return
	}
	if _, ok := args["native_context_bytes"]; !ok {
		return
	}
	query := args.stringOr("task_hint", "")
	if query == "" || len(query) > 4096 {
		return
	}
	token, err := releaseToken()
	if err != nil {
		return
	}
	now := time.Now()
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.healthQueries == nil {
		s.healthQueries = map[string]healthPendingQuery{}
	}
	for key, entry := range s.healthQueries {
		if !now.Before(entry.Expires) {
			delete(s.healthQueries, key)
		}
	}
	if len(s.healthQueries) >= 64 {
		return
	}
	s.healthQueries[token] = healthPendingQuery{Text: query, Expires: now.Add(time.Minute)}
	args["_health_query_token"], _ = json.Marshal(token)
}
func (s *sourceReleaseState) consumeHealthQuery(token string) string {
	if !releaseTokenValid(token) {
		return ""
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	entry, ok := s.healthQueries[token]
	delete(s.healthQueries, token)
	if !ok || !time.Now().Before(entry.Expires) {
		return ""
	}
	return entry.Text
}

func sourceReleaseArgsForHealth(query string) commandArgs {
	raw, _ := json.Marshal(query)
	return commandArgs{"task_hint": raw, "native_context_bytes": json.RawMessage(`0`)}
}
