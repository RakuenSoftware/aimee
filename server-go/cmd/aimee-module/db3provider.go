package main

import (
	"context"
	"fmt"
	"log"
	"os"
	"strconv"
	"strings"

	"github.com/JBailes/aimee/server-go/bus"
	"github.com/JBailes/aimee/server-go/db3"
	"github.com/JBailes/aimee/server-go/modules/vectordb"
)

// DB3 vector providers run out of this multicall, like plugin instances and for
// the same reason: the set is a deployment decision rather than a compile-time
// list, so they are matched by prefix and instanced.
//
// WHY THIS FILE EXISTS. The vectordb package, its index, its provider, its
// router and its wire were all written and tested, and none of it could be
// DEPLOYED: nothing in this multicall knew the name, so there was no executable
// for `provision-plugin-module.py --kind db3-provider` to point a grant at. The
// externalization path was proven in-process and over a bus with a fake search
// function, and the one thing it could not be was installed. This is the
// missing process.
//
// It does NOT go through bus.RunModuleProcess. That serves module-runtime
// stages -- an invoke kind and a handler -- and a DB3 provider serves none of
// them. It answers the DB3 SEARCH request and publishes CAPABILITIES and
// APPLIED, which is what db3.RunProvider does over a plain attached client. A
// provider forced into the module-runtime shape would have to declare stages it
// never serves.
const db3ProviderPrefix = "db3-"

// db3ProviderConfig is what an operator may choose about a provider instance.
//
// The dimension MUST match the corpus the collection holds. A provider that
// answers at the wrong width does not fail loudly: every vector is rejected by
// the index and the search returns nothing, which reads as an empty corpus. So
// it is required rather than defaulted.
type db3ProviderConfig struct {
	instance   string
	ref        uint32
	collection string
	dimension  int
	metric     vectordb.Metric
}

func db3MetricFromEnv(raw string) (vectordb.Metric, error) {
	switch strings.ToLower(strings.TrimSpace(raw)) {
	case "", "cosine":
		return vectordb.Cosine, nil
	case "l2":
		return vectordb.L2, nil
	case "dot":
		return vectordb.Dot, nil
	default:
		return vectordb.Cosine, fmt.Errorf("AIMEE_DB3_METRIC=%q is not cosine, l2, or dot", raw)
	}
}

func db3ProviderConfigFromEnv(instance string) (db3ProviderConfig, error) {
	config := db3ProviderConfig{instance: instance}

	ref, err := principalRefFromEnv()
	if err != nil {
		return config, err
	}
	// The band is the allocation authority, and it is checked HERE as well as in
	// the router. A provider that attached on a canonical module's ref would
	// derive that module's event kinds, and bus_host_serve_kind binds one kind
	// to one slot -- so the loser is the core module, denied at attach with
	// nothing in its own log to say why.
	if err := db3.ValidateProviderRef(ref); err != nil {
		return config, err
	}
	config.ref = ref

	config.collection = strings.TrimSpace(os.Getenv("AIMEE_DB3_COLLECTION"))
	if config.collection == "" {
		return config, fmt.Errorf("AIMEE_DB3_COLLECTION is unset; a provider serves exactly one collection")
	}

	raw := strings.TrimSpace(os.Getenv("AIMEE_DB3_DIMENSION"))
	if raw == "" {
		return config, fmt.Errorf("AIMEE_DB3_DIMENSION is unset; it must match the corpus width")
	}
	dimension, err := strconv.Atoi(raw)
	if err != nil || dimension <= 0 {
		return config, fmt.Errorf("AIMEE_DB3_DIMENSION=%q is not a positive integer", raw)
	}
	config.dimension = dimension

	config.metric, err = db3MetricFromEnv(os.Getenv("AIMEE_DB3_METRIC"))
	if err != nil {
		return config, err
	}
	return config, nil
}

// runDB3Provider attaches as a vector provider and serves until ctx ends.
func runDB3Provider(ctx context.Context, instance, socketPath string) error {
	config, err := db3ProviderConfigFromEnv(instance)
	if err != nil {
		return fmt.Errorf("db3 provider %s: %w", instance, err)
	}

	client, err := bus.ConnectClient(ctx, socketPath, 1, config.ref)
	if err != nil {
		return fmt.Errorf("db3 provider %s: attach as principal %d: %w",
			instance, config.ref, err)
	}
	defer client.Detach()

	index := vectordb.NewIndex(config.metric, config.dimension)
	provider := vectordb.NewProvider(index, config.collection)
	log.Printf("db3 provider %s: serving collection %q at %d dimensions as principal %d",
		instance, config.collection, config.dimension, config.ref)
	return provider.Run(ctx, client)
}
