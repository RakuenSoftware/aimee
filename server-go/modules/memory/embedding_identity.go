package memory

import (
	"bytes"
	"crypto/sha256"
	"encoding/binary"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"hash"
	"sort"
	"strconv"
	"strings"
	"unicode/utf8"
)

// EmbeddingIdentity commits the vector space, not a friendly model alias.
// ProviderConfiguration contains public behavior settings only; endpoints,
// credentials, request IDs and operational counters are not identity material.
type EmbeddingIdentity struct {
	SchemaVersion         int               `json:"schema_version"`
	Model                 string            `json:"model"`
	ArtifactRevision      string            `json:"artifact_revision"`
	TokenizerRevision     string            `json:"tokenizer_revision"`
	QueryPreprocessing    string            `json:"query_preprocessing"`
	DocumentPreprocessing string            `json:"document_preprocessing"`
	QueryPrefix           string            `json:"query_prefix"`
	DocumentPrefix        string            `json:"document_prefix"`
	Pooling               string            `json:"pooling"`
	Normalization         string            `json:"normalization"`
	Dimensions            int               `json:"dimensions"`
	ProviderConfiguration map[string]string `json:"provider_configuration"`
}

// Length-prefixed UTF-8 has identical commitments in Go and provider runtimes,
// including non-ASCII prefixes, without a language-specific JSON canonicalizer.
func identityPart(h hash.Hash, value string) {
	var size [4]byte
	binary.BigEndian.PutUint32(size[:], uint32(len(value)))
	h.Write(size[:])
	h.Write([]byte(value))
}
func immutableEmbeddingRevision(value string) bool {
	if len(value) != 40 && len(value) != 64 {
		return false
	}
	_, err := hex.DecodeString(value)
	return err == nil && strings.ToLower(value) == value
}
func (i EmbeddingIdentity) Digest() (string, error) {
	if i.SchemaVersion != 1 || i.Dimensions < 1 || i.Dimensions > 4000 || !immutableEmbeddingRevision(i.ArtifactRevision) || !immutableEmbeddingRevision(i.TokenizerRevision) {
		return "", errors.New("memory: incomplete embedding identity")
	}
	values := []string{"embedding-identity-v1", strconv.Itoa(i.SchemaVersion), i.Model, i.ArtifactRevision, i.TokenizerRevision, i.QueryPreprocessing, i.DocumentPreprocessing, i.QueryPrefix, i.DocumentPrefix, i.Pooling, i.Normalization, strconv.Itoa(i.Dimensions)}
	for n, value := range values {
		if len(value) > 4096 || !utf8.ValidString(value) || (value == "" && n != 7 && n != 8) {
			return "", errors.New("memory: invalid embedding identity field")
		}
	}
	if len(i.ProviderConfiguration) == 0 || len(i.ProviderConfiguration) > 16 {
		return "", errors.New("memory: missing or excessive embedding provider configuration")
	}
	keys := make([]string, 0, len(i.ProviderConfiguration))
	for key, value := range i.ProviderConfiguration {
		if key == "" || len(key) > 128 || value == "" || len(value) > 4096 || !utf8.ValidString(key) || !utf8.ValidString(value) {
			return "", errors.New("memory: invalid embedding provider configuration")
		}
		keys = append(keys, key)
	}
	sort.Strings(keys)
	h := sha256.New()
	for _, value := range values {
		identityPart(h, value)
	}
	identityPart(h, strconv.Itoa(len(keys)))
	for _, key := range keys {
		identityPart(h, key)
		identityPart(h, i.ProviderConfiguration[key])
	}
	return fmt.Sprintf("sha256:%x", h.Sum(nil)), nil
}

func embeddingIdentityResponse(body []byte) (EmbedResponse, bool) {
	var health map[string]json.RawMessage
	if json.Unmarshal(body, &health) != nil {
		return EmbedResponse{}, false
	}
	raw, exists := health["embedding_identity"]
	if !exists {
		return EmbedResponse{}, false
	}
	var identity *EmbeddingIdentity
	decoder := json.NewDecoder(bytes.NewReader(raw))
	decoder.DisallowUnknownFields()
	if decoder.Decode(&identity) != nil || identity == nil {
		return EmbedResponse{Error: "embed: invalid complete embedding identity", IdentityState: "identity_mismatch"}, true
	}
	digest, err := identity.Digest()
	var declared string
	if err != nil || json.Unmarshal(health["embedding_identity_digest"], &declared) != nil || declared != digest {
		return EmbedResponse{Error: "embed: embedding identity commitment mismatch", IdentityState: "identity_mismatch"}, true
	}
	return EmbedResponse{ServingID: "embedding-v1:" + digest, Dim: identity.Dimensions, IdentityState: "verified", EmbeddingIdentityDigest: digest, EmbeddingIdentity: identity}, true
}

func embeddingIdentityState(servingID string) string {
	if strings.HasPrefix(servingID, "embedding-v1:sha256:") && len(servingID) == len("embedding-v1:sha256:")+64 {
		if _, err := hex.DecodeString(strings.TrimPrefix(servingID, "embedding-v1:sha256:")); err == nil {
			return "verified"
		}
	}
	return "legacy_unknown"
}

// Index identity additionally binds the owner extraction/chunking and distance
// policy. It must change even when the provider keeps the same vector space.
type IndexGenerationIdentity struct {
	EmbeddingDigest string `json:"embedding_digest"`
	ChunkingPolicy  string `json:"chunking_policy"`
	ExtractorPolicy string `json:"extractor_policy"`
	DistancePolicy  string `json:"distance_policy"`
	IndexSettings   string `json:"index_settings"`
}

func (i IndexGenerationIdentity) Digest() (string, error) {
	if !strings.HasPrefix(i.EmbeddingDigest, "sha256:") || len(i.EmbeddingDigest) != 71 {
		return "", errors.New("memory: index requires embedding identity commitment")
	}
	if _, err := hex.DecodeString(i.EmbeddingDigest[7:]); err != nil {
		return "", errors.New("memory: invalid index embedding commitment")
	}
	h := sha256.New()
	for _, value := range []string{"index-generation-v1", i.EmbeddingDigest, i.ChunkingPolicy, i.ExtractorPolicy, i.DistancePolicy, i.IndexSettings} {
		if value == "" || len(value) > 4096 || !utf8.ValidString(value) {
			return "", errors.New("memory: incomplete index generation identity")
		}
		identityPart(h, value)
	}
	return fmt.Sprintf("sha256:%x", h.Sum(nil)), nil
}
