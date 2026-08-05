package wfe

import (
	"bytes"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"regexp"
	"syscall"

	"github.com/JBailes/aimee/server-go/modules/roundtable/panel"
)

var (
	ErrInvalidWorkItem    = errors.New("invalid work-item id")
	ErrInvalidNode        = errors.New("invalid workflow node id")
	ErrImmutable          = errors.New("immutable artifact already has different content")
	workItemIDPattern     = regexp.MustCompile(`^wi_[A-Za-z0-9_.-]+$`)
	artifactNodeIDPattern = regexp.MustCompile(`^[A-Za-z][A-Za-z0-9_-]*$`)
)

// ArtifactStore owns the durable, lossless documents used by the workflow
// engine. Content is never shortened to fit an implementation buffer. An I/O or
// allocation failure is returned to the caller and must park the workflow.
type ArtifactStore struct {
	root string
}

func NewArtifactStore(root string) (*ArtifactStore, error) {
	if root == "" {
		return nil, errors.New("artifact root is required")
	}
	abs, err := filepath.Abs(root)
	if err != nil {
		return nil, fmt.Errorf("resolve artifact root: %w", err)
	}
	if err := os.MkdirAll(abs, 0o700); err != nil {
		return nil, fmt.Errorf("create artifact root: %w", err)
	}
	return &ArtifactStore{root: abs}, nil
}

func (s *ArtifactStore) workItemDir(workItemID string) (string, error) {
	if !workItemIDPattern.MatchString(workItemID) {
		return "", ErrInvalidWorkItem
	}
	return filepath.Join(s.root, workItemID), nil
}

func (s *ArtifactStore) DeleteWorkItem(workItemID string) error {
	dir, err := s.workItemDir(workItemID)
	if err != nil {
		return err
	}
	if err := os.RemoveAll(dir); err != nil {
		return fmt.Errorf("delete work-item artifacts: %w", err)
	}
	return nil
}

func (s *ArtifactStore) path(workItemID, name string) (string, error) {
	dir, err := s.workItemDir(workItemID)
	if err != nil {
		return "", err
	}
	return filepath.Join(dir, name), nil
}

// PutProposal stores the original request once. It is immutable after the
// workflow is admitted, so authoring a plan can never overwrite the ask it is
// supposed to satisfy. Repeating the same write is idempotent.
func (s *ArtifactStore) PutProposal(workItemID string, content []byte) error {
	path, err := s.path(workItemID, "proposal.md")
	if err != nil {
		return err
	}
	if err := os.MkdirAll(filepath.Dir(path), 0o700); err != nil {
		return fmt.Errorf("create work-item artifact directory: %w", err)
	}

	f, err := os.OpenFile(path, os.O_WRONLY|os.O_CREATE|os.O_EXCL, 0o600)
	if errors.Is(err, os.ErrExist) {
		existing, readErr := os.ReadFile(path)
		if readErr != nil {
			return fmt.Errorf("read existing proposal: %w", readErr)
		}
		if !bytes.Equal(existing, content) {
			return ErrImmutable
		}
		return nil
	}
	if err != nil {
		return fmt.Errorf("create proposal: %w", err)
	}
	if err := writeAndSync(f, content); err != nil {
		_ = f.Close()
		_ = os.Remove(path)
		return fmt.Errorf("write proposal: %w", err)
	}
	if err := f.Close(); err != nil {
		return fmt.Errorf("close proposal: %w", err)
	}
	return syncDir(filepath.Dir(path))
}

func (s *ArtifactStore) ImportProposal(workItemID, sourcePath string) error {
	content, err := os.ReadFile(sourcePath)
	if err != nil {
		return fmt.Errorf("read source proposal: %w", err)
	}
	return s.PutProposal(workItemID, content)
}

func (s *ArtifactStore) Proposal(workItemID string) ([]byte, error) {
	return s.read(workItemID, "proposal.md")
}

// PutPlan atomically replaces only the mutable plan artifact. The proposal is a
// different inode and cannot be changed through this API.
func (s *ArtifactStore) PutPlan(workItemID string, content []byte) error {
	return s.replace(workItemID, "plan.md", content)
}

func (s *ArtifactStore) Plan(workItemID string) ([]byte, error) {
	return s.read(workItemID, "plan.md")
}

// Artifact is the typed output of one workflow node. Content remains an
// unrestricted byte slice: a producer either publishes the complete output or
// returns an error. The hash is derived by the store and is never trusted from
// an external runner.
type Artifact struct {
	Type    string `json:"type"`
	Content []byte `json:"content"`
	Hash    string `json:"hash"`
}

// PutNodeArtifact atomically publishes the latest output for a graph node. Node
// outputs are mutable across refinement iterations, but they never overwrite
// the immutable proposal or another node's output.
func (s *ArtifactStore) PutNodeArtifact(workItemID, nodeID, artifactType string, content []byte) (Artifact, error) {
	if !artifactNodeIDPattern.MatchString(nodeID) {
		return Artifact{}, ErrInvalidNode
	}
	if artifactType == "" {
		artifactType = "opaque"
	}
	artifact := Artifact{Type: artifactType, Content: content, Hash: Hash(content)}
	encoded, err := json.Marshal(artifact)
	if err != nil {
		return Artifact{}, fmt.Errorf("encode node artifact: %w", err)
	}
	if err := s.replace(workItemID, "node-"+nodeID+".json", encoded); err != nil {
		return Artifact{}, err
	}
	return artifact, nil
}

func (s *ArtifactStore) NodeArtifact(workItemID, nodeID string) (Artifact, error) {
	if !artifactNodeIDPattern.MatchString(nodeID) {
		return Artifact{}, ErrInvalidNode
	}
	content, err := s.read(workItemID, "node-"+nodeID+".json")
	if err != nil {
		return Artifact{}, err
	}
	var artifact Artifact
	if err := json.Unmarshal(content, &artifact); err != nil {
		return Artifact{}, fmt.Errorf("decode node artifact: %w", err)
	}
	if artifact.Type == "" || artifact.Hash != Hash(artifact.Content) {
		return Artifact{}, errors.New("node artifact failed integrity validation")
	}
	return artifact, nil
}

// Findings and review feedback are the roundtable's vocabulary, and the
// roundtable now owns them: it runs as a module process whose sources cannot
// import internal/, so the definitions live there and the control plane refers
// to them here. These are aliases, not copies -- one type, no conversion, and
// no way for the two sides to drift apart.
type Finding = panel.Finding
type ReviewFeedback = panel.ReviewFeedback

func (s *ArtifactStore) PutFeedback(workItemID string, feedback ReviewFeedback) error {
	if feedback.SchemaVersion == 0 {
		feedback.SchemaVersion = 1
	}
	content, err := json.Marshal(feedback)
	if err != nil {
		return fmt.Errorf("encode feedback: %w", err)
	}
	return s.replace(workItemID, "feedback.json", content)
}

func (s *ArtifactStore) Feedback(workItemID string) (ReviewFeedback, error) {
	content, err := s.read(workItemID, "feedback.json")
	if err != nil {
		return ReviewFeedback{}, err
	}
	var feedback ReviewFeedback
	if err := json.Unmarshal(content, &feedback); err != nil {
		return ReviewFeedback{}, fmt.Errorf("decode feedback: %w", err)
	}
	return feedback, nil
}

// ReviewPacket carries complete strings. There is deliberately no Truncated
// flag: this layer either returns the complete documents or an error.
type ReviewPacket struct {
	Proposal    string          `json:"proposal"`
	Plan        string          `json:"plan"`
	PlanHash    string          `json:"plan_hash"`
	PriorReview *ReviewFeedback `json:"prior_review,omitempty"`
}

func (s *ArtifactStore) PlanReviewPacket(workItemID string) (ReviewPacket, error) {
	proposal, err := s.Proposal(workItemID)
	if err != nil {
		return ReviewPacket{}, err
	}
	plan, err := s.Plan(workItemID)
	if err != nil {
		return ReviewPacket{}, err
	}
	return ReviewPacket{
		Proposal: string(proposal),
		Plan:     string(plan),
		PlanHash: Hash(plan),
	}, nil
}

func Hash(content []byte) string {
	sum := sha256.Sum256(content)
	return hex.EncodeToString(sum[:])
}

func (s *ArtifactStore) read(workItemID, name string) ([]byte, error) {
	path, err := s.path(workItemID, name)
	if err != nil {
		return nil, err
	}
	content, err := os.ReadFile(path)
	if err != nil {
		return nil, fmt.Errorf("read %s: %w", name, err)
	}
	return content, nil
}

func (s *ArtifactStore) replace(workItemID, name string, content []byte) error {
	path, err := s.path(workItemID, name)
	if err != nil {
		return err
	}
	dir := filepath.Dir(path)
	if err := os.MkdirAll(dir, 0o700); err != nil {
		return fmt.Errorf("create work-item artifact directory: %w", err)
	}
	tmp, err := os.CreateTemp(dir, "."+name+".*.tmp")
	if err != nil {
		return fmt.Errorf("create temporary artifact: %w", err)
	}
	tmpPath := tmp.Name()
	defer os.Remove(tmpPath)
	if err := tmp.Chmod(0o600); err != nil {
		_ = tmp.Close()
		return fmt.Errorf("set artifact permissions: %w", err)
	}
	if err := writeAndSync(tmp, content); err != nil {
		_ = tmp.Close()
		return fmt.Errorf("write temporary artifact: %w", err)
	}
	if err := tmp.Close(); err != nil {
		return fmt.Errorf("close temporary artifact: %w", err)
	}
	if err := os.Rename(tmpPath, path); err != nil {
		return fmt.Errorf("publish artifact: %w", err)
	}
	return syncDir(dir)
}

func writeAndSync(w *os.File, content []byte) error {
	if _, err := io.Copy(w, bytes.NewReader(content)); err != nil {
		return err
	}
	return w.Sync()
}

func syncDir(path string) error {
	dir, err := os.Open(path)
	if err != nil {
		return fmt.Errorf("open artifact directory: %w", err)
	}
	defer dir.Close()
	if err := dir.Sync(); err != nil {
		// Some durable appliance filesystems (including SmoothFS) make file
		// fsync durable but reject fsync on directory descriptors with EINVAL or
		// ENOTSUP. The rename/write has already succeeded; treat only those
		// explicit "operation unsupported" results as the portable fallback.
		if errors.Is(err, syscall.EINVAL) || errors.Is(err, syscall.ENOTSUP) {
			return nil
		}
		return fmt.Errorf("sync artifact directory: %w", err)
	}
	return nil
}
