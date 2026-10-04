package memory

import (
	"bytes"
	"context"
	"encoding/binary"
	"encoding/json"
	"errors"
	"fmt"
	"strings"
	"time"

	"github.com/JBailes/aimee/server-go/bus"
)

var (
	ErrClientConfig   = errors.New("memory: invalid client configuration")
	ErrClientRequest  = errors.New("memory: invalid client request")
	ErrClientResponse = errors.New("memory: malformed module response")
)

// StageCaller is implemented by the existing Go bus callers. It owns admission,
// correlation, cancellation and transport status; the memory client owns framing.
type StageCaller interface {
	Call(context.Context, uint32, uint32, uint64, time.Duration, []byte) ([]byte, error)
}

// Client borrows an admitted bus caller. It never attaches a principal, retries a
// mutation, or supplies a local policy fallback. Use a ConcurrentModuleCaller
// when sharing a client across goroutines; close/drain that caller before detach.
type Client struct {
	caller   StageCaller
	deadline time.Duration
}

// NewClient uses a five-second bound when deadline is zero. Maintenance callers
// should supply their explicit longer bound; the request context can shorten it.
func NewClient(caller StageCaller, deadline time.Duration) (*Client, error) {
	if caller == nil || deadline < 0 {
		return nil, ErrClientConfig
	}
	if deadline == 0 {
		deadline = 5 * time.Second
	}
	return &Client{caller: caller, deadline: deadline}, nil
}

func (c *Client) call(ctx context.Context, event, stage uint32, trace uint64, request []byte) ([]byte, error) {
	if c == nil || c.caller == nil || ctx == nil {
		return nil, ErrClientConfig
	}
	if err := ctx.Err(); err != nil {
		return nil, err
	}
	if len(request) > int(bus.ModuleMessageMaxBody) {
		return nil, ErrClientRequest
	}
	deadline := c.deadline
	if until, ok := ctx.Deadline(); ok {
		if remaining := time.Until(until); remaining < deadline {
			deadline = remaining
		}
		if deadline <= 0 {
			return nil, context.DeadlineExceeded
		}
	}
	response, err := c.caller.Call(ctx, event, stage, trace, deadline, request)
	if err != nil {
		return nil, err
	}
	if len(response) > int(bus.ModuleMessageMaxBody) {
		return nil, ErrClientResponse
	}
	return response, nil
}

func clientHeader(magic uint32, size int) []byte {
	frame := make([]byte, size)
	binary.LittleEndian.PutUint32(frame, magic)
	binary.LittleEndian.PutUint32(frame[4:], WireVersion)
	return frame
}

func clientValue(response []byte, magic, low, high uint32) (uint32, error) {
	if len(response) != 8 || binary.LittleEndian.Uint32(response) != magic {
		return 0, ErrClientResponse
	}
	value := binary.LittleEndian.Uint32(response[4:])
	if value < low || value > high {
		return 0, ErrClientResponse
	}
	return value, nil
}

// Rerank returns the owner's confidence band for a fixed-point score.
func (c *Client) Rerank(ctx context.Context, trace uint64, scoreMicros int64) (uint32, error) {
	request := clientHeader(RequestMagic, RequestLen)
	binary.LittleEndian.PutUint64(request[8:], uint64(scoreMicros))
	response, err := c.call(ctx, EventRerank, StageRerank, trace, request)
	if err != nil {
		return 0, err
	}
	return clientValue(response, ResponseMagic, ConfidenceLow, ConfidenceHigh)
}

// CheckFact preserves an empty relation so the owner can return FactBadArg.
func (c *Client) CheckFact(ctx context.Context, trace uint64, head NodeKind, relation string, tail NodeKind) (FactVerdict, error) {
	if len(relation) > RelTypeMax {
		return FactBadArg, ErrClientRequest
	}
	request := clientHeader(GateRequestMagic, GateRequestLen)
	binary.LittleEndian.PutUint32(request[8:], uint32(head))
	binary.LittleEndian.PutUint32(request[12:], uint32(tail))
	binary.LittleEndian.PutUint16(request[16:], uint16(len(relation)))
	copy(request[20:], relation)
	response, err := c.call(ctx, EventWrite, StageWrite, trace, request)
	if err != nil {
		return FactBadArg, err
	}
	value, err := clientValue(response, GateResponseMagic, uint32(FactAccept), uint32(FactBadArg))
	if err != nil {
		return FactBadArg, err
	}
	return FactVerdict(value), nil
}

func clientText(magic uint32, text string) ([]byte, error) {
	if len(text) > int(bus.ModuleMessageMaxBody)-12 {
		return nil, ErrClientRequest
	}
	request := clientHeader(magic, 12)
	binary.LittleEndian.PutUint32(request[8:], uint32(len(text)))
	return append(request, text...), nil
}

func (c *Client) RequestsSensitive(ctx context.Context, trace uint64, text string) (bool, error) {
	request, err := clientText(PiiRequestMagic, text)
	if err != nil {
		return false, err
	}
	response, err := c.call(ctx, EventRetrieve, StageRetrieve, trace, request)
	if err != nil {
		return false, err
	}
	value, err := clientValue(response, PiiResponseMagic, 0, 1)
	return value == 1, err
}

type TurnScan struct {
	Retraction   bool
	HasAttribute bool
	Attribute    string
}

func (c *Client) ScanTurn(ctx context.Context, trace uint64, text string) (TurnScan, error) {
	request, err := clientText(ScanRequestMagic, text)
	if err != nil {
		return TurnScan{}, err
	}
	response, err := c.call(ctx, EventExtractIndex, StageExtractIndex, trace, request)
	if err != nil {
		return TurnScan{}, err
	}
	if len(response) < ScanResponseHeaderLen || binary.LittleEndian.Uint32(response) != ScanResponseMagic {
		return TurnScan{}, ErrClientResponse
	}
	retraction := binary.LittleEndian.Uint32(response[4:])
	has := binary.LittleEndian.Uint32(response[8:])
	length := binary.LittleEndian.Uint32(response[12:])
	if retraction > 1 || has > 1 || length >= AttrMax || uint64(length) != uint64(len(response)-ScanResponseHeaderLen) {
		return TurnScan{}, ErrClientResponse
	}
	return TurnScan{retraction == 1, has == 1, string(response[ScanResponseHeaderLen:])}, nil
}

func (c *Client) Sensitivities(ctx context.Context, trace uint64, relations []string) ([]RelSensitivity, error) {
	if len(relations) == 0 || len(relations) > (int(bus.ModuleMessageMaxBody)-SensRequestHeaderLen)/2 {
		return nil, ErrClientRequest
	}
	size := SensRequestHeaderLen
	for _, relation := range relations {
		if len(relation) > RelTypeMax {
			return nil, ErrClientRequest
		}
		size += 2 + len(relation)
		if size > int(bus.ModuleMessageMaxBody) {
			return nil, ErrClientRequest
		}
	}
	request := clientHeader(SensRequestMagic, size)
	binary.LittleEndian.PutUint32(request[8:], uint32(len(relations)))
	offset := SensRequestHeaderLen
	for _, relation := range relations {
		binary.LittleEndian.PutUint16(request[offset:], uint16(len(relation)))
		copy(request[offset+2:], relation)
		offset += 2 + len(relation)
	}
	response, err := c.call(ctx, EventRetrieve, StageRetrieve, trace, request)
	if err != nil {
		return nil, err
	}
	if len(response) != SensResponseHeaderLen+len(relations) ||
		binary.LittleEndian.Uint32(response) != SensResponseMagic ||
		binary.LittleEndian.Uint32(response[4:]) != uint32(len(relations)) {
		return nil, ErrClientResponse
	}
	result := make([]RelSensitivity, len(relations))
	for i, value := range response[SensResponseHeaderLen:] {
		if RelSensitivity(value) > SensSecret {
			return nil, ErrClientResponse
		}
		result[i] = RelSensitivity(value)
	}
	return result, nil
}

func (c *Client) Extract(ctx context.Context, trace uint64, text string, max uint32) ([]Triple, error) {
	if max == 0 || len(text) > int(bus.ModuleMessageMaxBody)-ExtractRequestHeaderLen {
		return nil, ErrClientRequest
	}
	request := clientHeader(ExtractRequestMagic, ExtractRequestHeaderLen)
	binary.LittleEndian.PutUint32(request[8:], max)
	binary.LittleEndian.PutUint32(request[12:], uint32(len(text)))
	response, err := c.call(ctx, EventExtractIndex, StageExtractIndex, trace, append(request, text...))
	if err != nil {
		return nil, err
	}
	if len(response) < ExtractResponseHeaderLen || binary.LittleEndian.Uint32(response) != ExtractResponseMagic {
		return nil, ErrClientResponse
	}
	count := binary.LittleEndian.Uint32(response[4:])
	// Every triple needs two kinds and three lengths, even with empty strings.
	if count > max || uint64(count) > uint64(len(response)-ExtractResponseHeaderLen)/20 {
		return nil, ErrClientResponse
	}
	d := clientDecoder{body: response[ExtractResponseHeaderLen:]}
	result := make([]Triple, 0, int(count))
	for range count {
		kinds := d.take(8)
		if kinds == nil {
			return nil, ErrClientResponse
		}
		triple := Triple{SubjectKind: NodeKind(binary.LittleEndian.Uint32(kinds)), ObjectKind: NodeKind(binary.LittleEndian.Uint32(kinds[4:]))}
		triple.Subject = d.field(TripleSubjectMax)
		triple.RelType = d.field(TripleRelTypeMax)
		triple.Object = d.field(TripleObjectMax)
		result = append(result, triple)
	}
	if d.failed || len(d.body) != 0 {
		return nil, ErrClientResponse
	}
	return result, nil
}

type clientDecoder struct {
	body   []byte
	failed bool
}

func (d *clientDecoder) take(n int) []byte {
	if d.failed || n < 0 || n > len(d.body) {
		d.failed = true
		return nil
	}
	value := d.body[:n]
	d.body = d.body[n:]
	return value
}

func (d *clientDecoder) field(capacity uint32) string {
	header := d.take(4)
	if header == nil {
		return ""
	}
	length := binary.LittleEndian.Uint32(header)
	if length >= capacity || uint64(length) > uint64(len(d.body)) {
		d.failed = true
		return ""
	}
	return string(d.take(int(length)))
}

func (c *Client) Commands(ctx context.Context, trace uint64) ([]Command, error) {
	response, err := c.call(ctx, EventDeclareCommands, StageDeclareCommands, trace, clientHeader(CommandsRequestMagic, CommandsRequestLen))
	if err != nil {
		return nil, err
	}
	if len(response) < 12 || binary.LittleEndian.Uint32(response) != CommandsResponseMagic || binary.LittleEndian.Uint32(response[4:]) != WireVersion {
		return nil, ErrClientResponse
	}
	count := binary.LittleEndian.Uint32(response[8:])
	if uint64(count) > uint64(len(response)-12)/16 {
		return nil, ErrClientResponse
	}
	d := clientDecoder{body: response[12:]}
	result := make([]Command, 0, int(count))
	for range count {
		header := d.take(16)
		if header == nil || binary.LittleEndian.Uint16(header[14:]) != 0 {
			return nil, ErrClientResponse
		}
		command := Command{Surfaces: binary.LittleEndian.Uint32(header), Visibility: binary.LittleEndian.Uint32(header[4:])}
		command.Group = string(d.take(int(binary.LittleEndian.Uint16(header[8:]))))
		command.Verb = string(d.take(int(binary.LittleEndian.Uint16(header[10:]))))
		command.Summary = string(d.take(int(binary.LittleEndian.Uint16(header[12:]))))
		result = append(result, command)
	}
	if d.failed || len(d.body) != 0 {
		return nil, ErrClientResponse
	}
	return result, nil
}

func clientJSONObject(body []byte, out any) error {
	body = bytes.TrimSpace(body)
	if len(body) == 0 || body[0] != '{' || json.Unmarshal(body, out) != nil {
		return ErrClientResponse
	}
	return nil
}

// Embed preserves the owner's unavailable, unauthorized, truncated and error
// outcomes. A successful bus call alone does not mean embedding succeeded.
func (c *Client) Embed(ctx context.Context, trace uint64, request EmbedRequest) (EmbedResponse, error) {
	body, err := json.Marshal(request)
	if err != nil {
		return EmbedResponse{}, fmt.Errorf("%w: %v", ErrClientRequest, err)
	}
	response, err := c.call(ctx, EventEmbed, StageEmbed, trace, body)
	if err != nil {
		return EmbedResponse{}, err
	}
	var result EmbedResponse
	if err := clientJSONObject(response, &result); err != nil {
		return EmbedResponse{}, err
	}
	var required struct {
		Dim *int `json:"dim"`
	}
	if json.Unmarshal(response, &required) != nil || required.Dim == nil {
		return EmbedResponse{}, ErrClientResponse
	}
	if result.Dim < 0 || result.Dim != len(result.Vector) {
		return EmbedResponse{}, ErrClientResponse
	}
	return result, nil
}

// DataJSON preserves exact integer tokens and unknown versioned payload fields.
// The fixed authority validates operation-specific fields and all authorization.
func (c *Client) DataJSON(ctx context.Context, trace uint64, request []byte) (json.RawMessage, error) {
	if len(request) == 0 || len(request) > int(bus.ModuleMessageMaxBody) {
		return nil, ErrClientRequest
	}
	var envelope struct {
		Operation string `json:"operation"`
	}
	if json.Unmarshal(request, &envelope) != nil || strings.TrimSpace(envelope.Operation) == "" {
		return nil, ErrClientRequest
	}
	response, err := c.call(ctx, EventData, StageData, trace, request)
	if err != nil {
		return nil, err
	}
	var result struct {
		Records json.RawMessage `json:"records"`
	}
	if clientJSONObject(response, &result) != nil || len(result.Records) == 0 {
		return nil, ErrClientResponse
	}
	if string(result.Records) != "null" {
		var records []map[string]json.RawMessage
		if json.Unmarshal(result.Records, &records) != nil {
			return nil, ErrClientResponse
		}
	}
	return json.RawMessage(response), nil
}

// Command invokes the public command contract. Its result is the complete
// surface response, including degraded evidence, typed argument and not-found errors. Transport
// failures remain errors and are never retried here.
func (c *Client) Command(ctx context.Context, trace uint64, verb string, args json.RawMessage) (json.RawMessage, error) {
	frame, err := bus.EncodeCommand(verb, args)
	if err != nil {
		return nil, ErrClientRequest
	}
	reply, err := c.call(ctx, EventCommand, StageCommand, trace, frame)
	if err != nil {
		return nil, err
	}
	body, err := bus.DecodeCommandResult(reply)
	if err != nil {
		return nil, ErrClientResponse
	}
	var object map[string]json.RawMessage
	if json.Unmarshal(body, &object) != nil || object == nil {
		return nil, ErrClientResponse
	}
	var status string
	if json.Unmarshal(object["status"], &status) != nil || (status != "ok" && status != "error" && status != "degraded" && !(verb == "benchmark" && status == "async-only")) {
		return nil, ErrClientResponse
	}
	return body, nil
}

func (c *Client) CheckFactWrite(ctx context.Context, trace uint64, request FactWriteRequest) (FactWriteDecision, error) {
	if len(request.Relation) > RelTypeMax {
		return FactWriteDecision{}, ErrClientRequest
	}
	body, err := json.Marshal(struct {
		Operation string            `json:"operation"`
		FactWrite *FactWriteRequest `json:"fact_write"`
	}{"fact-write-decision", &request})
	if err != nil {
		return FactWriteDecision{}, ErrClientRequest
	}
	raw, err := c.DataJSON(ctx, trace, body)
	if err != nil {
		return FactWriteDecision{}, err
	}
	var response struct {
		FactWrite *FactWriteDecision `json:"fact_write"`
	}
	if err := json.Unmarshal(raw, &response); err != nil {
		return FactWriteDecision{}, ErrClientResponse
	}
	if response.FactWrite == nil || response.FactWrite.Verdict > FactBadArg ||
		(response.FactWrite.CommitAllowed && response.FactWrite.Verdict != FactAccept && response.FactWrite.Verdict != FactNovel) {
		return FactWriteDecision{}, ErrClientResponse
	}
	return *response.FactWrite, nil
}

// ResetDerived is the internal owner lifecycle operation. Bus admission limits
// it to the trusted connection host; it is not a declared public command.
func (c *Client) ResetDerived(ctx context.Context, trace uint64) error {
	reply, err := c.DataJSON(ctx, trace, []byte(`{"operation":"reset-derived"}`))
	if err != nil {
		return err
	}
	var result struct {
		Deleted bool `json:"deleted"`
	}
	if json.Unmarshal(reply, &result) != nil || !result.Deleted {
		return ErrClientResponse
	}
	return nil
}
