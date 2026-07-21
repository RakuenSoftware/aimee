package engine

import (
	"bytes"
	"context"
	"crypto/sha256"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net"
	"net/http"
	"strconv"
	"strings"
	"time"

	"github.com/JBailes/aimee/server-go/internal/db1"
)

type DelegateRequest struct {
	Role             string
	Persona          string
	Delegate         string
	Prompt           string
	Workdir          string
	Tools            bool
	WorkItemID       string
	Stage            string
	ExecutionVersion string
}

type DelegateResult struct {
	Response string
	Agent    string
	CostUSD  float64
}

type AgentClient interface {
	Delegate(context.Context, DelegateRequest) (DelegateResult, error)
}

type AgentHTTPConfig struct {
	BaseURL        string
	UnixSocket     string
	Bearer         string
	PollEvery      time.Duration
	RequestTimeout time.Duration
	Store          *db1.Store
}

// HTTPAgentClient talks to the agent service as a resource plane. It owns no
// workflow state or transitions; losing it parks the Go-owned run and a later
// scheduler pass can retry safely.
type HTTPAgentClient struct {
	baseURL, bearer string
	client          *http.Client
	pollEvery       time.Duration
	store           *db1.Store
}

func NewHTTPAgentClient(cfg AgentHTTPConfig) (*HTTPAgentClient, error) {
	if cfg.BaseURL == "" {
		cfg.BaseURL = "http://aimee"
	}
	transport := http.DefaultTransport.(*http.Transport).Clone()
	if cfg.UnixSocket != "" {
		socket := cfg.UnixSocket
		transport.DialContext = func(ctx context.Context, _, _ string) (net.Conn, error) {
			return (&net.Dialer{}).DialContext(ctx, "unix", socket)
		}
	}
	if cfg.PollEvery <= 0 {
		cfg.PollEvery = time.Second
	}
	if cfg.RequestTimeout <= 0 {
		cfg.RequestTimeout = 30 * time.Second
	}
	return &HTTPAgentClient{baseURL: strings.TrimRight(cfg.BaseURL, "/"), bearer: cfg.Bearer,
		client: &http.Client{Transport: transport, Timeout: cfg.RequestTimeout}, pollEvery: cfg.PollEvery, store: cfg.Store}, nil
}

func (c *HTTPAgentClient) Delegate(ctx context.Context, request DelegateRequest) (DelegateResult, error) {
	if request.Role == "" || request.Persona == "" || request.Prompt == "" {
		return DelegateResult{}, errors.New("delegate role, persona, and prompt are required")
	}
	payload := map[string]any{"role": request.Role, "persona": request.Persona, "prompt": request.Prompt, "cwd": request.Workdir, "tools": request.Tools}
	// Empty means ordinary eligibility routing. A non-empty delegate is an
	// explicit positive pin from the workflow, never an exclusion list.
	if request.Delegate != "" {
		payload["via"] = request.Delegate
	}
	keyMaterial, _ := json.Marshal(request)
	key := fmt.Sprintf("%s:%s:%s:%x", request.WorkItemID, request.Stage, request.ExecutionVersion, sha256.Sum256(keyMaterial))
	var launched struct {
		JobID int    `json:"job_id"`
		Error string `json:"error"`
	}
	if c.store != nil && request.WorkItemID != "" {
		if existing, err := c.store.DelegateJob(ctx, key); err == nil {
			launched.JobID = existing
		}
	}
	if launched.JobID == 0 {
		if err := c.doJSONKey(ctx, http.MethodPost, "/v1/delegate/run", payload, &launched, key); err != nil {
			return DelegateResult{}, err
		}
		if c.store != nil && request.WorkItemID != "" {
			if err := c.store.SaveDelegateJob(ctx, key, launched.JobID); err != nil {
				return DelegateResult{}, err
			}
		}
	}
	if launched.JobID <= 0 {
		return DelegateResult{}, fmt.Errorf("agent service returned no job id: %s", launched.Error)
	}
	ticker := time.NewTicker(c.pollEvery)
	defer ticker.Stop()
	for {
		var status struct {
			JobStatus string  `json:"job_status"`
			Result    string  `json:"result"`
			Agent     string  `json:"agent_name"`
			CostUSD   float64 `json:"cost_usd"`
			Error     string  `json:"error"`
		}
		if err := c.doJSON(ctx, http.MethodPost, "/v1/delegate/status", map[string]any{"job_id": launched.JobID, "full_result": true, "result_limit": -1}, &status); err != nil {
			if ctx.Err() != nil {
				c.cancelRemote(launched.JobID, ctx)
				return DelegateResult{}, ctx.Err()
			}
			return DelegateResult{}, err
		}
		switch status.JobStatus {
		case "done":
			return DelegateResult{Response: status.Result, Agent: status.Agent, CostUSD: status.CostUSD}, nil
		case "failed", "cancelled", "stopped", "invalid", "not_found":
			if c.store != nil {
				_ = c.store.ForgetDelegateJob(context.WithoutCancel(ctx), key)
			}
			if status.Result != "" {
				return DelegateResult{}, errors.New(status.Result)
			}
			return DelegateResult{}, fmt.Errorf("delegate job %d %s", launched.JobID, status.JobStatus)
		}
		select {
		case <-ctx.Done():
			// The remote resource-plane job outlives an HTTP poll unless it is
			// explicitly cancelled. Wait for the cancellation acknowledgement so
			// a wall-cap resume cannot overlap the old job in the same worktree.
			c.cancelRemote(launched.JobID, ctx)
			return DelegateResult{}, ctx.Err()
		case <-ticker.C:
		}
	}
}

func (c *HTTPAgentClient) cancelRemote(jobID int, parent context.Context) {
	cancelCtx, cancel := context.WithTimeout(context.WithoutCancel(parent), 10*time.Second)
	defer cancel()
	var ignored map[string]any
	_ = c.doJSON(cancelCtx, http.MethodPost, "/v1/job/cancel",
		map[string]any{"job_id": jobID, "reason": "WFE turn cancelled"}, &ignored)
}

func (c *HTTPAgentClient) doJSON(ctx context.Context, method, path string, input, output any) error {
	return c.doJSONKey(ctx, method, path, input, output, "")
}
func (c *HTTPAgentClient) doJSONKey(ctx context.Context, method, path string, input, output any, idempotencyKey string) error {
	var body io.Reader
	if input != nil {
		encoded, err := json.Marshal(input)
		if err != nil {
			return err
		}
		body = bytes.NewReader(encoded)
	}
	req, err := http.NewRequestWithContext(ctx, method, c.baseURL+path, body)
	if err != nil {
		return err
	}
	req.Header.Set("Content-Type", "application/json")
	if idempotencyKey != "" {
		req.Header.Set("Idempotency-Key", idempotencyKey)
	}
	if c.bearer != "" {
		req.Header.Set("Authorization", "Bearer "+c.bearer)
	}
	resp, err := c.client.Do(req)
	if err != nil {
		return err
	}
	defer resp.Body.Close()
	if resp.StatusCode < 200 || resp.StatusCode >= 300 {
		data, _ := io.ReadAll(resp.Body)
		return fmt.Errorf("agent service %s: %s", strconv.Itoa(resp.StatusCode), strings.TrimSpace(string(data)))
	}
	if err := json.NewDecoder(resp.Body).Decode(output); err != nil {
		return fmt.Errorf("decode agent response: %w", err)
	}
	return nil
}
