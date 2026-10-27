package api

import (
	"context"
	"encoding/json"
	"errors"
	"github.com/JBailes/aimee/server-go/delegate"
	"github.com/JBailes/aimee/server-go/internal/workflowstore"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
)

func TestManualSubmissionRejectsBodyExecutionScopeOverride(t *testing.T) {
	s := &Server{}
	request := httptest.NewRequest(http.MethodPost, "/v1/dev/submit", strings.NewReader(`{"proposal_md":"work","repo":"/repo","execution_scope":"`+strings.Repeat("b", 64)+`"}`))
	request = request.WithContext(delegate.WithExecutionScope(request.Context(), strings.Repeat("a", 64)))
	response := httptest.NewRecorder()
	s.devSubmit(response, request)
	if response.Code != http.StatusBadRequest || !strings.Contains(response.Body.String(), "unknown field") {
		t.Fatalf("public body scope override reached admission: %d %s", response.Code, response.Body.String())
	}
}
func TestScheduledTriggerRequiresRegisteredHostScope(t *testing.T) {
	s := &Server{}
	registered := triggerFireRequest{Origin: "config", Source: "watch-dir", Workspace: "/approved", Pipeline: "build", Event: "docs/proposals/pending", Mode: "autonomous", MaxSpend: 2}
	if _, err := s.triggerExecutionContext(context.WithValue(context.Background(), registeredTriggerContextKey{}, registeredTriggerContext{triggerRegistration(registered)})); err == nil {
		t.Fatal("unowned scheduled trigger acquired source authority")
	}
	scope := strings.Repeat("a", 64)
	calls := 0
	s.SetTriggerExecutionScopeResolver(func(_ context.Context, request TriggerExecutionScopeRequest) (string, error) {
		calls++
		if request.Registration.Workspace != "/approved" || request.Registration.Origin != "config" || len(request.ConfigurationSHA256) != 64 {
			t.Fatal("wrong registered descriptor binding")
		}
		return scope, nil
	})
	ctx, err := s.triggerExecutionContext(context.WithValue(context.Background(), registeredTriggerContextKey{}, registeredTriggerContext{triggerRegistration(registered)}))
	if err != nil || delegate.ExecutionScopeFromContext(ctx) != scope || calls != 1 {
		t.Fatalf("host source not propagated: %v calls%d", err, calls)
	}
	// A public origin value is never a private registered-source marker.
	registered.Origin = "config"
	manual := delegate.WithExecutionScope(context.Background(), scope)
	got, err := s.triggerExecutionContext(manual)
	if err != nil || delegate.ExecutionScopeFromContext(got) != scope || calls != 1 {
		t.Fatal("manual origin invoked service issuer")
	}
	unowned, err := s.triggerExecutionContext(context.Background())
	if err != nil || delegate.ExecutionScopeFromContext(unowned) != "" || calls != 1 {
		t.Fatal("unowned manual request acquired service issuer")
	}
	registered.Origin = "config"
	s.SetTriggerExecutionScopeResolver(func(context.Context, TriggerExecutionScopeRequest) (string, error) {
		return "", errors.New("private diagnostic")
	})
	if _, err := s.triggerExecutionContext(context.WithValue(ctx, registeredTriggerContextKey{}, registeredTriggerContext{triggerRegistration(registered)})); err == nil || strings.Contains(err.Error(), "private diagnostic") {
		t.Fatal("resolver failure acquired source or leaked private details")
	}
}
func TestWorkflowScopeIsNotPublicWorkItemJSON(t *testing.T) {
	raw, err := json.Marshal(workflowstore.WorkItem{ID: "item", ExecutionScope: strings.Repeat("a", 64)})
	if err != nil || strings.Contains(string(raw), "execution_scope") || strings.Contains(string(raw), strings.Repeat("a", 64)) {
		t.Fatal("authorization scope leaked through public workitem JSON")
	}
}

func TestManualTriggerCannotForgeRegisteredSourceOrigin(t *testing.T) {
	server := &Server{}
	calls := 0
	server.SetTriggerExecutionScopeResolver(func(context.Context, TriggerExecutionScopeRequest) (string, error) {
		calls++
		return strings.Repeat("a", 64), nil
	})
	request := httptest.NewRequest(http.MethodPost, "/v1/trigger/fire", strings.NewReader(`{"source":"watch-dir","workspace":"/approved","proposal":"candidate","origin":"config"}`))
	recorder := httptest.NewRecorder()
	server.triggerFire(recorder, request)
	if recorder.Code != http.StatusBadRequest || calls != 0 || !strings.Contains(recorder.Body.String(), "unknown field") {
		t.Fatalf("public origin reached issuer: code=%d calls=%d", recorder.Code, calls)
	}
}
