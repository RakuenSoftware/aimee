package memory

import (
	"context"
	"errors"
	"fmt"
	"log"
	"os"
	"strings"
	"time"

	configclient "github.com/JBailes/aimee/server-go/config"
	store "github.com/JBailes/aimee/server-go/db"
	contract "github.com/JBailes/aimee/server-go/memory"
	"github.com/JBailes/aimee/server-go/modules/audit"
	"github.com/JBailes/aimee/server-go/modules/egress"
	"github.com/RakuenSoftware/ouroboros-ring/go/ring"
)

// This outbound identity reaches only the same daemon's storage/configuration
// and content-free ACTION publication. It is distinct from serving principal 7.
const processStorePrincipalRef uint32 = 73

type processResources struct {
	store.Store
	config         *configclient.Client
	auditPublisher audit.Publisher
}

func (r processResources) MemoryAuditAction(ctx context.Context, action audit.Action) error {
	return audit.PublishAction(ctx, r.auditPublisher, action)
}

// configStartupWait bounds how long a starting module waits for the config
// module, which its supervisor starts at the same moment, to serve.
const configStartupWait = 30 * time.Second

func (r processResources) MemorySettings() (map[string]any, error) { return r.config.Snapshot() }

// OnConfigChange has fn called after the configuration changes: the personal
// indexer re-reads its model endpoint then instead of on a timer.
func (r processResources) OnConfigChange(fn func()) { r.config.OnChange(fn) }
func (r processResources) EmbeddingEndpoint() (string, error) {
	values, err := r.config.Snapshot()
	if err != nil {
		return "", err
	}
	if endpoint, ok := values["embedder_url"].(string); ok && endpoint != "" {
		return endpoint, nil
	}
	if model, ok := values["embedder_model"].(string); ok && model != "" {
		return "https://aimee-embedder:8762", nil
	}
	return os.Getenv("EMBEDDER_URL"), nil
}

func openProcessResources(ctx context.Context, socket string) (processResources, func(), error) {
	client, err := ring.ConnectClient(ctx, socket, 1, processStorePrincipalRef)
	if err != nil {
		return processResources{}, nil, err
	}
	caller, err := ring.NewConcurrentModuleCaller(ctx, client)
	if err != nil {
		client.Detach()
		return processResources{}, nil, err
	}
	closeConnection := func() { caller.CloseAndWait(); client.Detach() }
	db, err := store.NewStore(caller)
	if err != nil {
		closeConnection()
		return processResources{}, nil, err
	}
	config, err := configclient.NewClient(caller, 5*time.Second)
	if err != nil {
		closeConnection()
		return processResources{}, nil, err
	}
	// Cache what is read and drop it when the config module announces a change.
	config.Subscribe(caller)
	return processResources{Store: db, config: config, auditPublisher: client}, closeConnection, nil
}

func processEgress(ctx context.Context, socket string) egress.Client {
	client, err := ring.ConnectClient(ctx, socket, 1, egress.MemoryClientRef)
	if err != nil {
		log.Printf("memory egress unavailable: %v", err)
		return nil
	}
	caller, err := ring.NewConcurrentModuleCaller(ctx, client)
	if err != nil {
		client.Detach()
		log.Printf("memory egress unavailable: %v", err)
		return nil
	}
	transport, err := egress.NewBusAuthorizer(caller)
	if err != nil {
		caller.CloseAndWait()
		client.Detach()
		log.Printf("memory egress unavailable: %v", err)
		return nil
	}
	return transport
}

// NewProcessHandler constructs the one memory service used by multicall and
// standalone processes in either placement. Placement is mandatory; a missing
// storage connection never silently becomes a handler without a store.
// Outbound callers and index workers follow ctx. Their shared-memory mappings
// remain valid for the process lifetime, including in-flight audit publication.
func NewProcessHandler(ctx context.Context, socket, placementName string) (ring.ModuleHandler, error) {
	if ctx == nil || socket == "" {
		return nil, errors.New("memory: module context and bus socket required")
	}
	placement, err := ParsePlacement(placementName)
	if err != nil {
		return nil, err
	}
	resources, closeConnection, err := openProcessResources(ctx, socket)
	if err != nil {
		return nil, fmt.Errorf("memory: config/storage/audit bus connection: %w", err)
	}
	settings, err := resources.config.SnapshotWhenServed(ctx, configStartupWait)
	if err != nil {
		closeConnection()
		return nil, fmt.Errorf("memory: backend configuration: %w", err)
	}
	if err = contract.ApplyBackendSettings(settings); err != nil {
		closeConnection()
		return nil, fmt.Errorf("memory: backend configuration: %w", err)
	}
	backendName := strings.TrimSpace(os.Getenv("AIMEE_MEMORY_BACKEND"))
	if backendName != "" && backendName != "native" && backendName != "aimee-native" {
		executor := processEgress(ctx, socket)
		data, err := configuredExternalStore(executor, placement)
		if err != nil {
			closeConnection()
			return nil, err
		}
		if err = resetDerivedBackend(ctx, data); err != nil {
			closeConnection()
			return nil, err
		}
		data.auditAction = resources.MemoryAuditAction
		log.Printf("memory module: placement=%s storage=backend-owned backend=%s", placement, backendName)
		return NewHandler(executor, WithDataStore(placement, data), func(options *handlerOptions) { options.dataContext = ctx }), nil
	}
	data, err := NewPostgresDataStore(resources, placement)
	if err != nil {
		closeConnection()
		return nil, err
	}
	// Private memory also opens the store directly. It must not race the Aimee
	// owner's migration/replay and serve a content snapshot before surviving
	// erasure intents have been applied. Both owners use the storage contract;
	// neither imports the other's migration or admission implementation.
	if placement == PlacementServer {
		replayCtx, cancel := context.WithTimeout(ctx, 2*time.Minute)
		err = waitPrivateErasureReplay(replayCtx, resources)
		cancel()
		if err != nil {
			closeConnection()
			return nil, fmt.Errorf("memory: private erasure replay: %w", err)
		}
	}
	// As before, governed model/embedding execution may be unavailable while
	// deterministic memory operations remain usable. Operations requiring egress
	// report its absence through the normal owner contracts.
	executor := processEgress(ctx, socket)
	factory, err := configuredMemoryBackend(executor)
	if err != nil {
		closeConnection()
		return nil, err
	}
	data.(*postgresDataStore).backendFactory = factory
	if factory != nil {
		replayCtx, cancel := context.WithTimeout(ctx, 2*time.Minute)
		err = resetDerivedBackend(replayCtx, data)
		cancel()
		if err != nil {
			closeConnection()
			return nil, fmt.Errorf("memory: derived erasure replay: %w", err)
		}
	}
	StartPersonalIndex(ctx, data, executor, os.Getenv("EMBEDDER_URL"))
	StartSharedIndex(ctx, data, executor)
	backendName = "aimee-native"
	if factory != nil {
		backendName = os.Getenv("AIMEE_MEMORY_BACKEND")
	}
	log.Printf("memory module: placement=%s storage=postgres backend=%s", placement, backendName)
	return NewHandler(executor, WithDataStore(placement, data), func(options *handlerOptions) { options.dataContext = ctx }), nil
}

func waitPrivateErasureReplay(ctx context.Context, db store.Store) error {
	for {
		var removed int64
		err := db.QueryRow(ctx, `SELECT user_memory_replay_erasure_intents()`).Scan(&removed)
		if err == nil {
			return nil
		}
		// On a fresh store the Aimee owner may still be installing the contract.
		// No memory handler or index worker is published during this wait.
		timer := time.NewTimer(100 * time.Millisecond)
		select {
		case <-ctx.Done():
			timer.Stop()
			return fmt.Errorf("%w (last storage error: %v)", ctx.Err(), err)
		case <-timer.C:
		}
	}
}
