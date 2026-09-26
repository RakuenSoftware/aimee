package memory

import (
	"context"
	"encoding/json"
	"os"
	"sort"
	"strconv"
	"strings"
)

const typedSelectionPolicyVersion = "typed-diversity-rankwindow3-v2"
const typedSelectionCandidateLimit = 128

type typedSelectionCandidate struct {
	channel string
	item    typedItem
	rank    int
}
type typedSelectionDecision struct {
	Channel           string `json:"channel"`
	ID                string `json:"stable_id"`
	BasePosition      int    `json:"base_position"`
	SelectionPosition int    `json:"selection_position"`
	Priority          int    `json:"priority"`
	FamilyState       string `json:"family_state"`
}
type typedSelectionReport struct {
	Decisions         []typedSelectionDecision `json:"decisions"`
	ArtifactDigest    string                   `json:"artifact_digest"`
	Version           string                   `json:"version"`
	Exposure          string                   `json:"exposure"`
	Tolerance         int                      `json:"discretionary_rank_window"`
	MaxItems          int                      `json:"max_items"`
	UnsatisfiedFloors []string                 `json:"unsatisfied_type_floors"`
	Truncated         bool                     `json:"candidate_work_limit_reached"`
}

// Only the owner process configuration selects this immutable experiment.
// Request text, serving counts and confidence cannot enable or tune it.
func (r *typedContextResult) configureSelection() {
	if os.Getenv("AIMEE_MEMORY_SELECTION_POLICY") != typedSelectionPolicyVersion {
		return
	}
	r.SelectionPolicy = &typedSelectionReport{ArtifactDigest: rankingArtifactDigest(true), Version: typedSelectionPolicyVersion, Exposure: "disabled", Tolerance: 3, MaxItems: 64, UnsatisfiedFloors: []string{}}
	r.selectionCollect = true
}
func (r *typedContextResult) collectSelection(channel string, item typedItem) {
	if len(r.selectionPending) >= typedSelectionCandidateLimit {
		r.SelectionPolicy.Truncated = true
		r.trace(channel, item.id, typedEstimate(item.text), false, "selection candidate work limit")
		if r.Requirements != nil {
			r.coverageCandidates = append(r.coverageCandidates, item)
		}
		return
	}
	r.selectionPending = append(r.selectionPending, typedSelectionCandidate{channel: channel, item: item, rank: len(r.selectionPending)})
}
func selectionText(item typedItem) string { return strings.Join(strings.Fields(item.text), " ") }
func (r *typedContextResult) applySelection() {
	if !r.selectionCollect {
		return
	}
	r.selectionCollect = false
	candidates := r.selectionPending
	r.selectionPending = nil
	items := make([]typedItem, 0, len(candidates))
	for _, c := range candidates {
		if r.Channels[c.channel].Enabled {
			items = append(items, c.item)
		}
	}
	required := map[string]bool{}
	if r.Requirements != nil {
		roles, supported := r.Requirements.expandedRoles()
		if supported {
			for _, role := range roles {
				if role.Optional {
					continue
				}
				ids, _ := evidenceRoleMatches(role, items)
				if len(ids) > 0 {
					required[ids[0]] = true
				}
				// Canonical families are not independence certificates. Preserve
				// the strongest potential support plus bounded representatives
				// of different complete origins so copies cannot crowd out the
				// only potentially independent source. Coverage stays unknown.
				if role.Role == "independent_support" {
					families := map[string]bool{}
					representatives, first := 0, false
					for _, item := range items {
						h, ok := coverageAssertion(item)
						if !ok || h.Subject != role.Subject || h.Relation != role.Relation {
							continue
						}
						if !first {
							required[item.id] = true
							first = true
						}
						novel := false
						for _, family := range item.families {
							if !families[family] {
								novel = true
							}
						}
						if novel && representatives < min(16, max(2, role.MinIndependent)) {
							required[item.id] = true
							representatives++
							for _, family := range item.families {
								families[family] = true
							}
						}
					}
				}
			}
		}
	}
	mandatory := map[string]bool{}
	floor := map[string]bool{}
	for i := range candidates {
		c := &candidates[i]
		c.item.selectionPriority = 3
		if !r.Channels[c.channel].Enabled {
			continue
		}
		if h, ok := coverageAssertion(c.item); ok && (h.Kind == "instruction" || h.Kind == "policy" || (h.Authority > 0 && h.PriorVersionID != "")) {
			// Identical copies do not become thirty mandatory reservations. Distinct
			// constraints and conflicting corrections retain their own reservation.
			key := h.Subject + "\x00" + h.Relation + "\x00" + h.Object + "\x00" + h.Kind
			if !mandatory[key] {
				c.item.selectionPriority = 0
				mandatory[key] = true
			}
		}
		if required[c.item.id] && c.item.selectionPriority > 1 {
			c.item.selectionPriority = 1
		}
		if !floor[c.channel] {
			if c.item.selectionPriority > 2 {
				c.item.selectionPriority = 2
			}
			floor[c.channel] = true
		}
	}
	sort.SliceStable(candidates, func(i, j int) bool {
		return candidates[i].item.selectionPriority < candidates[j].item.selectionPriority
	})
	seenText := map[string]bool{}
	seenFamily := map[string]bool{}
	selected := 0
	for i := 0; i < len(candidates); i++ {
		// Diversity only reorders a bounded, same-channel rank neighborhood of
		// discretionary evidence. Mandatory content and task roles precede it.
		best := i
		penalty := func(c typedSelectionCandidate) int {
			n := 0
			if seenText[c.channel+"\x00"+selectionText(c.item)] {
				n += 2
			}
			for _, f := range c.item.families {
				if seenFamily[f] {
					n++
				}
			}
			return n
		}
		if candidates[i].item.selectionPriority == 3 {
			for j := i + 1; j < len(candidates); j++ {
				c := candidates[j]
				if c.channel == candidates[i].channel && c.item.selectionPriority == 3 && c.rank <= candidates[i].rank+3 && penalty(c) < penalty(candidates[best]) {
					best = j
				}
			}
		}
		c := candidates[best]
		familyState := "unknown"
		if len(c.item.families) > 0 {
			familyState = "canonical_complete_not_independence"
		}
		r.SelectionPolicy.Decisions = append(r.SelectionPolicy.Decisions, typedSelectionDecision{Channel: c.channel, ID: c.item.id, BasePosition: c.rank + 1, SelectionPosition: i + 1, Priority: c.item.selectionPriority, FamilyState: familyState})
		copy(candidates[i+1:best+1], candidates[i:best])
		candidates[i] = c
		if selected >= r.SelectionPolicy.MaxItems {
			r.trace(c.channel, c.item.id, typedEstimate(c.item.text), false, "selection item limit")
			if r.Requirements != nil {
				r.coverageCandidates = append(r.coverageCandidates, c.item)
			}
			continue
		}
		before := len(r.Channels[c.channel].selected)
		r.add(c.channel, c.item)
		if len(r.Channels[c.channel].selected) > before {
			selected++
			seenText[c.channel+"\x00"+selectionText(c.item)] = true
			for _, f := range c.item.families {
				seenFamily[f] = true
			}
		}
	}
}
func (r *typedContextResult) selectionDropPriority() int {
	priority := -1
	if r.SelectionPolicy == nil {
		return priority
	}
	for _, c := range r.Channels {
		if n := len(c.selected); n > 0 && c.selected[n-1].selectionPriority > priority {
			priority = c.selected[n-1].selectionPriority
		}
	}
	return priority
}
func (r *typedContextResult) finishSelectionReport() {
	if r.SelectionPolicy == nil {
		return
	}
	r.SelectionPolicy.UnsatisfiedFloors = []string{}
	for _, name := range typedChannelOrder {
		c := r.Channels[name]
		if c.Enabled && len(c.Items) == 0 {
			r.SelectionPolicy.UnsatisfiedFloors = append(r.SelectionPolicy.UnsatisfiedFloors, name)
		}
	}
}

// Family identity comes only from complete canonical ancestry at the exact
// source revision. Multiple families are not independence certificates.
func (s *postgresDataStore) selectionAssertionFamilies(ctx context.Context, h assertionHit) []string {
	if !h.memoryParentsObserved || len(h.memoryParents) == 0 || len(h.memoryParents) > 8 {
		return nil
	}
	families := map[string]bool{}
	for _, p := range h.memoryParents {
		id, err := strconv.ParseInt(p.RecordID, 10, 64)
		if err != nil {
			return nil
		}
		evidence, err := s.memoryEvidence(ctx, id)
		if err != nil {
			return nil
		}
		// Match the successful public owner envelope before applying the shared
		// exact-revision family validator; memoryEvidence returns its payload only.
		evidence["status"] = "ok"
		raw, err := json.Marshal(evidence)
		if err != nil {
			return nil
		}
		found := healthFamiliesFromEvidence(raw, &typedSourceVersion{Kind: "memory_record", Version: p})
		if len(found) == 0 {
			return nil
		}
		for _, f := range found {
			families[f] = true
		}
	}
	result := make([]string, 0, len(families))
	for f := range families {
		result = append(result, f)
	}
	sort.Strings(result)
	return result
}
