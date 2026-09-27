package memory

import (
	"context"
	"errors"
	"fmt"
	"log"
	"os"
	"time"

	"github.com/JBailes/aimee/server-go/bus"
	configclient "github.com/JBailes/aimee/server-go/config"
	store "github.com/JBailes/aimee/server-go/db"
	"github.com/JBailes/aimee/server-go/modules/audit"
	"github.com/JBailes/aimee/server-go/modules/egress"
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
func (r processResources) MemorySettings() (map[string]any, error) { return r.config.Snapshot() }
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

func openProcessStore(ctx context.Context, socket string) (processResources, func(), error) {
	client, err := bus.ConnectClient(ctx, socket, 1, processStorePrincipalRef)
	if err != nil {
		return processResources{}, nil, err
	}
	caller, err := bus.NewConcurrentModuleCaller(ctx, client)
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
	return processResources{Store: db, config: config, auditPublisher: client}, closeConnection, nil
}

func processEgress(ctx context.Context, socket string) egress.Client {
	client, err := bus.ConnectClient(ctx, socket, 1, egress.MemoryClientRef)
	if err != nil {
		log.Printf("memory egress unavailable: %v", err)
		return nil
	}
	caller, err := bus.NewConcurrentModuleCaller(ctx, client)
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
func NewProcessHandler(ctx context.Context, socket, placementName string) (bus.ModuleHandler, error) {
	if ctx == nil || socket == "" {
		return nil, errors.New("memory: module context and bus socket required")
	}
	placement, err := ParsePlacement(placementName)
	if err != nil {
		return nil, err
	}
	resources, closeConnection, err := openProcessStore(ctx, socket)
	if err != nil {
		return nil, fmt.Errorf("memory: postgres bus connection: %w", err)
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
	StartPersonalIndex(ctx, data, executor, os.Getenv("EMBEDDER_URL"))
	StartSharedIndex(ctx, data, executor)
	log.Printf("memory module: placement=%s storage=postgres", placement)
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
