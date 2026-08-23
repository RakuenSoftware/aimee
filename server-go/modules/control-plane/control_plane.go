// Package controlplane serves the behaviour that belongs to aimee-kb.
//
// Three modules, three things. The postgres module owns PostgreSQL and carries
// nothing domain-specific. The aimee module owns what is specific to
// aimee-server. This owns what is specific to aimee-kb: the corpus, the
// ingest pipeline, the sketches, the documents, the KB's own tenancy.
//
// The distinction is not cosmetic. It decides where an operation runs, and
// therefore what has to cross a wire. A count-min sketch is 1 MiB and the KB's
// index build loads it, mutates it per file, and saves it -- as a db2 wire
// operation that is 2 MiB of traffic per build for work that belongs where the
// data is. Owned here, it never crosses anything.
//
// Storage comes from the postgres module over the bus. This module opens no
// pool and imports no driver: what it owns is meaning, not connections.
package controlplane

import (
	"encoding/binary"

	"github.com/JBailes/aimee/server-go/bus"
)

const (
	// Stage 1. The kind follows the registry's rule, 4097 + 256*ref +
	// (stage - 1), with control-plane at principal ref 32.
	EventHealth uint32 = 12289
	StageHealth uint32 = 1

	requestMagic  uint32 = 0x51504843 // "CHPQ"
	responseMagic uint32 = 0x52504843 // "CHPR"
	wireVersion   uint32 = 1

	requestLen  = 8
	responseLen = 12
)

// Ready reports whether this module can do its job.
//
// It answers about the module rather than about PostgreSQL. Storage health is
// the postgres module's question and it already answers it; repeating that here
// would give two modules an opinion about one fact, and they would eventually
// disagree.
type Ready struct {
	// StorageReachable is whether the postgres module answered, not whether the
	// database is healthy. A caller that needs the latter asks postgres.
	StorageReachable bool
}

// Handle serves the health stage.
func Handle(invocation bus.ModuleInvocation, body []byte) ([]byte, bus.ModuleStatus) {
	if invocation.StageID != StageHealth {
		return nil, bus.ModuleStatusCapabilityAbsent
	}
	if len(body) != requestLen {
		return nil, bus.ModuleStatusInvalidRequest
	}
	if binary.LittleEndian.Uint32(body[0:4]) != requestMagic ||
		binary.LittleEndian.Uint32(body[4:8]) != wireVersion {
		return nil, bus.ModuleStatusInvalidRequest
	}
	if invocation.Cancelled() {
		return nil, bus.ModuleStatusCancelled
	}

	reply := make([]byte, responseLen)
	binary.LittleEndian.PutUint32(reply[0:4], responseMagic)
	binary.LittleEndian.PutUint32(reply[4:8], wireVersion)
	// No capabilities are served yet, so the flag word is zero. It is present
	// from the first commit rather than added later, because a reply that grows
	// a field is a wire change and this one will grow as capabilities move in.
	binary.LittleEndian.PutUint32(reply[8:12], 0)
	return reply, bus.ModuleStatusOK
}

// EncodeHealthRequest builds the request a caller sends.
func EncodeHealthRequest() []byte {
	request := make([]byte, requestLen)
	binary.LittleEndian.PutUint32(request[0:4], requestMagic)
	binary.LittleEndian.PutUint32(request[4:8], wireVersion)
	return request
}
