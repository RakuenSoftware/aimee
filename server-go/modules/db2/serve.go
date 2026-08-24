package db2

import (
	"log"
	"sync"

	"github.com/JBailes/aimee/server-go/bus"
)

// NewLazyDispatchHandler serves the catalogue, opening the pool on first use.
//
// The store is not opened when the module starts. A module process is started
// before its database is necessarily reachable -- the KB brings PostgreSQL up
// alongside it -- and opening here would make an ordering race into a module
// that refuses to come up at all. ProductionStore does not latch a failed open,
// so a DSN corrected after start, or a database that was not up yet, recovers
// on the next call.
//
// Until it opens, every operation answers Internal rather than a wrong answer.
// The alternative -- answering "no rows" while the pool is down -- is worse
// than an error: a caller cannot tell an empty result from an absent database,
// and would record the absence as fact.
func NewLazyDispatchHandler() bus.ModuleHandler {
	return NewLazyDispatchHandlerWith(func() (Store, error) { return ProductionStore() })
}

// NewLazyDispatchHandlerWith builds the dispatcher from a store the caller
// opens lazily.
//
// Lazily because a module process starts before its database is necessarily
// reachable, and refusing to attach would make a transient into an outage; the
// first operation opens it, and a failure is logged per operation rather than
// once at startup where it would scroll away.
func NewLazyDispatchHandlerWith(open func() (Store, error)) bus.ModuleHandler {
	var (
		once     sync.Mutex
		dispatch bus.ModuleHandler
	)
	return func(invocation bus.ModuleInvocation, request []byte) ([]byte, bus.ModuleStatus) {
		once.Lock()
		if dispatch == nil {
			store, err := open()
			if err != nil {
				once.Unlock()
				// Logged every time rather than once: this is the module
				// failing to do its job, and a single line at startup would
				// scroll away while the failure continued.
				log.Printf("db2: store unavailable, operation refused: %v", err)
				return nil, bus.ModuleStatusInternal
			}
			dispatch = NewDispatchHandler(store)
		}
		handler := dispatch
		once.Unlock()
		return handler(invocation, request)
	}
}
