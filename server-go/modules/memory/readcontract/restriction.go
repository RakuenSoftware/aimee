package readcontract

import (
	"fmt"
	"regexp"
)

var restrictionDigest = regexp.MustCompile(`^[a-f0-9]{64}$`)

// SearchRestriction only narrows existing read authority. It is not a bearer
// credential. Upstream authentication and exact egress authorization remain
// mandatory. IDs are local to the configured upstream namespace.
type SearchRestriction struct {
	Version       int     `json:"version"`
	Namespace     string  `json:"namespace"`
	Authorization string  `json:"authorization"`
	IDs           []int64 `json:"ids"`
}

func (r *SearchRestriction) Validate() error {
	if r == nil || r.Version != 1 || r.Namespace == "" || len(r.Namespace) > 256 || !restrictionDigest.MatchString(r.Authorization) || r.IDs == nil || len(r.IDs) > 4096 {
		return fmt.Errorf("invalid search restriction")
	}
	seen := map[int64]bool{}
	for _, id := range r.IDs {
		if id <= 0 || seen[id] {
			return fmt.Errorf("invalid restricted ID")
		}
		seen[id] = true
	}
	return nil
}
