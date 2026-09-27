package executionpolicy

import (
	"bytes"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"io"
	"log"
	"os"
	"reflect"
	"strings"
	"syscall"
	"time"
)

// Calibration is operator deployment state, never a repository policy file or
// a model argument. Both opt-in and the reviewed artifact are re-read for each
// admission. Removing either rolls back adaptive enforcement without deleting
// accounting or contract history. No artifact is shipped with the application.
const explorationCalibrationPath = "/etc/aimee/exploration-calibration.json"

type explorationCalibration struct {
	Version      int                         `json:"schema_version"`
	ReviewedBy   string                      `json:"reviewed_by"`
	Created      time.Time                   `json:"created"`
	Expires      time.Time                   `json:"expires"`
	Scope        explorationCalibrationScope `json:"scope"`
	Limits       explorationLimits           `json:"limits"`
	ReportSHA256 string                      `json:"report_sha256"`
	Report       json.RawMessage             `json:"report"`
}

type explorationCalibrationScope struct {
	ProducerBuild      string `json:"producer_build"`
	Project            string `json:"project"`
	Workspace          string `json:"workspace"`
	WorkingDirectory   string `json:"working_directory"`
	WorktreeGeneration string `json:"worktree_generation"`
	IndexGeneration    string `json:"index_generation"`
	QueryClass         string `json:"query_class"`
	Route              string `json:"route"`
	Provider           string `json:"provider"`
	Model              string `json:"model"`
	LimitsDigest       string `json:"limits_digest"`
}

func calibrationScope(c explorationContract) explorationCalibrationScope {
	b := c.Binding
	return explorationCalibrationScope{b.ProducerBuild, b.Project, b.Workspace, b.WorkingDirectory, b.WorktreeGeneration,
		b.IndexGeneration, c.QueryClass, b.Route, b.Provider, b.Model, b.LimitsDigest}
}

func sha256Text(s string) bool {
	b, err := hex.DecodeString(s)
	return err == nil && len(b) == sha256.Size && strings.ToLower(s) == s
}

func parseExplorationCalibration(raw []byte, c explorationContract, now time.Time) (string, error) {
	var a explorationCalibration
	dec := json.NewDecoder(bytes.NewReader(raw))
	dec.DisallowUnknownFields()
	if len(raw) == 0 || len(raw) > 65536 || decodeSingle(dec, &a) != nil || a.Version != 1 ||
		a.ReviewedBy == "" || len(a.ReviewedBy) > 128 || a.Created.IsZero() || a.Created.After(now) ||
		!a.Expires.After(now) || !a.Expires.After(a.Created) || a.Expires.Sub(a.Created) > 30*24*time.Hour {
		return "", errors.New("invalid or expired reviewed calibration")
	}
	if !explorationApprovalScope(c, now) || a.Scope != calibrationScope(c) || !reflect.DeepEqual(a.Limits, c.Limits) {
		return "", errors.New("calibration does not cover this live contract")
	}
	var compact bytes.Buffer
	if json.Compact(&compact, a.Report) != nil {
		return "", errors.New("invalid calibration report JSON")
	}
	hash := sha256.Sum256(compact.Bytes())
	if !sha256Text(a.ReportSHA256) || hex.EncodeToString(hash[:]) != a.ReportSHA256 || !reviewedExplorationReport(a.Report) {
		return "", errors.New("paired workload gate is not satisfied")
	}
	return a.ReportSHA256, nil
}

// Every approval requires the same live owner, context and exact execution scope.
// An experiment changes only the evidence required to authorize its named sessions.
func explorationApprovalScope(c explorationContract, now time.Time) bool {
	b := c.Binding
	if !c.valid(now) || !c.CoverageComplete || !b.HostWorktree || !b.IndexObservedCurrent || !b.OwnerObservedCurrent ||
		c.QueryClass != "typed_requirements" || !strings.HasPrefix(b.WorktreeGeneration, "git-clean:") ||
		b.ProducerBuild == "" || b.Workspace == "" || b.WorkingDirectory == "" || b.Route == "" || b.Provider == "" || b.Model == "" ||
		!sha256Text(b.LimitsDigest) || !sha256Text(c.ReceiptDigest) ||
		!c.Limits.Enabled || c.Limits.RawScans == nil ||
		c.Limits.Files != nil || c.Limits.Graph != nil || c.Limits.Bytes != nil || c.Limits.Tokens != nil ||
		len(c.SupportedClasses) != 1 || c.SupportedClasses[0] != "raw_scan" {
		return false
	}
	return true
}

// Validate the frozen scorer's result and quantitative gates, rather than
// accepting a free-standing "calibrated" boolean. The operator review attests
// collection/judging provenance; this parser cannot infer it from aggregates.
func reviewedExplorationReport(raw []byte) bool {
	var r struct {
		Version  int                `json:"schema_version"`
		Manifest string             `json:"manifest_sha256"`
		Results  string             `json:"results_sha256"`
		Expected int                `json:"expected_pairs"`
		Measured int                `json:"measured_pairs"`
		Invalid  []json.RawMessage  `json:"invalid_cells"`
		Decision string             `json:"decision"`
		Method   string             `json:"paired_interval_method"`
		Policy   map[string]float64 `json:"policy"`
		Gates    map[string]bool    `json:"gates"`
		Metrics  struct {
			Arms map[string]struct {
				Successes         int64   `json:"task_successes"`
				Raw               int64   `json:"raw_scans"`
				Redundant         int64   `json:"redundant_scans"`
				Restrictions      int64   `json:"restrictions"`
				FalseRestrictions int64   `json:"false_restrictions"`
				Expansions        int64   `json:"expansions"`
				Cost              float64 `json:"total_cost"`
				P95               float64 `json:"p95_latency_seconds"`
			} `json:"arms"`
			Interval []float64 `json:"paired_success_interval"`
		} `json:"metrics"`
	}
	policy := map[string]float64{"task_success_noninferiority_margin": .01, "paired_confidence": .95,
		"maximum_p95_latency_ratio": 1.10, "maximum_total_cost_ratio": 1, "minimum_redundant_scan_reduction": .20}
	if json.Unmarshal(raw, &r) != nil || r.Version != 1 || !sha256Text(r.Manifest) || !sha256Text(r.Results) ||
		r.Expected <= 0 || r.Expected != r.Measured || len(r.Invalid) != 0 || r.Invalid == nil ||
		r.Decision != "eligible_for_operator_review" || r.Method != "bonferroni_clopper_pearson_discordant_rates_95_percent" ||
		!reflect.DeepEqual(r.Policy, policy) || len(r.Gates) != 4 || len(r.Metrics.Arms) != 2 ||
		len(r.Metrics.Interval) != 2 || r.Metrics.Interval[0] < -.01 || r.Metrics.Interval[1] > 1 ||
		r.Metrics.Interval[0] > r.Metrics.Interval[1] {
		return false
	}
	for _, k := range []string{"task_success_noninferior", "p95_latency", "total_cost", "redundant_scans"} {
		if !r.Gates[k] {
			return false
		}
	}
	for _, k := range []string{"baseline", "treatment"} {
		a, ok := r.Metrics.Arms[k]
		if !ok || a.Successes < 0 || a.Successes > int64(r.Measured) || a.Raw < 0 || a.Redundant < 0 || a.Redundant > a.Raw ||
			a.Restrictions < 0 || a.FalseRestrictions < 0 || a.FalseRestrictions > a.Restrictions || a.Expansions < 0 || a.Cost < 0 || a.P95 <= 0 {
			return false
		}
	}
	b, t := r.Metrics.Arms["baseline"], r.Metrics.Arms["treatment"]
	return b.Redundant > 0 && float64(t.Redundant) <= .8*float64(b.Redundant) && t.Cost <= b.Cost && t.P95 <= 1.10*b.P95
}

// Traverse using directory descriptors: no writable ancestor or symlink may
// redirect the fixed operator artifact into a model-writable checkout. These
// modules deploy on Linux; openat also avoids check-then-open replacement races.
func readExplorationCalibration() ([]byte, error) {
	return readExplorationApproval(explorationCalibrationPath)
}

// path is one of the two fixed deployment paths, never a request argument.
func readExplorationApproval(path string) ([]byte, error) {
	fd, err := syscall.Open("/", syscall.O_RDONLY|syscall.O_DIRECTORY|syscall.O_CLOEXEC, 0)
	if err != nil {
		return nil, err
	}
	defer func() { syscall.Close(fd) }()
	parts := strings.Split(strings.TrimPrefix(path, "/"), "/")
	for i, part := range parts {
		flags := syscall.O_RDONLY | syscall.O_CLOEXEC | syscall.O_NOFOLLOW | syscall.O_NONBLOCK
		if i < len(parts)-1 {
			flags |= syscall.O_DIRECTORY
		}
		next, e := syscall.Openat(fd, part, flags, 0)
		if e != nil {
			return nil, e
		}
		syscall.Close(fd)
		fd = next
		var stat syscall.Stat_t
		if syscall.Fstat(fd, &stat) != nil || stat.Uid != 0 || stat.Mode&0022 != 0 {
			return nil, errors.New("calibration path must be root-owned and protected")
		}
		if i == len(parts)-1 && (stat.Mode&syscall.S_IFMT != syscall.S_IFREG || stat.Size <= 0 || stat.Size > 65536) {
			return nil, errors.New("invalid calibration artifact file")
		}
	}
	file := os.NewFile(uintptr(fd), path)
	fd = -1 // file owns the descriptor after successful traversal
	defer file.Close()
	return io.ReadAll(io.LimitReader(file, 65537))
}

func approvedExplorationCalibration(c explorationContract, now time.Time) string {
	if manifest := os.Getenv("AIMEE_EXPLORATION_EXPERIMENT"); manifest != "" {
		return approvedExplorationExperiment(c, now, manifest)
	}
	if os.Getenv("AIMEE_EXPLORATION_ENFORCE") != "1" {
		return ""
	}
	raw, err := readExplorationCalibration()
	if err != nil {
		log.Print("[exploration] reviewed calibration unavailable; adaptive mode observe")
		return ""
	}
	receipt, err := parseExplorationCalibration(raw, c, now)
	if err != nil {
		log.Print("[exploration] reviewed calibration does not cover the live contract; adaptive mode observe")
		return ""
	}
	return receipt
}
