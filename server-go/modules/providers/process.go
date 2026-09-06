package providers

import (
	"context"
	"errors"
	"os"
	"time"

	"github.com/JBailes/aimee/server-go/bus"
	configclient "github.com/JBailes/aimee/server-go/config"
	"github.com/JBailes/aimee/server-go/modules/egress"
	"github.com/JBailes/aimee/server-go/modules/providers/vaultresource"
)

func NewProcessHandler(ctx context.Context, socket, home string) (bus.ModuleHandler, error) {
	store, err := NewStore(home)
	if err != nil {
		return nil, err
	}
	if socket == "" {
		return NewManager(store, nil).Handle, nil
	}
	client, err := bus.ConnectClient(ctx, socket, 1, egress.ProvidersClientRef)
	if err != nil {
		return nil, err
	}
	caller, err := bus.NewConcurrentModuleCaller(ctx, client)
	if err != nil {
		client.Detach()
		return nil, err
	}
	transport, err := egress.NewBusAuthorizer(caller)
	if err != nil {
		caller.CloseAndWait()
		client.Detach()
		return nil, err
	}
	config, err := configclient.NewClient(caller, 5*time.Second)
	if err != nil {
		caller.CloseAndWait()
		client.Detach()
		return nil, err
	}
	manager := NewManager(store, vaultresource.VaultResources{Home: home})
	manager.worker, err = newProbeWorker(ctx, home)
	if err != nil {
		caller.CloseAndWait()
		client.Detach()
		return nil, err
	}
	manager.SetEgress(transport)
	manager.SetConfig(config)
	return manager.Handle, nil
}
func NewDefaultHandler() (bus.ModuleHandler, error) {
	if len(os.Args) != 2 {
		return nil, errors.New("module bus socket required")
	}
	return NewProcessHandler(context.Background(), os.Args[1], os.Getenv("AIMEE_HOME"))
}
