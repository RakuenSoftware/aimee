package memory

import (
	"bytes"
	"context"
	"encoding/json"
	"fmt"
	"time"

	memorycontract "github.com/JBailes/aimee/server-go/memory"
)

var (
	ErrClientConfig   = memorycontract.ErrClientConfig
	ErrClientRequest  = memorycontract.ErrClientRequest
	ErrClientResponse = memorycontract.ErrClientResponse
)

type StageCaller = memorycontract.StageCaller
type TurnScan = memorycontract.TurnScan

// Client retains legacy typed authority convenience methods. New callers use server-go/memory.
type Client struct{ *memorycontract.Client }

func NewClient(caller StageCaller, deadline time.Duration) (*Client, error) {
	c, err := memorycontract.NewClient(caller, deadline)
	if err != nil {
		return nil, err
	}
	return &Client{c}, nil
}

// contract preserves the legacy nil-receiver failure behavior. Directly
// promoting embedded methods would dereference a nil legacy wrapper first.
func (c *Client) contract() *memorycontract.Client {
	if c == nil {
		return nil
	}
	return c.Client
}

func (c *Client) Rerank(ctx context.Context, trace uint64, scoreMicros int64) (uint32, error) {
	return c.contract().Rerank(ctx, trace, scoreMicros)
}

func (c *Client) CheckFact(ctx context.Context, trace uint64, head NodeKind, relation string, tail NodeKind) (FactVerdict, error) {
	return c.contract().CheckFact(ctx, trace, head, relation, tail)
}

func (c *Client) RequestsSensitive(ctx context.Context, trace uint64, text string) (bool, error) {
	return c.contract().RequestsSensitive(ctx, trace, text)
}

func (c *Client) ScanTurn(ctx context.Context, trace uint64, text string) (TurnScan, error) {
	return c.contract().ScanTurn(ctx, trace, text)
}

func (c *Client) Sensitivities(ctx context.Context, trace uint64, relations []string) ([]RelSensitivity, error) {
	return c.contract().Sensitivities(ctx, trace, relations)
}

func (c *Client) Extract(ctx context.Context, trace uint64, text string, max uint32) ([]Triple, error) {
	return c.contract().Extract(ctx, trace, text, max)
}

func (c *Client) Commands(ctx context.Context, trace uint64) ([]Command, error) {
	return c.contract().Commands(ctx, trace)
}

func (c *Client) Embed(ctx context.Context, trace uint64, request EmbedRequest) (EmbedResponse, error) {
	return c.contract().Embed(ctx, trace, request)
}

func (c *Client) Command(ctx context.Context, trace uint64, verb string, args json.RawMessage) (json.RawMessage, error) {
	return c.contract().Command(ctx, trace, verb, args)
}

func (c *Client) CheckFactWrite(ctx context.Context, trace uint64, request FactWriteRequest) (FactWriteDecision, error) {
	return c.contract().CheckFactWrite(ctx, trace, request)
}

func clientJSONObject(body []byte, out any) error {
	body = bytes.TrimSpace(body)
	if len(body) == 0 || body[0] != '{' || json.Unmarshal(body, out) != nil {
		return ErrClientResponse
	}
	return nil
}

// DataJSON keeps the exact JSON response for callers such as the live probe.
// Scope is carried unchanged; only the authenticated Go owner can authorize it.
func (c *Client) DataJSON(ctx context.Context, trace uint64, request []byte) (json.RawMessage, error) {
	decoded, err := decodeDataRequest(request)
	if err != nil || decoded.Operation == "" {
		return nil, ErrClientRequest
	}
	if c == nil || c.Client == nil {
		return nil, ErrClientConfig
	}
	response, err := c.Client.DataJSON(ctx, trace, request)
	if err != nil {
		return nil, err
	}
	var result DataResponse
	if err := clientJSONObject(response, &result); err != nil {
		return nil, err
	}
	var envelope struct {
		Records json.RawMessage `json:"records"`
	}
	if json.Unmarshal(response, &envelope) != nil || len(envelope.Records) == 0 {
		return nil, ErrClientResponse
	}
	return json.RawMessage(response), nil
}

func (c *Client) Data(ctx context.Context, trace uint64, request DataRequest) (DataResponse, error) {
	body, err := json.Marshal(request)
	if err != nil {
		return DataResponse{}, fmt.Errorf("%w: %v", ErrClientRequest, err)
	}
	response, err := c.DataJSON(ctx, trace, body)
	if err != nil {
		return DataResponse{}, err
	}
	var result DataResponse
	if err := clientJSONObject(response, &result); err != nil {
		return DataResponse{}, err
	}
	return result, nil
}
