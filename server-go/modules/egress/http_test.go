package egress

import (
	"encoding/json"
	"net"
	"net/http"
	"net/http/httptest"
	"sync/atomic"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
)

func TestHTTPStageExecutesBoundBytesAndDoesNotFollowRedirects(t *testing.T) {
	var escaped atomic.Int32
	evil := httptest.NewServer(http.HandlerFunc(func(http.ResponseWriter, *http.Request) { escaped.Add(1) }))
	defer evil.Close()
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path == "/embed" && r.URL.Query().Has("redirect") {
			http.Redirect(w, r, evil.URL, http.StatusFound)
			return
		}
		w.WriteHeader(http.StatusCreated)
		_, _ = w.Write([]byte(`[1,2,3]`))
	}))
	defer server.Close()
	handler := newHandler(fixedResolver{{IP: net.ParseIP("127.0.0.1")}})
	call := func(target string) ([]byte, bus.ModuleStatus) {
		request := HTTPRequest{Request: Request{TargetURL: target, Purpose: "embedding", Method: "POST",
			RequestSHA256: RequestDigest("POST", target, []byte("text"), false)},
			Headers: map[string]string{"Content-Type": "text/plain"}, Body: []byte("text"),
			MaxResponseBytes: 1024, TimeoutMS: 1000}
		body, _ := json.Marshal(request)
		return handler(bus.ModuleInvocation{PrincipalClass: 1, PrincipalRef: MemoryClientRef,
			StageID: StageHTTP}, body)
	}
	reply, status := call(server.URL + "/embed")
	response, err := decodeHTTPResponse(reply)
	if status != bus.ModuleStatusOK || err != nil || response.Status != http.StatusCreated || string(response.Body) != `[1,2,3]` {
		t.Fatalf("status=%d response=%+v err=%v", status, response, err)
	}
	reply, status = call(server.URL + "/embed?redirect=1")
	response, err = decodeHTTPResponse(reply)
	if status != bus.ModuleStatusOK || err != nil || response.Status != http.StatusFound || response.Location != evil.URL {
		t.Fatalf("redirect status=%d response=%+v err=%v", status, response, err)
	}
	if escaped.Load() != 0 {
		t.Fatal("egress followed a redirect without a new governed request")
	}
}

func TestHTTPStageRejectsRequestDigestDrift(t *testing.T) {
	handler := newHandler(fixedResolver{{IP: net.ParseIP("127.0.0.1")}})
	request := HTTPRequest{Request: Request{TargetURL: "http://127.0.0.1:1/embed", Purpose: "embedding",
		Method: "POST", RequestSHA256: RequestDigest("POST", "http://127.0.0.1:1/embed", []byte("before"), false)},
		Body: []byte("after"), MaxResponseBytes: 32, TimeoutMS: 1000}
	body, _ := json.Marshal(request)
	if _, status := handler(bus.ModuleInvocation{PrincipalClass: 1, PrincipalRef: MemoryClientRef,
		StageID: StageHTTP}, body); status != bus.ModuleStatusInvalidRequest {
		t.Fatalf("tampered bytes status=%d", status)
	}
}
