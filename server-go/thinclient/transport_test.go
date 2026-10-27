package thinclient

import (
	"bytes"
	"context"
	"errors"
	"fmt"
	"io"
	"net"
	"net/http"
	"net/http/httptest"
	"path/filepath"
	"strings"
	"sync"
	"sync/atomic"
	"testing"
	"time"
)

func clientFor(t *testing.T, handler http.HandlerFunc) *http.Client {
	t.Helper()
	server := httptest.NewServer(handler)
	t.Cleanup(server.Close)
	return &http.Client{Transport: &Transport{Endpoint: "tcp:" + strings.TrimPrefix(server.URL, "http://")}}
}

func TestRequestResponseBytesAndMetadata(t *testing.T) {
	payload := []byte{0, '{', '"', 'x', '"', ':', '1', '}', 255}
	client := clientFor(t, func(w http.ResponseWriter, r *http.Request) {
		body, err := io.ReadAll(r.Body)
		if err != nil || !bytes.Equal(body, payload) || r.Method != "PUT" || r.RequestURI != "/v1/domain?cursor=12%2F3" || r.Host != "selected-host" || r.Header.Get("Cookie") != "session=opaque" || r.Header.Get("X-Aimee-Actor") != "alice" || len(r.Header.Values("X-Many")) != 2 {
			t.Errorf("request bytes/metadata changed: method=%s uri=%s host=%s body=%x headers=%v err=%v", r.Method, r.RequestURI, r.Host, body, r.Header, err)
		}
		w.Header().Add("X-Many", "a")
		w.Header().Add("X-Many", "b")
		w.Header().Set("Content-Length", "5")
		w.WriteHeader(409)
		w.Write([]byte{0, 'o', 'k', 255, '!'})
	})
	req, _ := http.NewRequest("PUT", "http://unused/v1/domain?cursor=12%2F3", bytes.NewReader(payload))
	req.Host = "selected-host"
	req.Header.Set("Cookie", "session=opaque")
	req.Header.Set("X-Aimee-Actor", "alice")
	req.Header.Add("X-Many", "1")
	req.Header.Add("X-Many", "2")
	response, err := client.Do(req)
	if err != nil {
		t.Fatal(err)
	}
	defer response.Body.Close()
	body, err := io.ReadAll(response.Body)
	if err != nil || response.StatusCode != 409 || response.ContentLength != 5 || len(response.Header.Values("X-Many")) != 2 || !bytes.Equal(body, []byte{0, 'o', 'k', 255, '!'}) {
		t.Fatalf("response changed: %+v body=%x err=%v", response, body, err)
	}
}

func TestSSEChunkingAndCancellation(t *testing.T) {
	first := make(chan struct{})
	released := make(chan struct{})
	client := clientFor(t, func(w http.ResponseWriter, r *http.Request) {
		defer close(released)
		w.Header().Set("Content-Type", "text/event-stream")
		fmt.Fprint(w, "data: first\n\n")
		w.(http.Flusher).Flush()
		close(first)
		<-r.Context().Done()
	})
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	req, _ := http.NewRequestWithContext(ctx, "GET", "http://unused/events", nil)
	response, err := client.Do(req)
	if err != nil {
		t.Fatal(err)
	}
	<-first
	b := make([]byte, len("data: first\n\n"))
	if _, err := io.ReadFull(response.Body, b); err != nil || string(b) != "data: first\n\n" {
		t.Fatalf("SSE prefix %q %v", b, err)
	}
	if response.ContentLength != -1 || len(response.TransferEncoding) != 1 || response.TransferEncoding[0] != "chunked" {
		t.Fatalf("chunked metadata changed: %+v", response)
	}
	done := make(chan error, 1)
	go func() { _, err := response.Body.Read(make([]byte, 1)); done <- err }()
	cancel()
	select {
	case err := <-done:
		if !errors.Is(err, context.Canceled) {
			t.Fatalf("cancel error %v", err)
		}
	case <-time.After(2 * time.Second):
		t.Fatal("cancel did not unblock Read")
	}
	if err := response.Body.Close(); err != nil {
		t.Fatal(err)
	}
	select {
	case <-released:
	case <-time.After(2 * time.Second):
		t.Fatal("connection not closed")
	}
}

func TestConcurrentBodyCloseJoinsReader(t *testing.T) {
	client := clientFor(t, func(w http.ResponseWriter, r *http.Request) {
		w.WriteHeader(200)
		w.(http.Flusher).Flush()
		<-r.Context().Done()
	})
	response, err := client.Get("http://unused/events")
	if err != nil {
		t.Fatal(err)
	}
	done := make(chan struct{})
	go func() { response.Body.Read(make([]byte, 1)); close(done) }()
	var wg sync.WaitGroup
	for i := 0; i < 8; i++ {
		wg.Go(func() { response.Body.Close() })
	}
	wg.Wait()
	select {
	case <-done:
	case <-time.After(2 * time.Second):
		t.Fatal("Close did not unblock reader")
	}
	if _, err := response.Body.Read(make([]byte, 1)); !errors.Is(err, io.ErrClosedPipe) {
		t.Fatalf("read after Close %v", err)
	}
}

func TestUnixSocketAndCloseDelimitedResponse(t *testing.T) {
	path := filepath.Join(t.TempDir(), "http.sock")
	listener, err := net.Listen("unix", path)
	if err != nil {
		t.Fatal(err)
	}
	defer listener.Close()
	go func() {
		conn, err := listener.Accept()
		if err != nil {
			return
		}
		defer conn.Close()
		b := make([]byte, 4096)
		conn.Read(b)
		fmt.Fprint(conn, "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\nplain")
	}()
	client := &http.Client{Transport: &Transport{Endpoint: "unix:" + path}}
	response, err := client.Get("http://unix/raw")
	if err != nil {
		t.Fatal(err)
	}
	defer response.Body.Close()
	b, err := io.ReadAll(response.Body)
	if err != nil || string(b) != "plain" || response.ContentLength != -1 {
		t.Fatalf("close body %q %v %+v", b, err, response)
	}
}

func TestErrorsAndPreCancelledRequest(t *testing.T) {
	ctx, cancel := context.WithCancel(context.Background())
	cancel()
	req, _ := http.NewRequestWithContext(ctx, "GET", "http://unused/", nil)
	if _, err := (&Transport{Endpoint: "tcp:127.0.0.1:1"}).RoundTrip(req); !errors.Is(err, context.Canceled) {
		t.Fatalf("cancel %v", err)
	}
	req, _ = http.NewRequest("GET", "http://unused/", nil)
	req.Header.Set("X-Aimee-Actor", "ali\x00ce")
	if _, err := (&Transport{Endpoint: "tcp:127.0.0.1:1"}).RoundTrip(req); err == nil {
		t.Fatal("NUL header was truncated")
	}
	req.Header.Del("X-Aimee-Actor")
	if _, err := (&Transport{Endpoint: "unix:" + filepath.Join(t.TempDir(), "absent.sock")}).RoundTrip(req); err == nil {
		t.Fatal("missing endpoint accepted")
	}
}

func TestDeadlineDuringOpenAndRead(t *testing.T) {
	for _, flush := range []bool{false, true} {
		t.Run(fmt.Sprint("headers=", flush), func(t *testing.T) {
			client := clientFor(t, func(w http.ResponseWriter, r *http.Request) {
				if flush {
					w.WriteHeader(200)
					w.(http.Flusher).Flush()
				}
				<-r.Context().Done()
			})
			ctx, cancel := context.WithTimeout(context.Background(), 50*time.Millisecond)
			defer cancel()
			req, _ := http.NewRequestWithContext(ctx, "GET", "http://unused/pending", nil)
			response, err := client.Do(req)
			if err == nil {
				defer response.Body.Close()
				_, err = response.Body.Read(make([]byte, 1))
			}
			if !errors.Is(err, context.DeadlineExceeded) {
				t.Fatalf("deadline became %v", err)
			}
		})
	}
}

func TestTruncatedLengthIsNotSuccessfulEOF(t *testing.T) {
	listener, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	defer listener.Close()
	go func() {
		conn, err := listener.Accept()
		if err != nil {
			return
		}
		defer conn.Close()
		b := make([]byte, 4096)
		conn.Read(b)
		fmt.Fprint(conn, "HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\nshort")
	}()
	client := &http.Client{Transport: &Transport{Endpoint: "tcp:" + listener.Addr().String()}}
	response, err := client.Get("http://unused/truncated")
	if err != nil {
		t.Fatal(err)
	}
	defer response.Body.Close()
	body, err := io.ReadAll(response.Body)
	if err == nil || string(body) != "short" {
		t.Fatalf("truncated frame body=%q err=%v", body, err)
	}
}

func TestDefaultStreamHasNoImplicitFiveSecondIdleTimeout(t *testing.T) {
	client := clientFor(t, func(w http.ResponseWriter, r *http.Request) {
		w.WriteHeader(200)
		w.(http.Flusher).Flush()
		timer := time.NewTimer(5200 * time.Millisecond)
		defer timer.Stop()
		select {
		case <-timer.C:
			fmt.Fprint(w, "later")
		case <-r.Context().Done():
		}
	})
	ctx, cancel := context.WithTimeout(context.Background(), 7*time.Second)
	defer cancel()
	req, _ := http.NewRequestWithContext(ctx, "GET", "http://unused/idle", nil)
	response, err := client.Do(req)
	if err != nil {
		t.Fatal(err)
	}
	defer response.Body.Close()
	body, err := io.ReadAll(response.Body)
	if err != nil || string(body) != "later" {
		t.Fatalf("idle response %q %v", body, err)
	}
}

type blockedRequestBody struct {
	entered chan struct{}
	closed  chan struct{}
	closes  atomic.Int32
}

func (b *blockedRequestBody) Read([]byte) (int, error) {
	close(b.entered)
	<-b.closed
	return 0, io.ErrClosedPipe
}
func (b *blockedRequestBody) Close() error {
	if b.closes.Add(1) == 1 {
		close(b.closed)
	}
	return nil
}
func TestCancellationClosesBlockedRequestBodyExactlyOnce(t *testing.T) {
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	body := &blockedRequestBody{entered: make(chan struct{}), closed: make(chan struct{})}
	req, _ := http.NewRequestWithContext(ctx, "POST", "http://unused/", body)
	done := make(chan error, 1)
	go func() { _, err := (&Transport{Endpoint: "tcp:127.0.0.1:1"}).RoundTrip(req); done <- err }()
	select {
	case <-body.entered:
	case <-time.After(2 * time.Second):
		t.Fatal("request body reader not entered")
	}
	cancel()
	select {
	case err := <-done:
		if !errors.Is(err, context.Canceled) {
			t.Fatalf("cancel %v", err)
		}
	case <-time.After(2 * time.Second):
		t.Fatal("blocked request reader survived cancellation")
	}
	if body.closes.Load() != 1 {
		t.Fatalf("request body closed %d times", body.closes.Load())
	}
}
