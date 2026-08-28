package mcp

import (
	"bufio"
	"bytes"
	"context"
	"errors"
	"fmt"
	"io"
	"net/http"
	"net/url"
	"strings"
	"sync"
	"time"

	"github.com/JBailes/aimee/server-go/modules/egress"
)

type testEgress struct{}

func (testEgress) Authorize(context.Context, uint64, egress.Request) (egress.Decision, error) {
	return egress.Decision{Allowed: true, PolicyRevision: egress.PolicyRevision}, nil
}
func (testEgress) Do(context.Context, uint64, egress.HTTPRequest) (egress.HTTPResponse, error) {
	return egress.HTTPResponse{}, errors.New("unused test HTTP path")
}
func (testEgress) OpenSSE(ctx context.Context, _ uint64, request egress.SSERequest) (egress.SSEStream, error) {
	return openDirectTestSSE(ctx, request)
}

type directTestSSE struct {
	base, bearer, endpoint string
	limits                 egress.SSELimits
	stream, post           *http.Client
	body                   io.ReadCloser
	frames                 chan []byte
	errs                   chan error
	closed                 chan struct{}
	once                   sync.Once
}

func openDirectTestSSE(_ context.Context, request egress.SSERequest) (*directTestSSE, error) {
	if request.URL == "" {
		return nil, errors.New("empty SSE url")
	}
	transport := &http.Transport{ResponseHeaderTimeout: time.Duration(request.TimeoutMS) * time.Millisecond,
		MaxResponseHeaderBytes: 64 << 10}
	bearer := ""
	if request.CredentialHandle != "" {
		bearer = "s3cret"
	}
	t := &directTestSSE{base: request.URL, bearer: bearer, limits: request.Limits,
		stream: &http.Client{Transport: transport}, post: &http.Client{Transport: transport.Clone(), Timeout: time.Duration(request.Limits.PostTimeoutMS) * time.Millisecond},
		frames: make(chan []byte, 16), errs: make(chan error, 1), closed: make(chan struct{})}
	ctx, cancel := context.WithTimeout(context.Background(), time.Duration(request.Limits.ConnectionLifetimeMS)*time.Millisecond)
	req, err := http.NewRequestWithContext(ctx, "GET", request.URL, nil)
	if err != nil {
		cancel()
		return nil, err
	}
	req.Header.Set("Accept", "text/event-stream")
	t.auth(req)
	resp, err := t.stream.Do(req)
	if err != nil {
		cancel()
		return nil, err
	}
	if resp.StatusCode/100 != 2 {
		cancel()
		resp.Body.Close()
		return nil, fmt.Errorf("HTTP %d", resp.StatusCode)
	}
	t.body = resp.Body
	endpoint := make(chan string, 1)
	go t.read(cancel, endpoint)
	select {
	case ep := <-endpoint:
		t.endpoint = ep
		return t, nil
	case err := <-t.errs:
		_ = t.Close()
		return nil, err
	case <-time.After(time.Duration(request.TimeoutMS) * time.Millisecond):
		_ = t.Close()
		return nil, errors.New("no endpoint event before timeout")
	}
}
func (t *directTestSSE) auth(req *http.Request) {
	if t.bearer != "" {
		req.Header.Set("Authorization", "Bearer "+t.bearer)
	}
}
func testReadLine(r *bufio.Reader, max int) ([]byte, error) {
	var out []byte
	for {
		p, e := r.ReadSlice('\n')
		if len(out)+len(p) > max {
			return nil, errors.New("line limit")
		}
		out = append(out, p...)
		if e == nil {
			return out, nil
		}
		if !errors.Is(e, bufio.ErrBufferFull) {
			return out, e
		}
	}
}
func (t *directTestSSE) resolve(v string) string {
	b, e := url.Parse(t.base)
	if e != nil {
		return ""
	}
	r, e := url.Parse(v)
	if e != nil {
		return ""
	}
	x := b.ResolveReference(r)
	if x.Scheme != b.Scheme || !strings.EqualFold(x.Host, b.Host) {
		return ""
	}
	return x.String()
}
func (t *directTestSSE) read(cancel context.CancelFunc, endpoint chan<- string) {
	defer cancel()
	defer close(t.frames)
	r := bufio.NewReaderSize(t.body, min(t.limits.MaxLineBytes, 64<<10))
	var name string
	var data bytes.Buffer
	var total int64
	idle := time.AfterFunc(time.Duration(t.limits.IdleTimeoutMS)*time.Millisecond, func() { _ = t.body.Close() })
	defer idle.Stop()
	report := func(e error) {
		select {
		case t.errs <- e:
		default:
		}
	}
	dispatch := func() error {
		if data.Len() == 0 {
			name = ""
			return nil
		}
		p := strings.TrimRight(data.String(), "\n")
		data.Reset()
		n := name
		name = ""
		if n == "endpoint" {
			ep := t.resolve(strings.TrimSpace(p))
			if ep == "" {
				return errors.New("endpoint origin")
			}
			select {
			case endpoint <- ep:
			default:
			}
			return nil
		}
		select {
		case t.frames <- []byte(p):
		case <-t.closed:
		}
		return nil
	}
	for {
		line, e := testReadLine(r, t.limits.MaxLineBytes)
		total += int64(len(line))
		if total > t.limits.MaxConnectionBytes {
			report(errors.New("connection limit"))
			return
		}
		if len(line) > 0 {
			idle.Reset(time.Duration(t.limits.IdleTimeoutMS) * time.Millisecond)
		}
		s := strings.TrimRight(string(line), "\r\n")
		switch {
		case s == "":
			if x := dispatch(); x != nil {
				report(x)
				return
			}
		case strings.HasPrefix(s, ":"):
		case strings.HasPrefix(s, "event:"):
			name = strings.TrimSpace(s[6:])
		case strings.HasPrefix(s, "data:"):
			v := strings.TrimPrefix(s[5:], " ")
			add := len(v)
			if data.Len() > 0 {
				add++
			}
			if data.Len()+add > t.limits.MaxEventBytes {
				report(errors.New("event limit"))
				return
			}
			if data.Len() > 0 {
				data.WriteByte('\n')
			}
			data.WriteString(v)
		}
		if e != nil {
			report(e)
			return
		}
	}
}
func (t *directTestSSE) Send(frame []byte) error {
	req, e := http.NewRequest("POST", t.endpoint, bytes.NewReader(frame))
	if e != nil {
		return e
	}
	req.Header.Set("Content-Type", "application/json")
	t.auth(req)
	resp, e := t.post.Do(req)
	if e != nil {
		return e
	}
	n, de := io.CopyN(io.Discard, resp.Body, t.limits.MaxPostResponseBytes+1)
	ce := resp.Body.Close()
	if n > t.limits.MaxPostResponseBytes {
		return errors.New("post body limit")
	}
	if de != nil && !errors.Is(de, io.EOF) {
		return de
	}
	if ce != nil {
		return ce
	}
	if resp.StatusCode/100 != 2 {
		return fmt.Errorf("HTTP %d", resp.StatusCode)
	}
	return nil
}
func (t *directTestSSE) Recv(timeout time.Duration) ([]byte, error) {
	timer := time.NewTimer(timeout)
	defer timer.Stop()
	select {
	case f, ok := <-t.frames:
		if !ok {
			select {
			case e := <-t.errs:
				return nil, e
			default:
				return nil, errors.New("closed")
			}
		}
		return f, nil
	case <-timer.C:
		return nil, errors.New("timeout")
	}
}
func (t *directTestSSE) Close() error {
	t.once.Do(func() {
		close(t.closed)
		if t.body != nil {
			_ = t.body.Close()
		}
		t.stream.CloseIdleConnections()
		t.post.CloseIdleConnections()
	})
	return nil
}
