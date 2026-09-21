package executionpolicy

// Memory publication is an explicit release of persistent canonical fields,
// distinct from permission to search. This narrow policy does not infer tenancy
// membership, approve an organization dump, or grant opaque metadata/evidence.
import (
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"strconv"
	"time"
)

type MemoryReleaseRule struct {
	ID            string   `json:"id"`
	ScopeType     string   `json:"scope_type"`
	ScopeValue    string   `json:"scope_value"`
	FollowUpdates bool     `json:"follow_updates"`
	Revision      string   `json:"revision,omitempty"`
	Cues          []string `json:"cues,omitempty"`
}
type MemoryReleasePolicy struct {
	Version    int                 `json:"version"`
	Recipient  string              `json:"recipient"`
	PeerUID    uint32              `json:"peer_uid"`
	Namespace  string              `json:"namespace"`
	BaseView   string              `json:"base_view"`
	ValidUntil string              `json:"valid_until"`
	Active     bool                `json:"active"`
	Records    []MemoryReleaseRule `json:"records"`
}

func (p MemoryReleasePolicy) Validate(recipient string, uid uint32, now time.Time) error {
	until, e := time.Parse(time.RFC3339Nano, p.ValidUntil)
	if e != nil || !now.Before(until) || !p.Active || p.Version != 1 || recipient == "" || p.Recipient != recipient || p.PeerUID != uid || p.Namespace == "" || len(p.BaseView) != 64 || len(p.Records) > 4096 {
		return fmt.Errorf("memory publication denied")
	}
	if _, e = hex.DecodeString(p.BaseView); e != nil {
		return fmt.Errorf("invalid base release")
	}
	seen := map[string]bool{}
	for _, r := range p.Records {
		id, e := strconv.ParseInt(r.ID, 10, 64)
		if e != nil || id <= 0 || strconv.FormatInt(id, 10) != r.ID || (r.FollowUpdates && r.Revision != "") || seen[r.ID] || r.ScopeType == "" || r.ScopeValue == "" || (!r.FollowUpdates && r.Revision == "") {
			return fmt.Errorf("invalid memory release rule")
		}
		seen[r.ID] = true
	}
	return nil
}
func (p MemoryReleasePolicy) Allows(namespace, id, revision, scopeType, scopeValue string) bool {
	if namespace != p.Namespace {
		return false
	}
	for _, r := range p.Records {
		if r.ID == id && r.ScopeType == scopeType && r.ScopeValue == scopeValue && (r.FollowUpdates || r.Revision == revision) {
			return true
		}
	}
	return false
}
func (p MemoryReleasePolicy) Digest() string {
	b, _ := json.Marshal(p)
	h := sha256.Sum256(b)
	return hex.EncodeToString(h[:])
}
