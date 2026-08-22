package api

import (
	"bytes"
	"context"
	"errors"
	"net/http"
	"net/http/httptest"
	"testing"
	"time"

	appconfig "github.com/JBailes/aimee/server-go/config"
)

type unavailableConfigCaller struct{}

func (unavailableConfigCaller) Call(context.Context, uint32, uint32, uint64, time.Duration,
	[]byte) ([]byte, error) {
	return nil, errors.New("module capacity exhausted")
}

func TestConfigTransportFailuresAreServiceUnavailable(t *testing.T) {
	server, _, _ := newTestServer(t)
	client, err := appconfig.NewClient(unavailableConfigCaller{}, time.Second)
	if err != nil {
		t.Fatal(err)
	}
	server.SetConfigStore(client)

	get := httptest.NewRecorder()
	server.ServeHTTP(get, httptest.NewRequest(http.MethodGet, "/v1/config", nil))
	if get.Code != http.StatusServiceUnavailable {
		t.Fatalf("GET status = %d, body=%s", get.Code, get.Body.String())
	}

	request := httptest.NewRequest(http.MethodPost, "/v1/config/set",
		bytes.NewBufferString(`{"key":"autonomy.max_turns","value":301}`))
	setWorkflowIdentity(request, "operator", true)
	set := httptest.NewRecorder()
	server.ServeHTTP(set, request)
	if set.Code != http.StatusServiceUnavailable {
		t.Fatalf("POST status = %d, body=%s", set.Code, set.Body.String())
	}
}
