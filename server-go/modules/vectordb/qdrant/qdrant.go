// Package qdrant serves a DB3 vector provider out of a Qdrant instance.
//
// It is one implementation of vectordb.Backend, and the first one that is not
// in-process. Everything specific to Qdrant lives here: its REST shapes, its
// filter language, and the two places its semantics do not line up with the DB3
// contract (see Tombstone and score below). The provider, the wire, the scope
// filtering and the authority split are all unchanged, which is the point of
// the backend seam -- adding Milvus means another package like this one and
// nothing else.
//
// Talked to over its REST API with the standard library rather than the vendor
// SDK. The surface used here is five endpoints; an SDK would be a dependency,
// a release cadence and a transitive tree for that.
package qdrant

import (
	"errors"
	"fmt"
	"net/http"
	"strings"
	"sync"
	"time"

	"github.com/JBailes/aimee/server-go/modules/vectordb"
)

// tombstonePayloadKey marks a point that DB2 has tombstoned.
//
// Qdrant has no tombstone: it has points and it has their absence. DB2's
// tombstone is neither -- it keeps the identity while making the point
// unreachable, so the id is never reused for something else. Modelling it as a
// payload flag that every search excludes preserves both halves, where deleting
// would preserve only one.
//
// The name is prefixed so it cannot collide with a label DB2 projects. A label
// called "tombstoned" would otherwise be able to resurrect a point.
const tombstonePayloadKey = "__aimee_tombstoned"

// ErrConfig reports a backend that cannot be built as asked.
var ErrConfig = errors.New("qdrant: invalid configuration")

// Config is what a deployment chooses about a Qdrant backend.
type Config struct {
	// URL is the base address, e.g. http://127.0.0.1:6333.
	URL string
	// APIKey is sent as api-key when set.
	APIKey string
	// Dimension is the vector width. It must match the corpus; see
	// vectordb.Backend for why this is required rather than discovered.
	Dimension int
	// Metric is how the store scores.
	Metric vectordb.Metric
	// CollectionPrefix namespaces DB3 collections inside one Qdrant, so an
	// instance may serve more than one deployment without their points mixing.
	CollectionPrefix string
	// Timeout bounds every request. A provider that hung would hold DB2's
	// search until its own deadline instead of failing over to pgvector.
	Timeout time.Duration
	// HTTPClient is injectable for tests.
	HTTPClient *http.Client
}

// Backend is a vectordb.Backend served by Qdrant.
type Backend struct {
	config Config
	client *http.Client
	// ensured remembers collections already created, so the create call is made
	// once rather than before every write.
	ensured sync.Map
}

// New builds a Qdrant-backed store.
func New(config Config) (*Backend, error) {
	if strings.TrimSpace(config.URL) == "" {
		return nil, fmt.Errorf("%w: URL is empty", ErrConfig)
	}
	if config.Dimension <= 0 {
		return nil, fmt.Errorf("%w: dimension %d is not positive", ErrConfig, config.Dimension)
	}
	if config.Timeout <= 0 {
		config.Timeout = 10 * time.Second
	}
	config.URL = strings.TrimRight(config.URL, "/")
	backend := &Backend{config: config, client: config.HTTPClient}
	if backend.client == nil {
		backend.client = &http.Client{Timeout: config.Timeout}
	}
	return backend, nil
}

func (b *Backend) Dimension() int          { return b.config.Dimension }
func (b *Backend) Metric() vectordb.Metric { return b.config.Metric }
func (b *Backend) Close() error            { return nil }

// distanceName is Qdrant's name for the metric this backend scores by.
func (b *Backend) distanceName() string {
	switch b.config.Metric {
	case vectordb.L2:
		return "Euclid"
	case vectordb.Dot:
		return "Dot"
	default:
		return "Cosine"
	}
}

// score converts Qdrant's score to the contract's, where larger is always
// nearer.
//
// Qdrant returns a DISTANCE for Euclid, so smaller is nearer, and a similarity
// for the other two. Passing a Euclid distance through unchanged would invert
// every ranking while still returning plausible ids -- an error no caller could
// see. The in-process index negates for the same reason.
func (b *Backend) score(raw float64) float64 {
	if b.config.Metric == vectordb.L2 {
		return -raw
	}
	return raw
}

func (b *Backend) collectionName(collection string) string {
	if b.config.CollectionPrefix == "" {
		return collection
	}
	return b.config.CollectionPrefix + "_" + collection
}
