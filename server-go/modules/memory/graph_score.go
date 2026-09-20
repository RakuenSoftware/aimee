package memory

import (
	"math"
	"strings"
	"time"
)

var graphGravity = map[string]float64{
	"defines": 1, "contains": .85, "depends_on": .75, "routes": .70,
	"exports": .60, "co_edited": .60, "calls": .55, "co_discussed": .45, "imports": .30,
}

func graphConfidence(class string) float64 {
	switch strings.ToUpper(class) {
	case "", "A":
		return 1
	case "B":
		return .75
	default:
		return .5
	}
}
func graphEdgeScore(relation string, code bool, structural, observed int, utility float64, hop int, class string) float64 {
	gravity, ok := graphGravity[relation]
	if !ok {
		gravity = .45
		if class != "" {
			gravity = .8
		}
	}
	structure := 1.0
	if code {
		structure += float64(max(0, min(3, structural))) / 3
	}
	observations := 1 + math.Min(3, math.Log1p(float64(max(0, observed))))/3
	if math.IsNaN(utility) || math.IsInf(utility, 0) {
		utility = 0
	}
	return gravity * graphConfidence(class) * structure * observations * (1 + math.Max(-.5, math.Min(2, utility))) * math.Pow(.5, float64(max(0, hop-1)))
}
func graphUtility(raw float64, touched string, now time.Time) float64 {
	if touched == "" {
		return raw
	}
	var at time.Time
	for _, layout := range []string{time.RFC3339Nano, "2006-01-02 15:04:05.999999999Z07:00", "2006-01-02 15:04:05.999999999Z07", "2006-01-02 15:04:05"} {
		if parsed, err := time.Parse(layout, touched); err == nil {
			at = parsed
			break
		}
	}
	if at.IsZero() {
		return raw
	}
	if at.Year() <= 1970 {
		return 0
	}
	return math.Max(-5, math.Min(5, raw*math.Pow(.5, math.Max(0, now.Sub(at).Hours())/(24*90))))
}
func graphCodeNode(node string) bool {
	for _, prefix := range []string{"file:", "symbol:", "import:", "export:", "route:", "project:"} {
		if strings.HasPrefix(node, prefix) {
			return true
		}
	}
	return false
}
func graphCodeQuery(query string) bool {
	for _, token := range strings.Fields(textBound(query, 4096)) {
		token = strings.Trim(token, "\"'`()[],;?!")
		if graphCodeNode(token) || (strings.Contains(token, "/") && strings.Contains(token, ".")) || strings.Contains(token, "::") || strings.Contains(token, "->") {
			return true
		}
		if i := strings.IndexByte(token, ':'); i >= 0 && i+1 < len(token) && token[i+1] >= '0' && token[i+1] <= '9' {
			return true
		}
		for _, extension := range []string{".c", ".h", ".cc", ".cpp", ".hpp", ".inc", ".py", ".js", ".ts", ".go", ".rs", ".java", ".rb", ".sh", ".sql"} {
			if strings.HasSuffix(token, extension) {
				return true
			}
		}
	}
	return false
}
