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
	"context"
	"encoding/binary"
	"errors"
	"time"

	"github.com/JBailes/aimee/server-go/bus"
)

// How long a health probe may spend reaching storage. Short, because a health
// answer that arrives after the caller gave up is not an answer.
const probeTimeout = 400 * time.Millisecond

// ErrMalformedReply reports a reply this module did not write.
var ErrMalformedReply = errors.New("control-plane: malformed health reply")

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

	// The flag word. StorageReachable says the postgres module ANSWERED and this
	// module's own schema is recorded -- evidence rather than a guess, set from a
	// migration and a version read, both over the bus.
	//
	// It deliberately does not say the database is healthy. That is the postgres
	// module's question and it already answers it, and two modules with an
	// opinion about one fact eventually disagree.
	flagStorageReachable = uint32(1 << 0)
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

// DecodeHealthReply reads what the stage answered.
func DecodeHealthReply(reply []byte) (Ready, error) {
	if len(reply) != responseLen ||
		binary.LittleEndian.Uint32(reply[0:4]) != responseMagic ||
		binary.LittleEndian.Uint32(reply[4:8]) != wireVersion {
		return Ready{}, ErrMalformedReply
	}
	flags := binary.LittleEndian.Uint32(reply[8:12])
	return Ready{StorageReachable: flags&flagStorageReachable != 0}, nil
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

	// Bounded by the caller's deadline: reaching storage is a bus round trip,
	// and a probe that outlives the call asking for it holds a connection for an
	// answer nobody is waiting for.
	ctx := context.Background()
	if remaining := invocation.Remaining(probeTimeout); remaining > 0 {
		var cancel context.CancelFunc
		ctx, cancel = context.WithTimeout(ctx, remaining)
		defer cancel()
	}
	reachable, _ := storageEvidence(ctx)

	reply := make([]byte, responseLen)
	binary.LittleEndian.PutUint32(reply[0:4], responseMagic)
	binary.LittleEndian.PutUint32(reply[4:8], wireVersion)
	var flags uint32
	if reachable {
		flags |= flagStorageReachable
	}
	binary.LittleEndian.PutUint32(reply[8:12], flags)
	return reply, bus.ModuleStatusOK
}

// EncodeHealthRequest builds the request a caller sends.
func EncodeHealthRequest() []byte {
	request := make([]byte, requestLen)
	binary.LittleEndian.PutUint32(request[0:4], requestMagic)
	binary.LittleEndian.PutUint32(request[4:8], wireVersion)
	return request
}
