package engine

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"net"
	"net/http"
	"time"
)

type HTTPRunnerConfig struct {
	Endpoint   string
	UnixSocket string
	Timeout    time.Duration
}

type HTTPRunner struct {
	endpoint string
	client   *http.Client
}

func NewHTTPRunner(cfg HTTPRunnerConfig) (*HTTPRunner, error) {
	if cfg.Endpoint == "" {
		return nil, errors.New("runner endpoint is required")
	}
	transport := http.DefaultTransport.(*http.Transport).Clone()
	if cfg.UnixSocket != "" {
		socket := cfg.UnixSocket
		transport.DialContext = func(ctx context.Context, _, _ string) (net.Conn, error) {
			return (&net.Dialer{}).DialContext(ctx, "unix", socket)
		}
	}
	if cfg.Timeout == 0 {
		cfg.Timeout = 30 * time.Minute
	}
	return &HTTPRunner{endpoint: cfg.Endpoint, client: &http.Client{
		Transport: transport,
		Timeout:   cfg.Timeout,
	}}, nil
}

func (r *HTTPRunner) Run(ctx context.Context, request StepRequest) (StepResult, error) {
	// json.Marshal is intentionally not preceded by any size check. The process
	// boundary receives either the complete typed request or an explicit error.
	body, err := json.Marshal(request)
	if err != nil {
		return StepResult{}, fmt.Errorf("encode runner request: %w", err)
	}
	httpRequest, err := http.NewRequestWithContext(ctx, http.MethodPost, r.endpoint,
		bytes.NewReader(body))
	if err != nil {
		return StepResult{}, fmt.Errorf("create runner request: %w", err)
	}
	httpRequest.Header.Set("Content-Type", "application/json")
	response, err := r.client.Do(httpRequest)
	if err != nil {
		return StepResult{}, fmt.Errorf("call runner: %w", err)
	}
	defer response.Body.Close()
	if response.StatusCode < 200 || response.StatusCode >= 300 {
		var envelope struct {
			Error string `json:"error"`
		}
		if err := json.NewDecoder(response.Body).Decode(&envelope); err != nil || envelope.Error == "" {
			envelope.Error = response.Status
		}
		return StepResult{}, fmt.Errorf("runner rejected request: %s", envelope.Error)
	}
	var result StepResult
	if err := json.NewDecoder(response.Body).Decode(&result); err != nil {
		return StepResult{}, fmt.Errorf("decode runner response: %w", err)
	}
	return result, nil
}
