package observability

import (
	"context"
	"fmt"
	"log/slog"

	"github.com/JBailes/aimee/server-go/bus"
	"go.opentelemetry.io/otel/attribute"
	"go.opentelemetry.io/otel/metric"
)

// TapObserver turns the ordered bus tap into content-free telemetry. Payload
// data is deliberately never attached to a log, metric, or trace.
type TapObserver struct {
	log        *slog.Logger
	events     metric.Int64Counter
	bytes      metric.Int64Counter
	exceptions metric.Int64Counter
}

func NewTapObserver(telemetry Telemetry) (*TapObserver, error) {
	meter := telemetry.Meter("github.com/JBailes/aimee/server-go/observability/tap")
	events, err := meter.Int64Counter("aimee.tap.events",
		metric.WithDescription("Events accepted by the ordered event-bus tap"))
	if err != nil {
		return nil, err
	}
	bytes, err := meter.Int64Counter("aimee.tap.payload.bytes",
		metric.WithDescription("Payload bytes observed by the event-bus tap"),
		metric.WithUnit("By"))
	if err != nil {
		return nil, err
	}
	exceptions, err := meter.Int64Counter("aimee.tap.exceptions",
		metric.WithDescription("Overflow, producer-reap, and invalid tap events"))
	if err != nil {
		return nil, err
	}
	return &TapObserver{
		log:    telemetry.Logger("github.com/JBailes/aimee/server-go/observability/tap"),
		events: events, bytes: bytes, exceptions: exceptions,
	}, nil
}

// Observe records one accepted event. Labels are bounded protocol vocabulary;
// sequence and handles are log attributes only, avoiding unbounded metric
// cardinality. The payload slice is used only to verify its declared length.
func (t *TapObserver) Observe(ctx context.Context, event bus.Event) {
	frame := event.Frame
	kind := fmt.Sprintf("%d", frame.EventKind)
	pattern := framePattern(frame.HdrFlags)
	placement := framePlacement(frame.HdrFlags)
	status := "accepted"
	if frame.Validate() != bus.OK ||
		(frame.HdrFlags&bus.FInline != 0 && uint32(len(event.Payload)) != frame.PayloadLen) {
		status = "invalid"
	}
	attrs := []attribute.KeyValue{
		attribute.String("event.kind", kind),
		attribute.String("message.type", pattern),
		attribute.String("payload.placement", placement),
		attribute.String("tap.status", status),
	}
	t.events.Add(ctx, 1, metric.WithAttributes(attrs...))
	t.bytes.Add(ctx, int64(frame.PayloadLen), metric.WithAttributes(attrs...))

	exception := status == "invalid" || frame.EventKind == bus.KindOverflow ||
		frame.EventKind == bus.KindProducerReaped
	if exception {
		t.exceptions.Add(ctx, 1, metric.WithAttributes(
			attribute.String("event.kind", kind),
			attribute.String("tap.status", status)))
	}

	level := slog.LevelInfo
	if exception {
		level = slog.LevelWarn
	}
	t.log.LogAttrs(ctx, level, "event bus tap accepted event",
		slog.String("event.kind", kind),
		slog.String("message.type", pattern),
		slog.String("payload.placement", placement),
		slog.String("tap.status", status),
		slog.Uint64("event.sequence", frame.Seq),
		slog.Uint64("event.payload_bytes", uint64(frame.PayloadLen)),
		slog.Uint64("event.source_handle", uint64(frame.SrcHandle)))
}

func framePattern(flags uint16) string {
	switch {
	case flags&bus.FNotification != 0:
		return "notification"
	case flags&bus.FRequest != 0:
		return "request"
	case flags&bus.FReply != 0:
		return "reply"
	case flags&bus.FCancel != 0:
		return "cancel"
	default:
		return "invalid"
	}
}

func framePlacement(flags uint16) string {
	switch {
	case flags&bus.FInline != 0:
		return "inline"
	case flags&bus.FArena != 0:
		return "arena"
	default:
		return "none"
	}
}
