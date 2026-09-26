package memory

import (
	"fmt"
	"sort"
	"strings"
)

// Formatting stays with the metric owner. Unknown labels and incomplete
// populations are prominent in text as well as in the JSON projection.
func healthReportText(r healthWindow) string {
	var b strings.Builder
	p := r.Metrics.Population
	fmt.Fprintf(&b, "Retrieval health: %s to %s\nProject: %q; workspace: %q; purpose: %s; query class: %s\n", p.From.Format("2006-01-02T15:04:05Z07:00"), p.Until.Format("2006-01-02T15:04:05Z07:00"), p.Project, p.Workspace, p.Purpose, p.QueryClass)
	fmt.Fprintf(&b, "Window complete: %t; capacity evictions: %d; retention expirations: %d\n", r.Complete, r.Evicted, r.Expired)
	for _, stage := range []string{"dispatched", "assembled_unsent", "unknown_dispatch", "network_uncertain"} {
		fmt.Fprintf(&b, "%s: %d retained attempts, %d record occurrences\n", stage, r.Attempts[stage], r.Records[stage])
	}
	fmt.Fprintf(&b, "Selected stage: %s; retained sample: %d invocations; %d unsampled invocations\n", p.Stage, r.Sampled, r.NotSampled)
	value := func(p *float64) string {
		if p == nil {
			return "unknown"
		}
		return fmt.Sprintf("%.4f", *p)
	}
	fmt.Fprintf(&b, "Record concentration (retained sample): top-1 %s; top-5 %s; HHI %s; Simpson %s; entropy %s; normalized entropy %s\n", value(r.Metrics.Records.Top1Share), value(r.Metrics.Records.Top5Share), value(r.Metrics.Records.HHI), value(r.Metrics.Records.Simpson), value(r.Metrics.Records.Entropy), value(r.Metrics.Records.Normalized))
	fmt.Fprintf(&b, "Estimated occurrences: %.2f; sampling-design standard error: %.2f\n", r.Metrics.EstimatedOccurrences, r.Metrics.EstimateSE)
	fmt.Fprintf(&b, "Repeat-serving: %s (%d comparable, %d unknown); lifecycle re-entry: %s (%d labeled, %d unknown)\n", value(r.Metrics.Repeat.Rate), r.Metrics.Repeat.Denominator, r.Metrics.Repeat.Unknown, value(r.Metrics.Lifecycle.Rate), r.Metrics.Lifecycle.Denominator, r.Metrics.Lifecycle.Unknown)
	keys := make([]string, 0, len(r.MetadataGaps))
	for key := range r.MetadataGaps {
		keys = append(keys, key)
	}
	sort.Strings(keys)
	for _, key := range keys {
		fmt.Fprintf(&b, "Missing %s: %d invocations\n", key, r.MetadataGaps[key])
	}
	fmt.Fprintf(&b, "Unmeasured: %s\n", strings.Join(r.Metrics.Unsupported, ", "))
	return b.String()
}
