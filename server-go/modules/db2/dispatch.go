package db2

import (
	"context"
	"sync"

	"github.com/JBailes/aimee/server-go/bus"
	db2contract "github.com/JBailes/aimee/server-go/db2"
)

// One handler serves 445 operations, so the shape that matters is the registry
// rather than the function.
//
// The bus routes by stage, and a stage is a family: eight of them, each holding
// up to a couple of hundred operations. The operation itself is in the request
// header, which is why an id is unique only within its family and the pair is
// what identifies an operation. Dispatch is that pair.
//
// A registry rather than a switch so the port can land one operation at a time
// without touching the dispatcher, and so an unimplemented operation answers
// capability-absent -- the same thing the C module answers for a backend it was
// not given -- instead of being a missing case somebody has to notice.

// Op is one operation's implementation. It receives the whole request frame
// because the generated decoder validates the header, and returns the whole
// reply frame because the generated encoder writes one.
type Op func(ctx context.Context, store Store, request []byte) ([]byte, bus.ModuleStatus)

type operationKey struct {
	stage     uint32
	operation uint32
}

var (
	registryMu sync.RWMutex
	registry   = map[operationKey]Op{}
)

// Register binds an implementation to a (stage, operation) pair.
//
// Registering the same pair twice is a programming error and panics at init:
// two implementations of one operation is not a condition to resolve at
// runtime, and the second would silently win.
func Register(stage, operation uint32, op Op) {
	registryMu.Lock()
	defer registryMu.Unlock()
	key := operationKey{stage: stage, operation: operation}
	if _, exists := registry[key]; exists {
		panic("db2: operation registered twice: stage " +
			itoa(stage) + " operation " + itoa(operation))
	}
	registry[key] = op
}

func lookup(stage, operation uint32) (Op, bool) {
	registryMu.RLock()
	defer registryMu.RUnlock()
	op, ok := registry[operationKey{stage: stage, operation: operation}]
	return op, ok
}

// Implemented reports how many operations have implementations, which is what
// the port's progress actually is.
func Implemented() int {
	registryMu.RLock()
	defer registryMu.RUnlock()
	return len(registry)
}

// NewDispatchHandler serves every registered operation over one stage table.
func NewDispatchHandler(store Store) bus.ModuleHandler {
	return func(invocation bus.ModuleInvocation, request []byte) ([]byte, bus.ModuleStatus) {
		header, err := db2contract.DecodeRequestHeader(request)
		if err != nil {
			return nil, bus.ModuleStatusInvalidRequest
		}
		op, ok := lookup(invocation.StageID, header.Operation)
		if !ok {
			return nil, bus.ModuleStatusCapabilityAbsent
		}
		if store == nil {
			return nil, bus.ModuleStatusCapabilityAbsent
		}
		// Checked before the work rather than only after it: an invocation
		// already cancelled has no reason to reach the database at all.
		if invocation.Cancelled() {
			return nil, bus.ModuleStatusCancelled
		}
		timeout := invocation.Remaining(defaultOperationTimeout)
		if timeout <= 0 {
			return nil, bus.ModuleStatusCancelled
		}
		ctx, cancel := context.WithTimeout(context.Background(), timeout)
		defer cancel()

		body, status := op(ctx, store, request)
		// A cancellation that arrived while the operation ran outranks its
		// result: the caller is no longer listening, and answering would put a
		// reply on a correlation nobody holds.
		if invocation.Cancelled() {
			return nil, bus.ModuleStatusCancelled
		}
		return body, status
	}
}

// itoa without importing strconv into a file that otherwise needs none.
func itoa(value uint32) string {
	if value == 0 {
		return "0"
	}
	var digits [10]byte
	position := len(digits)
	for value > 0 {
		position--
		digits[position] = byte('0' + value%10)
		value /= 10
	}
	return string(digits[position:])
}
