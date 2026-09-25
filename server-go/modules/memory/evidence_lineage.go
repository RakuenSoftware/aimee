package memory

import "sort"

// These inputs are assembled by the storage owner from scoped, versioned
// evidence. They are not a public declaration or model-authored trust label.
// Family identifies a recorded upstream origin, not independent corroboration.
// A certificate partitions witnesses only within its explicitly verified set;
// different sets are not assumed independent of one another.
type lineageOrigin struct {
	Family             string
	IndependenceSet    string
	Witness            string
	CommonDependencies []string
}

type lineageEdge struct {
	Parent   string
	Revision string
	Claim    string
	Relation string
}

type lineageNode struct {
	Revision string
	Complete bool
	Origin   *lineageOrigin
	Edges    []lineageEdge
}

type lineageSupport struct {
	SupportCount            int      `json:"support_count"`
	SourceFamilyCount       int      `json:"source_family_count"`
	IndependentSupportCount *int     `json:"independent_support_count"`
	IndependenceState       string   `json:"independence_state"`
	UnknownOriginCount      int      `json:"unknown_origin_count"`
	LineageState            string   `json:"lineage_state"`
	Reasons                 []string `json:"reasons"`
}

type lineageWalkResult struct {
	roots    map[string]lineageOrigin
	complete bool
}

// projectLineage counts only evidence-bearing, claim-specific edges. The caller
// must withhold records whose required audience intersection is not authorized;
// no hidden node identifiers or counts enter this projection. Missing nodes are
// unknown, never fresh independent witnesses. Bounds apply to actual traversal.
func projectLineage(claim string, supports []string, nodes map[string]lineageNode, maxNodes, maxDepth int) lineageSupport {
	out := lineageSupport{IndependenceState: "unknown", LineageState: "complete", Reasons: []string{}}
	reasons := map[string]bool{}
	if maxNodes > 4096 {
		maxNodes = 4096
	}
	if maxDepth > 64 {
		maxDepth = 64
	}
	edgeWork := 0
	merge := func(dst map[string]lineageOrigin, family string, origin lineageOrigin) {
		if previous, exists := dst[family]; exists {
			if previous.IndependenceSet != origin.IndependenceSet || previous.Witness != origin.Witness {
				reasons["conflicting_origin_certificate"] = true
				origin.IndependenceSet, origin.Witness = "", ""
			}
			deps := map[string]bool{}
			for _, dep := range previous.CommonDependencies {
				deps[dep] = true
			}
			for _, dep := range origin.CommonDependencies {
				deps[dep] = true
			}
			origin.CommonDependencies = nil
			for dep := range deps {
				origin.CommonDependencies = append(origin.CommonDependencies, dep)
			}
			sort.Strings(origin.CommonDependencies)
		}
		dst[family] = origin
	}
	roots := map[string]lineageOrigin{}
	visited := map[string]bool{}
	memo := map[string]lineageWalkResult{}
	active := map[string]bool{}
	var walk func(string, string, int) lineageWalkResult
	unknown := func(reason string) lineageWalkResult {
		reasons[reason] = true
		return lineageWalkResult{roots: map[string]lineageOrigin{}}
	}
	walk = func(id, revision string, depth int) lineageWalkResult {
		if depth > maxDepth || maxNodes < 1 {
			return unknown("truncated")
		}
		node, ok := nodes[id]
		if !ok || node.Revision == "" {
			return unknown("missing")
		}
		if revision != "" && revision != node.Revision {
			return unknown("stale_version")
		}
		if active[id] {
			return unknown("cycle")
		}
		if result, ok := memo[id]; ok {
			return result
		}
		if !visited[id] && len(visited) >= maxNodes {
			return unknown("truncated")
		}
		if len(node.Edges) > maxNodes*16-edgeWork {
			return unknown("truncated")
		}
		edgeWork += len(node.Edges)
		visited[id] = true
		active[id] = true
		defer delete(active, id)
		result := lineageWalkResult{roots: map[string]lineageOrigin{}, complete: node.Complete}
		if !node.Complete {
			reasons["undeclared_inputs"] = true
		}
		edges := append([]lineageEdge(nil), node.Edges...)
		sort.Slice(edges, func(i, j int) bool {
			a, b := edges[i], edges[j]
			if a.Parent != b.Parent {
				return a.Parent < b.Parent
			}
			if a.Revision != b.Revision {
				return a.Revision < b.Revision
			}
			if a.Claim != b.Claim {
				return a.Claim < b.Claim
			}
			return a.Relation < b.Relation
		})
		parents := 0
		for _, edge := range edges {
			if edge.Claim != claim {
				continue
			}
			switch edge.Relation {
			case "derived_from", "quotes", "republication_of":
			default:
				continue
			}
			parents++
			if edge.Revision == "" {
				result.complete = false
				reasons["unversioned_input"] = true
				continue
			}
			parent := walk(edge.Parent, edge.Revision, depth+1)
			result.complete = result.complete && parent.complete
			for family, origin := range parent.roots {
				merge(result.roots, family, origin)
			}
		}
		if parents == 0 {
			if node.Origin == nil || node.Origin.Family == "" {
				result.complete = false
				reasons["unknown_origin"] = true
			} else {
				origin := *node.Origin
				if !node.Complete {
					origin.IndependenceSet, origin.Witness = "", ""
				}
				result.roots[node.Origin.Family] = origin
			}
		} else if node.Origin != nil {
			result.complete = false
			reasons["derived_origin_conflict"] = true
		}
		// Do not memoize incomplete walks: a depth-limited visit must not hide a
		// later shallower path, and a cycle must never become a completed root set.
		if result.complete {
			memo[id] = result
		}
		return result
	}
	ids := append([]string(nil), supports...)
	sort.Strings(ids)
	last := ""
	for _, id := range ids {
		if id == last {
			continue
		}
		last = id
		out.SupportCount++
		result := walk(id, "", 0)
		if !result.complete {
			out.UnknownOriginCount++
			out.LineageState = "partial"
		}
		for family, origin := range result.roots {
			merge(roots, family, origin)
		}
	}
	out.SourceFamilyCount = len(roots)
	// Merge explicitly recorded common dependencies before counting witnesses.
	// Composite descendants do not merge A and B: they merely carry both roots.
	families := make([]string, 0, len(roots))
	for family := range roots {
		families = append(families, family)
	}
	sort.Strings(families)
	parent := map[string]string{}
	var find func(string) string
	find = func(x string) string {
		if parent[x] != x {
			parent[x] = find(parent[x])
		}
		return parent[x]
	}
	for _, family := range families {
		parent[family] = family
	}
	common := map[string]string{}
	for _, family := range families {
		for _, dep := range roots[family].CommonDependencies {
			if dep == "" {
				continue
			}
			if prior, ok := common[dep]; ok {
				parent[find(family)] = find(prior)
			} else {
				common[dep] = family
			}
		}
	}
	sets := map[string]bool{}
	certified := 0
	for _, family := range families {
		origin := roots[family]
		if origin.IndependenceSet == "" || origin.Witness == "" {
			continue
		}
		certified++
		sets[origin.IndependenceSet] = true
	}

	best := 0
	for set := range sets {
		// Bipartite connected components of witness IDs and dependency components.
		groups := map[string]string{}
		var group func(string) string
		group = func(x string) string {
			p, ok := groups[x]
			if !ok {
				groups[x] = x
				return x
			}
			if p != x {
				groups[x] = group(p)
			}
			return groups[x]
		}
		for _, family := range families {
			origin := roots[family]
			if origin.IndependenceSet != set || origin.Witness == "" {
				continue
			}
			a, b := "w:"+origin.Witness, "d:"+find(family)
			groups[group(a)] = group(b)
		}
		counts := map[string]bool{}
		for key := range groups {
			counts[group(key)] = true
		}
		if len(counts) > best {
			best = len(counts)
		}
	}
	if certified > 0 {
		out.IndependentSupportCount = &best
		out.IndependenceState = "partial"
		if certified == len(roots) && out.UnknownOriginCount == 0 && len(sets) == 1 {
			out.IndependenceState = "complete"
		}
	}
	for reason := range reasons {
		out.Reasons = append(out.Reasons, reason)
	}
	sort.Strings(out.Reasons)
	return out
}
