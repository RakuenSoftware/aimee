// Package config is the caller-side contract for the config module.
//
// It contains no storage implementation. Every read and mutation is a bounded
// request to the independently running Go config module over Aimee's event bus.
package config

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"math"
	"strings"
	"sync"
	"time"

	"github.com/RakuenSoftware/ouroboros-ring/go/ring"
)

const (
	// Carved from config's permanent principal ref 2:
	// 4096 + 2*256 + stage ordinal 1.
	EventConfig uint32 = 4609
	StageConfig uint32 = 1
	// EventConfigChanged is the module's config-changed notification: 4096 +
	// 2*256 + 2, published after every committed change with the new document
	// version. A subscribed client caches instead of asking again.
	EventConfigChanged uint32 = 4610

	MaxTriggerRules            = 32
	MinAutonomyMaxWallSecs     = 360
	DefaultDeadline            = 5 * time.Second
	defaultMaxResponseBodySize = 16 * 1024 * 1024
)

type TriggerRule struct {
	Source   string          `yaml:"source" json:"source"`
	Event    string          `yaml:"event,omitempty" json:"event"`
	Schedule string          `yaml:"schedule,omitempty" json:"schedule"`
	Mode     string          `yaml:"mode,omitempty" json:"mode"`
	Pipeline TriggerPipeline `yaml:"pipeline" json:"pipeline"`
}

type TriggerPipeline struct {
	Template    string  `yaml:"template" json:"template"`
	Workspace   string  `yaml:"workspace" json:"workspace"`
	MaxSpendUSD float64 `yaml:"max_spend_usd,omitempty" json:"max_spend_usd,omitempty"`
}

type Operation string

const (
	OpValues                 Operation = "values"
	OpSnapshot               Operation = "snapshot"
	OpValue                  Operation = "value"
	OpStringValue            Operation = "string-value"
	OpSetVersioned           Operation = "set-versioned"
	OpVersion                Operation = "version"
	OpTriggerRules           Operation = "trigger-rules"
	OpWorkspaceAdd           Operation = "workspace-add"
	OpWorkspaceRemove        Operation = "workspace-remove"
	OpApplyRoundtablePreset  Operation = "apply-roundtable-preset"
	OpPersistDefaults        Operation = "persist-defaults"
	OpSetTypedFacts          Operation = "set-typed-facts"
	OpSetAPIHTTPListener     Operation = "set-api-http-listener"
	OpSetModelConcurrency    Operation = "set-model-concurrency"
	OpRemoveModelConcurrency Operation = "remove-model-concurrency"
)

type Request struct {
	Operation       Operation `json:"operation"`
	Key             string    `json:"key,omitempty"`
	Value           any       `json:"value,omitempty"`
	PreviousVersion string    `json:"previous_version,omitempty"`
}

type TypedFactsMutation struct {
	Enabled          *bool `json:"enabled,omitempty"`
	AutoPromote      *bool `json:"auto_promote,omitempty"`
	PromoteThreshold *int  `json:"promote_threshold,omitempty"`
}

type APIHTTPListenerMutation struct {
	HTTPPort        int `json:"http_port"`
	RateLimitPerMin int `json:"rate_limit_per_min"`
}

type ModelConcurrencyMutation struct {
	Model string `json:"model"`
	Limit int    `json:"limit,omitempty"`
}

type Response struct {
	OK      bool           `json:"ok"`
	Code    string         `json:"code,omitempty"`
	Error   string         `json:"error,omitempty"`
	Value   any            `json:"value,omitempty"`
	Values  map[string]any `json:"values,omitempty"`
	Version string         `json:"version,omitempty"`
	Rules   []TriggerRule  `json:"rules,omitempty"`
}

var (
	ErrConfig      = errors.New("config bus client is not configured")
	ErrMalformed   = errors.New("config module returned a malformed response")
	ErrUnavailable = errors.New("config module unavailable")
)

type ModuleError struct {
	Code    string
	Message string
}

func (e *ModuleError) Error() string { return e.Message }

type StageCaller interface {
	Call(context.Context, uint32, uint32, uint64, time.Duration, []byte) ([]byte, error)
}

// Service is the caller-side capability consumed by the WFE. Implementations
// may be bus clients or test doubles; storage is deliberately absent from this
// package and from the interface.
type Service interface {
	Values() (map[string]any, error)
	SetVersioned(string, any, string) error
	Version(string) (string, error)
	TriggerRules() ([]TriggerRule, error)
	Int(string, int) int
	IntValue(string) (int, bool, error)
	BoolValue(string) (bool, bool, error)
}

type Client struct {
	caller   StageCaller
	deadline time.Duration
	cache    replyCache
}

// NotificationSource delivers a slot's subscribed notifications and tells it
// what it missed. ring.ConcurrentModuleCaller is one.
type NotificationSource interface {
	OnNotification(kind uint32, fn func(payload []byte))
	OnOverflow(kind uint32, fn func())
}

// cacheable operations answer from the document alone, so a reply stays right
// until the document version changes.
var cacheable = map[Operation]bool{
	OpValues: true, OpSnapshot: true, OpValue: true, OpStringValue: true,
	OpVersion: true, OpTriggerRules: true,
}

// replyCache holds the module's raw replies to read operations while the client
// is subscribed to config-changed. Raw bytes are decoded again for each caller,
// so no caller can alter what another reads. A reply is only stored if no
// change arrived while it was in flight (the generation is unchanged), so a
// read that raced a change is never kept.
type replyCache struct {
	mu        sync.Mutex
	enabled   bool
	gen       uint64
	version   string
	replies   map[string][]byte
	listeners []func()
}

func cacheKey(request Request) string { return string(request.Operation) + "\x00" + request.Key }

func (r *replyCache) lookup(key string) ([]byte, uint64, bool) {
	r.mu.Lock()
	defer r.mu.Unlock()
	reply, ok := r.replies[key]
	return reply, r.gen, ok && r.enabled
}

func (r *replyCache) store(key string, reply []byte, gen uint64) {
	r.mu.Lock()
	defer r.mu.Unlock()
	if r.enabled && r.gen == gen {
		if r.replies == nil {
			r.replies = map[string][]byte{}
		}
		r.replies[key] = reply
	}
}

// invalidate drops every cached reply and tells the listeners. version is the
// one announced; an announcement of the version already known (a duplicate,
// or the client's own write echoed back) changes nothing. An empty version is
// a missed notification, which always invalidates.
func (r *replyCache) invalidate(version string) {
	r.mu.Lock()
	if version != "" && version == r.version {
		r.mu.Unlock()
		return
	}
	if version != "" {
		r.version = version
	}
	r.gen++
	clear(r.replies)
	listeners := append([]func(){}, r.listeners...)
	r.mu.Unlock()
	for _, fn := range listeners {
		fn()
	}
}

// Subscribe has the client cache what it reads and drop the cache whenever the
// module announces a new version, or the ring reports a missed announcement.
// Without it every read is a bus call, as before.
func (c *Client) Subscribe(source NotificationSource) {
	if c == nil || source == nil {
		return
	}
	source.OnNotification(EventConfigChanged, func(payload []byte) {
		var changed struct {
			Version string `json:"version"`
		}
		if json.Unmarshal(payload, &changed) != nil {
			changed.Version = "" // unreadable: treat as missed
		}
		c.cache.invalidate(changed.Version)
	})
	source.OnOverflow(EventConfigChanged, func() { c.cache.invalidate("") })
	c.cache.mu.Lock()
	c.cache.enabled = true
	c.cache.mu.Unlock()
}

// OnChange has fn called after the configuration changes, once Subscribe is in
// effect. It runs on the notification goroutine and must not block.
func (c *Client) OnChange(fn func()) {
	if c == nil || fn == nil {
		return
	}
	c.cache.mu.Lock()
	defer c.cache.mu.Unlock()
	c.cache.listeners = append(c.cache.listeners, fn)
}

func NewClient(caller StageCaller, deadline time.Duration) (*Client, error) {
	if caller == nil {
		return nil, ErrConfig
	}
	if deadline <= 0 {
		deadline = DefaultDeadline
	}
	return &Client{caller: caller, deadline: deadline}, nil
}

func (c *Client) request(request Request) (Response, error) {
	if c == nil || c.caller == nil {
		return Response{}, ErrConfig
	}
	key := cacheKey(request)
	if cacheable[request.Operation] {
		if reply, _, ok := c.cache.lookup(key); ok {
			return decodeReply(reply)
		}
	}
	_, gen, _ := c.cache.lookup(key)
	body, err := json.Marshal(request)
	if err != nil {
		return Response{}, err
	}
	reply, err := c.caller.Call(context.Background(), EventConfig, StageConfig, 0, c.deadline, body)
	if err != nil {
		return Response{}, fmt.Errorf("%w: %w", ErrUnavailable, err)
	}
	response, err := decodeReply(reply)
	if cacheable[request.Operation] {
		// A typed refusal (an unknown key) is as much an answer as a value, so
		// it is kept too; only a reply that failed to decode is not.
		var moduleErr *ModuleError
		if err == nil || errors.As(err, &moduleErr) {
			c.cache.store(key, reply, gen)
		}
	} else if err == nil {
		// This client changed the document: what it cached is stale now, before
		// the announcement arrives.
		c.cache.invalidate("")
	}
	return response, err
}

func decodeReply(reply []byte) (Response, error) {
	if len(reply) == 0 || len(reply) > defaultMaxResponseBodySize {
		return Response{}, ErrMalformed
	}
	var response Response
	decoder := json.NewDecoder(strings.NewReader(string(reply)))
	decoder.UseNumber()
	if err := decoder.Decode(&response); err != nil {
		return Response{}, ErrMalformed
	}
	if err := decoder.Decode(&struct{}{}); err != io.EOF {
		return Response{}, ErrMalformed
	}
	if !response.OK {
		if response.Error == "" {
			return Response{}, ErrMalformed
		}
		return Response{}, &ModuleError{Code: response.Code, Message: response.Error}
	}
	return response, nil
}

func (c *Client) Values() (map[string]any, error) {
	response, err := c.request(Request{Operation: OpValues})
	if err != nil {
		return nil, err
	}
	if response.Values == nil {
		return nil, ErrMalformed
	}
	return response.Values, nil
}

// Snapshot returns every effective public configuration value. It exists for
// native services that need many narrow getters during one request; storage and
// parsing remain owned by the Go module.
func (c *Client) Snapshot() (map[string]any, error) {
	response, err := c.request(Request{Operation: OpSnapshot})
	if err != nil {
		return nil, err
	}
	if response.Values == nil {
		return nil, ErrMalformed
	}
	return response.Values, nil
}

// SnapshotWhenServed is Snapshot for a process starting beside the config
// module. A role's supervisor starts every module at once, so the first call
// can reach the bus before config serves its stage; that answer means "not
// yet", and it is retried every interval until config answers or within
// passes. Any other failure is returned at once, so a malformed or refusing
// config module still stops the caller.
func (c *Client) SnapshotWhenServed(ctx context.Context, within time.Duration) (map[string]any, error) {
	const interval = 100 * time.Millisecond
	deadline := time.Now().Add(within)
	for {
		values, err := c.Snapshot()
		if err == nil || !errors.Is(err, ring.ErrModuleCallCapabilityAbsent) || !time.Now().Before(deadline) {
			return values, err
		}
		select {
		case <-ctx.Done():
			return nil, fmt.Errorf("%w: %w", ErrUnavailable, ctx.Err())
		case <-time.After(interval):
		}
	}
}

func (c *Client) Value(key string) (any, error) {
	response, err := c.request(Request{Operation: OpValue, Key: key})
	return response.Value, err
}

func (c *Client) StringValue(key string) (string, bool, error) {
	response, err := c.request(Request{Operation: OpStringValue, Key: key})
	if err != nil {
		if moduleError, ok := err.(*ModuleError); ok && moduleError.Code == "not_found" {
			return "", false, nil
		}
		return "", false, err
	}
	value, ok := response.Value.(string)
	if !ok {
		return "", false, fmt.Errorf("config key %q is not a string", key)
	}
	return value, true, nil
}

func (c *Client) Set(key string, value any) error {
	return c.SetVersioned(key, value, "")
}

func (c *Client) SetVersioned(key string, value any, previousVersion string) error {
	_, err := c.request(Request{Operation: OpSetVersioned, Key: key, Value: value,
		PreviousVersion: previousVersion})
	return err
}

func (c *Client) SetTypedFacts(change TypedFactsMutation) error {
	_, err := c.request(Request{Operation: OpSetTypedFacts, Value: change})
	return err
}

func (c *Client) SetAPIHTTPListener(change APIHTTPListenerMutation) error {
	_, err := c.request(Request{Operation: OpSetAPIHTTPListener, Value: change})
	return err
}

func (c *Client) SetModelConcurrency(change ModelConcurrencyMutation) error {
	_, err := c.request(Request{Operation: OpSetModelConcurrency, Value: change})
	return err
}

func (c *Client) RemoveModelConcurrency(model string) error {
	_, err := c.request(Request{Operation: OpRemoveModelConcurrency,
		Value: ModelConcurrencyMutation{Model: model}})
	return err
}

func (c *Client) Version(key string) (string, error) {
	response, err := c.request(Request{Operation: OpVersion, Key: key})
	return response.Version, err
}

func (c *Client) TriggerRules() ([]TriggerRule, error) {
	response, err := c.request(Request{Operation: OpTriggerRules})
	if err != nil {
		return nil, err
	}
	if response.Rules == nil {
		return []TriggerRule{}, nil
	}
	return response.Rules, nil
}

func (c *Client) Int(key string, fallback int) int {
	value, ok, err := c.IntValue(key)
	if err != nil || !ok {
		return fallback
	}
	return value
}

func (c *Client) IntValue(key string) (int, bool, error) {
	value, err := c.Value(key)
	if err != nil {
		if moduleError, ok := err.(*ModuleError); ok && moduleError.Code == "not_found" {
			return 0, false, nil
		}
		return 0, false, err
	}
	switch number := value.(type) {
	case json.Number:
		parsed, parseErr := number.Int64()
		if parseErr == nil && int64(int(parsed)) == parsed {
			return int(parsed), true, nil
		}
	case float64:
		converted := int(number)
		if number == math.Trunc(number) && float64(converted) == number {
			return converted, true, nil
		}
	case int:
		return number, true, nil
	}
	return 0, false, fmt.Errorf("config key %q is not an integer", key)
}

func (c *Client) Bool(key string, fallback bool) bool {
	value, ok, err := c.BoolValue(key)
	if err != nil || !ok {
		return fallback
	}
	return value
}

func (c *Client) BoolValue(key string) (bool, bool, error) {
	value, err := c.Value(key)
	if err != nil {
		if moduleError, ok := err.(*ModuleError); ok && moduleError.Code == "not_found" {
			return false, false, nil
		}
		return false, false, err
	}
	result, ok := value.(bool)
	if !ok {
		return false, false, fmt.Errorf("config key %q is not boolean", key)
	}
	return result, true, nil
}
