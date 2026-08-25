package main

import (
	"strings"
	"testing"

	"github.com/JBailes/aimee/server-go/db3"
	"github.com/JBailes/aimee/server-go/modules/vectordb"
)

// The provider's configuration is where a silent failure would live, so every
// refusal below is a value that would otherwise produce a provider that attaches
// happily and answers nothing.

func TestDB3ProviderRefusesARefOutsideTheReservedBand(t *testing.T) {
	// Kinds are derived from the ref. A provider on a canonical module's ref
	// derives that module's block, and one kind has exactly one serving slot --
	// so the CORE module is the one denied at attach, with nothing in its own
	// log to explain it. 28, 29 and 30 are postgres, db2 and db1.
	t.Setenv("AIMEE_DB3_COLLECTION", "memory")
	t.Setenv("AIMEE_DB3_DIMENSION", "384")
	for _, bad := range []string{"1", "28", "29", "30", "199", "455", "512", "1000"} {
		t.Setenv("AIMEE_MODULE_PRINCIPAL_REF", bad)
		if _, err := db3ProviderConfigFromEnv("qdrant"); err == nil {
			t.Errorf("accepted out-of-band principal ref %q", bad)
		}
	}
	// The band's own endpoints are the cases an off-by-one gets wrong.
	for _, good := range []string{"456", "511"} {
		t.Setenv("AIMEE_MODULE_PRINCIPAL_REF", good)
		if _, err := db3ProviderConfigFromEnv("qdrant"); err != nil {
			t.Errorf("refused in-band principal ref %q: %v", good, err)
		}
	}
}

func TestDB3ProviderRefusesToStartWithoutAProvisionedPrincipal(t *testing.T) {
	t.Setenv("AIMEE_DB3_COLLECTION", "memory")
	t.Setenv("AIMEE_DB3_DIMENSION", "384")
	for _, bad := range []string{"", "0", "-1", "notanumber", "4294967296"} {
		t.Setenv("AIMEE_MODULE_PRINCIPAL_REF", bad)
		if _, err := db3ProviderConfigFromEnv("qdrant"); err == nil {
			t.Errorf("accepted principal ref %q", bad)
		}
	}
}

func TestDB3ProviderRequiresACollectionAndAWidth(t *testing.T) {
	// Both are required rather than defaulted, and for the same reason: a
	// provider serving the wrong collection answers confidently from the wrong
	// corpus, and one at the wrong width rejects every vector so the search
	// returns nothing and reads as an empty corpus. Neither fails loudly later.
	t.Setenv("AIMEE_MODULE_PRINCIPAL_REF", "456")

	t.Setenv("AIMEE_DB3_DIMENSION", "384")
	t.Setenv("AIMEE_DB3_COLLECTION", "")
	if _, err := db3ProviderConfigFromEnv("qdrant"); err == nil {
		t.Error("accepted an empty collection")
	}

	t.Setenv("AIMEE_DB3_COLLECTION", "memory")
	for _, bad := range []string{"", "0", "-4", "wide"} {
		t.Setenv("AIMEE_DB3_DIMENSION", bad)
		if _, err := db3ProviderConfigFromEnv("qdrant"); err == nil {
			t.Errorf("accepted dimension %q", bad)
		}
	}
}

func TestDB3ProviderReadsItsMetric(t *testing.T) {
	t.Setenv("AIMEE_MODULE_PRINCIPAL_REF", "456")
	t.Setenv("AIMEE_DB3_COLLECTION", "memory")
	t.Setenv("AIMEE_DB3_DIMENSION", "384")
	for raw, want := range map[string]vectordb.Metric{
		"": vectordb.Cosine, "cosine": vectordb.Cosine, "COSINE": vectordb.Cosine,
		"l2": vectordb.L2, "dot": vectordb.Dot,
	} {
		t.Setenv("AIMEE_DB3_METRIC", raw)
		config, err := db3ProviderConfigFromEnv("qdrant")
		if err != nil {
			t.Fatalf("metric %q: %v", raw, err)
		}
		if config.metric != want {
			t.Errorf("metric %q = %v, want %v", raw, config.metric, want)
		}
	}
	// An unrecognised metric is refused rather than silently treated as cosine:
	// scoring a corpus by the wrong function returns plausible neighbours that
	// are the wrong ones, which no caller can detect.
	t.Setenv("AIMEE_DB3_METRIC", "euclidean")
	if _, err := db3ProviderConfigFromEnv("qdrant"); err == nil {
		t.Error("accepted an unrecognised metric")
	}
}

func TestDB3ProviderNameIsMatchedByPrefixAndIsNotAModule(t *testing.T) {
	// The dispatch is by prefix because the set of providers is a deployment
	// decision, and it happens BEFORE the module table because a provider serves
	// no module-runtime stage. If the module table ever claimed the name, the
	// provider would be started as a module and serve nothing.
	name := strings.TrimPrefix("aimee-module-db3-qdrant", "aimee-module-")
	instance, ok := strings.CutPrefix(name, db3ProviderPrefix)
	if !ok || instance != "qdrant" {
		t.Fatalf("prefix match gave (%q, %v), want (\"qdrant\", true)", instance, ok)
	}
	if _, isModule := moduleConfig("aimee-module-db3-qdrant"); isModule {
		t.Error("the module table claims a db3 provider name")
	}
}

func TestDB3ProviderBandMatchesTheWireContract(t *testing.T) {
	// The refusal above is only meaningful while these are the same band the
	// router enforces. Two copies of a band is how they drift.
	if err := db3.ValidateProviderRef(db3.ProviderRefFirst); err != nil {
		t.Fatalf("ProviderRefFirst is refused by its own validator: %v", err)
	}
	if err := db3.ValidateProviderRef(db3.ProviderRefLimit); err == nil {
		t.Fatal("ProviderRefLimit is accepted; the band is inclusive at the top")
	}
}

func TestDB3ProviderSelectsItsBackend(t *testing.T) {
	t.Setenv("AIMEE_MODULE_PRINCIPAL_REF", "456")
	t.Setenv("AIMEE_DB3_COLLECTION", "memory")
	t.Setenv("AIMEE_DB3_DIMENSION", "384")

	// Default is in-process, because it needs no external service. It is right
	// for a smoke test and wrong for anything else, which is why the log names
	// which backend is serving.
	t.Setenv("AIMEE_DB3_BACKEND", "")
	config, err := db3ProviderConfigFromEnv("probe")
	if err != nil || config.backend != "memory" {
		t.Fatalf("default backend = %q (%v), want memory", config.backend, err)
	}

	// Qdrant needs an address, and a missing one is refused rather than
	// defaulted to localhost: a provider silently pointed at the wrong store
	// answers confidently from an empty one.
	t.Setenv("AIMEE_DB3_BACKEND", "qdrant")
	t.Setenv("AIMEE_DB3_QDRANT_URL", "")
	if _, err := db3ProviderConfigFromEnv("probe"); err == nil {
		t.Error("the qdrant backend was accepted with no URL")
	}

	t.Setenv("AIMEE_DB3_QDRANT_URL", "http://127.0.0.1:6333")
	config, err = db3ProviderConfigFromEnv("probe")
	if err != nil {
		t.Fatal(err)
	}
	if config.backend != "qdrant" || config.qdrant.URL != "http://127.0.0.1:6333" {
		t.Fatalf("qdrant config = %+v", config.qdrant)
	}
	// The store must be built at the same width and metric the provider
	// advertises, or DB2 is told one thing and answered by another.
	if config.qdrant.Dimension != config.dimension || config.qdrant.Metric != config.metric {
		t.Errorf("qdrant width/metric %d/%v disagree with the provider's %d/%v",
			config.qdrant.Dimension, config.qdrant.Metric, config.dimension, config.metric)
	}
	if _, err := newBackend(config); err != nil {
		t.Errorf("building the qdrant backend: %v", err)
	}

	t.Setenv("AIMEE_DB3_BACKEND", "milvus")
	if _, err := db3ProviderConfigFromEnv("probe"); err == nil {
		t.Error("an unimplemented backend name was accepted")
	}
}
