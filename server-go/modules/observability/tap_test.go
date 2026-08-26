package observability

import (
	"context"
	"log/slog"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
)

func TestTapObserverExportsMetadataWithoutPayload(t *testing.T) {
	ctx := context.Background()
	local := newCaptureHandler()
	runtime, err := New(ctx, Config{ServiceName: "tap-test", LocalHandler: local})
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = runtime.Shutdown(ctx) })
	tap, err := NewTapObserver(runtime)
	if err != nil {
		t.Fatal(err)
	}

	payload := []byte("super-secret-payload")
	tap.Observe(ctx, bus.Event{Frame: bus.Frame{
		HdrFlags: bus.FInline | bus.FNotification, WireVersion: bus.WireVersion,
		EventKind: bus.KindModuleBase, Seq: 41, PayloadLen: uint32(len(payload)), SrcHandle: 7,
		PayloadRef: bus.HdrLen,
	}, Payload: payload})

	recorder := httptest.NewRecorder()
	runtime.MetricsHandler().ServeHTTP(recorder, httptest.NewRequest(http.MethodGet, "/metrics", nil))
	body := recorder.Body.String()
	for _, metric := range []string{"aimee_tap_events_total", "aimee_tap_payload_bytes_total"} {
		if !strings.Contains(body, metric) {
			t.Errorf("missing %s in:\n%s", metric, body)
		}
	}
	if strings.Contains(body, string(payload)) {
		t.Fatal("payload leaked into Prometheus output")
	}

	if len(*local.records) != 1 {
		t.Fatalf("log records = %d, want 1", len(*local.records))
	}
	record := (*local.records)[0]
	if record.Message != "event bus tap accepted event" {
		t.Fatalf("message = %q", record.Message)
	}
	record.Attrs(func(attr slog.Attr) bool {
		if strings.Contains(attr.Value.String(), string(payload)) {
			t.Fatalf("payload leaked through attribute %s", attr.Key)
		}
		return true
	})
}

func TestTapObserverCountsInvalidFrames(t *testing.T) {
	runtime, err := New(context.Background(), Config{
		ServiceName: "tap-invalid-test", LocalHandler: newCaptureHandler(),
	})
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = runtime.Shutdown(context.Background()) })
	tap, err := NewTapObserver(runtime)
	if err != nil {
		t.Fatal(err)
	}
	tap.Observe(context.Background(), bus.Event{Frame: bus.Frame{
		HdrFlags: bus.FNotification, WireVersion: bus.WireVersion,
		EventKind: bus.KindOverflow, Seq: 42, PayloadLen: 10,
	}})
	recorder := httptest.NewRecorder()
	runtime.MetricsHandler().ServeHTTP(recorder, httptest.NewRequest(http.MethodGet, "/metrics", nil))
	if body := recorder.Body.String(); !strings.Contains(body, "aimee_tap_exceptions_total") {
		t.Fatalf("missing exception metric:\n%s", body)
	}
}
