package families

import (
	"context"

	store "github.com/JBailes/aimee/server-go/modules/aimee"
)

// Operation IDs 33–35 remain reserved for wire compatibility. Personal memory
// is owned by the selected private memory module; the public conversation
// family must not recall, revive, or create a second copy in user_memories.
// RunDB avoids beginning even the former upsert transaction. Historical schema,
// migrations and erasure remain available for records written by older hosts.
func retiredPersonalMemory(context.Context, store.DB, []string) (uint32, []string, error) {
	return store.StatusInvalid, nil, nil
}
