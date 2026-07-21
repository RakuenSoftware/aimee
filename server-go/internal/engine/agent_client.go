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
	"net/url"
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
	RetryTag         string
	// acceptPartial is reserved for native branch-producing blocks whose worktree output
	// is independently committed and verified by the Go native runner. Structured
	// and prose blocks must receive a complete resource-plane result.
	acceptPartial bool
}

type DelegateResult struct {
	Response string
	Agent    string
	CostUSD  float64
	Partial  bool
}

type AgentClient interface {
	Delegate(context.Context, DelegateRequest) (DelegateResult, error)
}

type EligibleAgent struct {
	Name        string
	Provider    string
	MaxParallel int
}

// AgentRosterClient is implemented by resource planes that can expose the live
// enabled roster. WFE uses it to fill every eligible roundtable slot; it never
// persists provider failures as exclusions. Production binds the roster endpoint
// to the same authenticated Unix socket (or explicitly authenticated transport)
// used by delegate execution; the resource plane owns tenant filtering and exact,
// case-sensitive role names. BaseURL remains supported for loopback tests and
// authenticated remote resource-plane deployments.
type AgentRosterClient interface {
	EligibleAgents(context.Context, string) ([]EligibleAgent, error)
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
	parsedBase, err := url.Parse(cfg.BaseURL)
	if err != nil {
		return nil, fmt.Errorf("parse agent service URL: %w", err)
	}
	host := parsedBase.Hostname()
	loopback := host == "localhost"
	if ip := net.ParseIP(host); ip != nil && ip.IsLoopback() {
		loopback = true
	}
	if cfg.UnixSocket == "" && cfg.Bearer == "" && !loopback {
		return nil, errors.New("remote agent service requires an authenticated bearer or Unix socket")
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
	key := delegateJobKey(request)
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
		case "partial":
			if strings.TrimSpace(status.Result) == "" {
				if c.store != nil {
					_ = c.store.ForgetDelegateJob(context.WithoutCancel(ctx), key)
				}
				return DelegateResult{}, fmt.Errorf("delegate job %d returned an empty partial result", launched.JobID)
			}
			if !request.acceptPartial {
				if c.store != nil {
					_ = c.store.ForgetDelegateJob(context.WithoutCancel(ctx), key)
				}
				return DelegateResult{}, fmt.Errorf("delegate job %d returned a partial result for a block that requires completion", launched.JobID)
			}
			// A delegate can leave a complete, independently verifiable artifact and
			// still be labelled partial when its final synthesis fails. Preserve that
			// artifact and its terminal durable mapping: an identical retry must replay
			// the same artifact idempotently, not launch overlapping work. Callers that
			// need corrective synthesis change the prompt, which changes the job key.
			// The native branch-producing block validates its own output contract.
			return DelegateResult{Response: status.Result, Agent: status.Agent, CostUSD: status.CostUSD, Partial: true}, nil
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

func delegateJobKey(request DelegateRequest) string {
	keyMaterial, _ := json.Marshal(request)
	return fmt.Sprintf("%s:%s:%s:%x", request.WorkItemID, request.Stage, request.ExecutionVersion, sha256.Sum256(keyMaterial))
}

func (c *HTTPAgentClient) EligibleAgents(ctx context.Context, role string) ([]EligibleAgent, error) {
	role = strings.TrimSpace(role)
	if role == "" {
		return nil, errors.New("eligible-agent role is required")
	}
	var response struct {
		Agents []struct {
			Name        string   `json:"name"`
			Provider    string   `json:"provider"`
			Enabled     bool     `json:"enabled"`
			PrimaryOnly bool     `json:"primary_only"`
			MaxParallel int      `json:"max_parallel"`
			Roles       []string `json:"roles"`
		} `json:"agents"`
	}
	if err := c.doJSON(ctx, http.MethodGet, "/v1/agent/list", nil, &response); err != nil {
		return nil, err
	}
	var eligible []EligibleAgent
	for _, agent := range response.Agents {
		agent.Name = strings.TrimSpace(agent.Name)
		if !agent.Enabled || agent.PrimaryOnly || agent.Name == "" || agent.MaxParallel < 1 {
			continue
		}
		roleOK := false
		for _, allowed := range agent.Roles {
			allowed = strings.TrimSpace(allowed)
			if allowed == "" {
				continue
			}
			if allowed == "all" || allowed == role {
				roleOK = true
				break
			}
		}
		if roleOK {
			eligible = append(eligible, EligibleAgent{Name: agent.Name, Provider: agent.Provider, MaxParallel: agent.MaxParallel})
		}
	}
	return eligible, nil
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
