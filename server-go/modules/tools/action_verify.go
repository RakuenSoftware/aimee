package tools

import (
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	executionpolicy "github.com/JBailes/aimee/server-go/modules/execution-policy"
	"io"
	"os"
	"strings"
	"syscall"
	"time"
)

type ActionVerification struct {
	State         string    `json:"state"`
	Destination   string    `json:"destination"`
	PayloadDigest string    `json:"payload_sha256"`
	ObjectVersion string    `json:"object_version,omitempty"`
	ObservedAt    time.Time `json:"observed_at"`
}

// VerifyGovernedAction proves a bounded file write's postcondition at the exact
// approved object. It never verifies a different path with equal bytes. The
// observation says nothing about whether another actor changes the file later.
func VerifyGovernedAction(tool string, raw []byte, cwd, destination, payload string) (ActionVerification, error) {
	result := ActionVerification{State: "outcome_unknown", Destination: destination, PayloadDigest: payload, ObservedAt: time.Now().UTC()}
	resource, err := DescribeGovernedAction(tool, raw, cwd)
	if err != nil || resource.Destination != destination || resource.PayloadDigest != payload {
		return result, errors.New("action verification binding changed")
	}
	if tool != "write_file" {
		return result, errors.New("exact postcondition adapter unavailable")
	}
	var args map[string]json.RawMessage
	if json.Unmarshal(resource.Arguments, &args) != nil {
		return result, errors.New("invalid effective arguments")
	}
	expected := ""
	// Native write_file treats absent/non-string content as empty. Preserve that
	// contract; verification compares the actual bytes, including the empty case.
	_ = json.Unmarshal(args["content"], &expected)
	path := strings.TrimPrefix(destination, "file:")
	if err := executionpolicy.AuthorizeActionObservation(path); err != nil {
		return result, err
	}
	before, err := os.Lstat(path)
	if err != nil || !before.Mode().IsRegular() {
		return result, errors.New("intended regular file unavailable")
	}
	file, err := os.OpenFile(path, os.O_RDONLY|syscall.O_NOFOLLOW|syscall.O_NONBLOCK, 0)
	if err != nil {
		return result, errors.New("intended file unreadable")
	}
	defer file.Close()
	opened, err := file.Stat()
	if err != nil || !os.SameFile(before, opened) {
		return result, errors.New("intended object changed")
	}
	actual, err := io.ReadAll(io.LimitReader(file, int64(len(expected))+1))
	if err != nil || string(actual) != expected {
		return result, errors.New("approved file postcondition not observed")
	}
	after, err := os.Lstat(path)
	if err != nil || !os.SameFile(opened, after) || opened.Size() != after.Size() || !opened.ModTime().Equal(after.ModTime()) {
		return result, errors.New("intended object changed during verification")
	}
	version, _ := json.Marshal([]any{destination, payload, after.Size(), after.ModTime().UTC(), sha256.Sum256(actual)})
	digest := sha256.Sum256(version)
	result.ObjectVersion = hex.EncodeToString(digest[:])
	result.State = "effect_confirmed"
	return result, nil
}
