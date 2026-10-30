package providers

import (
	"context"
	"time"

	configclient "github.com/JBailes/aimee/server-go/config"
	"github.com/JBailes/aimee/server-go/modules/egress"
	"github.com/JBailes/aimee/server-go/modules/providers/vaultresource"
	"github.com/RakuenSoftware/ouroboros-ring/go/ring"
)

func NewProcessHandler(ctx context.Context, socket, home string) (ring.ModuleHandler, error) {
	store, err := NewStore(home)
	if err != nil {
		return nil, err
	}
	if socket == "" {
		return NewManager(store, nil).Handle, nil
	}
	client, err := ring.ConnectClient(ctx, socket, 1, egress.ProvidersClientRef)
	if err != nil {
		return nil, err
	}
	caller, err := ring.NewConcurrentModuleCaller(ctx, client)
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
	manager.SetEgress(transport)
	manager.SetConfig(config)
	return manager.Handle, nil
}
