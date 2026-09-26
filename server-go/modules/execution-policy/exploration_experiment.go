package executionpolicy

import (
	"bytes"
	"encoding/json"
	"errors"
	"log"
	"reflect"
	"time"
)

// Experiments collect calibration; they are not evidence of calibration. A
// separate explicit deployment opt-in binds the frozen manifest, exact live
// scope and a finite list of pre-created sessions. No serving request may opt
// itself in, broaden the list or convert an experiment into release approval.
const explorationExperimentPath = "/etc/aimee/exploration-experiment.json"

type explorationExperiment struct {
	Version        int                         `json:"schema_version"`
	Kind           string                      `json:"kind"`
	AuthorizedBy   string                      `json:"authorized_by"`
	Created        time.Time                   `json:"created"`
	Expires        time.Time                   `json:"expires"`
	ManifestSHA256 string                      `json:"manifest_sha256"`
	Principal      string                      `json:"principal"`
	Sessions       []string                    `json:"sessions"`
	Scope          explorationCalibrationScope `json:"scope"`
	Limits         explorationLimits           `json:"limits"`
}

func parseExplorationExperiment(raw []byte, c explorationContract, now time.Time, manifest string) (string, error) {
	var a explorationExperiment
	d := json.NewDecoder(bytes.NewReader(raw))
	d.DisallowUnknownFields()
	if !sha256Text(manifest) || len(raw) == 0 || len(raw) > 65536 || decodeSingle(d, &a) != nil ||
		a.Version != 1 || a.Kind != "experiment" || a.AuthorizedBy == "" || len(a.AuthorizedBy) > 128 ||
		a.ManifestSHA256 != manifest || a.Created.IsZero() || a.Created.After(now) ||
		!a.Expires.After(now) || !a.Expires.After(a.Created) || a.Expires.Sub(a.Created) > 6*time.Hour ||
		a.Principal != c.Binding.Principal || len(a.Sessions) == 0 || len(a.Sessions) > 512 ||
		!explorationApprovalScope(c, now) || a.Scope != calibrationScope(c) || !reflect.DeepEqual(a.Limits, c.Limits) {
		return "", errors.New("experiment does not cover this live contract")
	}
	seen, included := make(map[string]bool, len(a.Sessions)), false
	for _, session := range a.Sessions {
		if session == "" || len(session) > 128 || seen[session] {
			return "", errors.New("invalid experiment session list")
		}
		seen[session] = true
		included = included || session == c.Binding.Session
	}
	if !included {
		return "", errors.New("session is outside the experiment")
	}
	// The tagged commitment cannot be mistaken for a passing report's digest.
	return "experiment:" + manifest, nil
}

func approvedExplorationExperiment(c explorationContract, now time.Time, manifest string) string {
	raw, err := readExplorationApproval(explorationExperimentPath)
	if err == nil {
		var receipt string
		receipt, err = parseExplorationExperiment(raw, c, now, manifest)
		if err == nil {
			return receipt
		}
	}
	log.Print("[exploration] experiment authorization unavailable for this contract; adaptive mode observe")
	return ""
}
