package memory

import (
	"bytes"
	"encoding/hex"
	"encoding/json"
	"strings"

	memorycontract "github.com/JBailes/aimee/server-go/memory"
)

type EmbeddingIdentity = memorycontract.EmbeddingIdentity
type IndexGenerationIdentity = memorycontract.IndexGenerationIdentity

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
