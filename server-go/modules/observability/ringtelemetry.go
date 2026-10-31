package observability

import (
	"context"
	"errors"
	"fmt"
	"log/slog"
	"os"

	"github.com/RakuenSoftware/ouroboros-ring/go/otlpexport"
	"github.com/RakuenSoftware/ouroboros-ring/go/ring"
	"go.opentelemetry.io/otel/exporters/otlp/otlplog/otlploghttp"
)

// RingTelemetryPrincipalRef is the ring principal of the telemetry exporter,
// the "telemetry" entry in src/modules/process-contracts.json. Its grant file
// says telemetry=yes, so the host hands this process, and only it, the D16
// consumer end of its telemetry ring.
const (
	RingTelemetryPrincipalClass uint32 = 1
	RingTelemetryPrincipalRef   uint32 = 77
)

// RunRingTelemetry is the process that takes every event off the host the
// moment the ring host stamps it (ouroboros ring D16). The host writes one
// content-free record per event into the telemetry ring before the tap runs;
// this process sends them to the OTLP collector as soon as it is woken, under
// the same collector policy as every Aimee exporter (validateOTLPEnvironment).
//
// With no collector configured there is nowhere off the host to send them. It
// says so once and sleeps until stopped, without attaching: the host keeps the
// newest records and counts the rest, and an exporter started later with a
// collector sends what is still held, preceded by a gap record for the rest.
func RunRingTelemetry(ctx context.Context, socket, serviceVersion string) error {
	cfg := ConfigFromEnv("aimee-ring", serviceVersion)
	cfg.ServiceNamespace = "aimee"
	if err := validateOTLPEnvironment(cfg); err != nil {
		return err
	}
	logger := slog.New(slog.NewJSONHandler(os.Stderr, &slog.HandlerOptions{Level: slog.LevelInfo}))
	if !cfg.OTLPEnabled && !cfg.OTLPLogsEnabled {
		logger.Warn("ring telemetry: no OTLP logs endpoint is configured " +
			"(OTEL_EXPORTER_OTLP_ENDPOINT or OTEL_EXPORTER_OTLP_LOGS_ENDPOINT); " +
			"ring events are not leaving this host")
		<-ctx.Done()
		return nil
	}
	res, err := serviceResource(cfg)
	if err != nil {
		return err
	}
	// Records stay in the ring until the collector accepts them, so the SDK
	// must not queue and retry them as well.
	out, err := otlploghttp.New(ctx, otlploghttp.WithRetry(otlploghttp.RetryConfig{Enabled: false}))
	if err != nil {
		return fmt.Errorf("ring telemetry: OTLP logs exporter: %w", err)
	}
	defer out.Shutdown(context.Background())
	client, reader, err := ring.ConnectTelemetry(ctx, socket, RingTelemetryPrincipalClass, RingTelemetryPrincipalRef)
	if err != nil {
		return fmt.Errorf("ring telemetry: attach %s: %w", socket, err)
	}
	defer client.Detach()
	defer reader.Close()
	report := func(err error) { logger.Warn("ring telemetry: " + err.Error()) }
	err = otlpexport.New(out, res).Run(ctx, reader, report)
	if errors.Is(err, ring.ErrEpoch) {
		// The supervisor restarts this process, which attaches to the new host.
		return errors.New("ring telemetry: the host went away")
	}
	return err
}
