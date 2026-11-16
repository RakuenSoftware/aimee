// Package thinclient binds the shared Aimee C HTTP transport. It carries ordinary
// HTTP bytes and metadata; model and application schemas belong to its callers.
package thinclient

/*
#cgo pkg-config: aimee-client
#include <stdlib.h>
#include <cli_client.h>
*/
import "C"

import (
	"bufio"
	"context"
	"errors"
	"io"
	"net/http"
	"strings"
	"sync"
	"time"
	"unsafe"
)

// Transport uses the same request-local endpoint and identity as the CLI.
// Endpoint is required; there is no alternate Go network transport.
type Transport struct {
	Endpoint, Bearer, TLSHome string
	ConnectTimeout, IOTimeout time.Duration
}

func milliseconds(d time.Duration) C.int {
	if d <= 0 {
		return 0
	}
	n := d.Milliseconds()
	if n < 1 {
		n = 1
	}
	if n > 2147483647 {
		n = 2147483647
	}
	return C.int(n)
}

func (t *Transport) RoundTrip(req *http.Request) (*http.Response, error) {
	if req == nil {
		return nil, errors.New("thinclient: nil request")
	}
	var closeRequest func()
	if req.Body != nil {
		var once sync.Once
		closeRequest = func() { once.Do(func() { req.Body.Close() }) }
		defer closeRequest()
	}
	if t == nil || t.Endpoint == "" || req.URL == nil {
		return nil, errors.New("thinclient: missing endpoint or URL")
	}
	if err := req.Context().Err(); err != nil {
		return nil, err
	}
	var payload []byte
	var err error
	if req.Body != nil {
		readStopped := make(chan struct{})
		stopRead := context.AfterFunc(req.Context(), func() { closeRequest(); close(readStopped) })
		payload, err = io.ReadAll(req.Body)
		if !stopRead() {
			<-readStopped
		}
		if contextErr := req.Context().Err(); contextErr != nil {
			return nil, contextErr
		}
		if err != nil {
			return nil, err
		}
	}
	for _, value := range []string{t.Endpoint, t.Bearer, t.TLSHome, req.Method, req.URL.RequestURI(), req.Host, req.URL.Host} {
		if strings.ContainsRune(value, 0) {
			return nil, errors.New("thinclient: NUL in request metadata")
		}
	}
	for name, values := range req.Header {
		if strings.ContainsRune(name, 0) {
			return nil, errors.New("thinclient: NUL in request header")
		}
		for _, value := range values {
			if strings.ContainsRune(value, 0) {
				return nil, errors.New("thinclient: NUL in request header")
			}
		}
	}
	// All allocations below are C-owned and live through synchronous Open only.
	var allocations []unsafe.Pointer
	defer func() {
		for _, p := range allocations {
			C.free(p)
		}
	}()
	cstr := func(s string) *C.char {
		p := C.CString(s)
		allocations = append(allocations, unsafe.Pointer(p))
		return p
	}
	method := req.Method
	if method == "" {
		method = "GET"
	}
	options := C.cli_http_options_t{endpoint: cstr(t.Endpoint), method: cstr(method), path: cstr(req.URL.RequestURI()),
		connect_timeout_ms: milliseconds(t.ConnectTimeout), io_timeout_ms: milliseconds(t.IOTimeout)}
	if t.Bearer != "" {
		options.bearer = cstr(t.Bearer)
	}
	if t.TLSHome != "" {
		options.tls_home = cstr(t.TLSHome)
	}
	if len(payload) != 0 {
		options.body = C.CBytes(payload)
		allocations = append(allocations, options.body)
		options.body_length = C.size_t(len(payload))
	}
	var headers []C.cli_http_header_t
	host := req.Host
	if host == "" {
		host = req.URL.Host
	}
	if host != "" {
		headers = append(headers, C.cli_http_header_t{name: cstr("Host"), value: cstr(host)})
	}
	for name, values := range req.Header {
		// Open writes the framing for the actual bytes above. HTTP's Host field is
		// represented separately by Request.Host, as in net/http's transport.
		if strings.EqualFold(name, "Host") || strings.EqualFold(name, "Content-Length") || strings.EqualFold(name, "Transfer-Encoding") || strings.EqualFold(name, "Connection") {
			continue
		}
		for _, value := range values {
			headers = append(headers, C.cli_http_header_t{name: cstr(name), value: cstr(value)})
		}
	}
	if len(headers) != 0 {
		p := C.malloc(C.size_t(len(headers)) * C.size_t(unsafe.Sizeof(C.cli_http_header_t{})))
		if p == nil {
			return nil, errors.New("thinclient: allocation failed")
		}
		allocations = append(allocations, p)
		copy(unsafe.Slice((*C.cli_http_header_t)(p), len(headers)), headers)
		options.headers = (*C.cli_http_header_t)(p)
		options.header_count = C.size_t(len(headers))
	}
	stream := C.cli_http_stream_new()
	if stream == nil {
		return nil, errors.New("thinclient: allocation failed")
	}
	body := &responseBody{stream: stream, ctx: req.Context(), cancelDone: make(chan struct{})}
	body.stopCancel = context.AfterFunc(req.Context(), func() { C.cli_http_stream_cancel(stream); close(body.cancelDone) })
	if C.cli_http_stream_open(stream, &options) != 0 {
		body.Close()
		if err := req.Context().Err(); err != nil {
			return nil, err
		}
		return nil, errors.New("thinclient: request transport failed")
	}
	response, err := http.ReadResponse(bufio.NewReader(strings.NewReader(C.GoString(C.cli_http_stream_headers(stream)))), req)
	if err != nil {
		body.Close()
		return nil, errors.New("thinclient: invalid response headers")
	}
	if err := req.Context().Err(); err != nil {
		body.Close()
		return nil, err
	}
	response.Body = body
	return response, nil
}

type responseBody struct {
	readMu     sync.Mutex
	closeOnce  sync.Once
	stream     *C.cli_http_stream_t
	ctx        context.Context
	stopCancel func() bool
	cancelDone chan struct{}
	closed     bool
}

func (b *responseBody) Read(p []byte) (int, error) {
	b.readMu.Lock()
	defer b.readMu.Unlock()
	if b.closed {
		return 0, io.ErrClosedPipe
	}
	if len(p) == 0 {
		return 0, nil
	}
	n := C.cli_http_stream_read(b.stream, unsafe.Pointer(&p[0]), C.size_t(len(p)))
	if n < 0 {
		if err := b.ctx.Err(); err != nil {
			return 0, err
		}
		return 0, errors.New("thinclient: response transport failed")
	}
	if n == 0 {
		return 0, io.EOF
	}
	return int(n), nil
}

func (b *responseBody) Close() error {
	b.closeOnce.Do(func() {
		// Shutdown unblocks Read without racing free. Then stop/join the context
		// callback and reader before releasing the C connection, exactly once.
		C.cli_http_stream_cancel(b.stream)
		if !b.stopCancel() {
			<-b.cancelDone
		}
		b.readMu.Lock()
		b.closed = true
		C.cli_http_stream_free(b.stream)
		b.readMu.Unlock()
	})
	return nil
}
