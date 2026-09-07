package providers

import (
	"encoding/json"
	"io"
	"os"
	"strings"
)

// RunBootstrapLookup serves the pre-bus credential bootstrap. It uses the same
// Go parser as the running owner, and returns only a canonical model name.
func RunBootstrapLookup(args []string) (bool, int) {
	if len(args) != 2 || args[1] != "__aimee_provider_bootstrap_lookup" {
		return false, 0
	}
	input, err := io.ReadAll(io.LimitReader(os.Stdin, 1024))
	var req object
	if err != nil || json.Unmarshal(input, &req) != nil {
		return true, 1
	}
	store, err := NewStore(str(req, "home"))
	if err != nil {
		return true, 1
	}
	name := ""
	_, err = store.transaction(false, func(root object) (object, error) {
		for _, model := range rows(root, "models") {
			if strings.EqualFold(str(model, "name"), str(req, "name")) {
				name = str(model, "name")
				break
			}
		}
		return nil, nil
	})
	if err != nil || name == "" {
		return true, 1
	}
	if json.NewEncoder(os.Stdout).Encode(object{"name": name}) != nil {
		return true, 1
	}
	return true, 0
}
