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
	"time"
)

const (
	// Carved from config's permanent principal ref 2:
	// 4096 + 2*256 + stage ordinal 1.
	EventConfig uint32 = 4609
	StageConfig uint32 = 1

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
	body, err := json.Marshal(request)
	if err != nil {
		return Response{}, err
	}
	reply, err := c.caller.Call(context.Background(), EventConfig, StageConfig, 0, c.deadline, body)
	if err != nil {
		return Response{}, fmt.Errorf("%w: %w", ErrUnavailable, err)
	}
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
