package mcp

import (
	"context"
	"fmt"
	"time"

	"github.com/JBailes/aimee/server-go/modules/egress"
)

type sseLimits struct {
	maxLineBytes         int
	maxEventBytes        int
	maxPostResponseBytes int64
	maxConnectionBytes   int64
	idleTimeout          time.Duration
	connectionLifetime   time.Duration
	postTimeout          time.Duration
}

var defaultSSELimits = sseLimits{
	maxLineBytes: 64 << 10, maxEventBytes: 1 << 20,
	maxPostResponseBytes: 64 << 10, maxConnectionBytes: 256 << 20,
	idleTimeout: 2 * time.Minute, connectionLifetime: 24 * time.Hour,
	postTimeout: 30 * time.Second,
}

// SSETransport is a thin MCP transport adapter. The stream socket, bearer and
// response bytes live in the separately authenticated egress process; this
// module sees only bounded JSON-RPC frames returned over the event bus.
type SSETransport struct{ stream egress.SSEStream }

func NewSSETransport(rawURL, credentialHandle string, timeout time.Duration, client egress.Client,
	traceID uint64) (*SSETransport, error) {
	return newSSETransportWithLimits(rawURL, credentialHandle, timeout, defaultSSELimits, client, traceID)
}

func newSSETransportWithLimits(rawURL, credentialHandle string, timeout time.Duration, limits sseLimits,
	client egress.Client, traceID uint64) (*SSETransport, error) {
	if rawURL == "" {
		return nil, fmt.Errorf("%w: empty SSE url", ErrTransport)
	}
	if timeout <= 0 {
		timeout = 30 * time.Second
	}
	if client == nil {
		return nil, fmt.Errorf("%w: egress streaming transport is not configured", ErrTransport)
	}
	stream, err := client.OpenSSE(context.Background(), traceID, egress.SSERequest{URL: rawURL, CredentialHandle: credentialHandle,
		TimeoutMS: timeout.Milliseconds(), Limits: egress.SSELimits{
			MaxLineBytes: limits.maxLineBytes, MaxEventBytes: limits.maxEventBytes,
			MaxPostResponseBytes: limits.maxPostResponseBytes,
			MaxConnectionBytes:   limits.maxConnectionBytes,
			IdleTimeoutMS:        limits.idleTimeout.Milliseconds(),
			ConnectionLifetimeMS: limits.connectionLifetime.Milliseconds(),
			PostTimeoutMS:        limits.postTimeout.Milliseconds(),
		}})
	if err != nil {
		return nil, fmt.Errorf("%w: %v", ErrTransport, err)
	}
	return &SSETransport{stream: stream}, nil
}

func (t *SSETransport) Send(frame []byte) error {
	if t == nil || t.stream == nil {
		return fmt.Errorf("%w: SSE stream is not configured", ErrTransport)
	}
	if err := t.stream.Send(frame); err != nil {
		return fmt.Errorf("%w: %v", ErrTransport, err)
	}
	return nil
}

func (t *SSETransport) Recv(timeout time.Duration) ([]byte, error) {
	if timeout <= 0 {
		return nil, ErrTimeout
	}
	frame, err := t.stream.Recv(timeout)
	if err != nil {
		if timeout > 0 && containsTimeout(err.Error()) {
			return nil, ErrTimeout
		}
		return nil, fmt.Errorf("%w: %v", ErrTransport, err)
	}
	return frame, nil
}

func containsTimeout(value string) bool {
	for i := 0; i+7 <= len(value); i++ {
		if value[i:i+7] == "timeout" {
			return true
		}
	}
	return false
}

func (t *SSETransport) Close() error {
	if t == nil || t.stream == nil {
		return nil
	}
	return t.stream.Close()
}
