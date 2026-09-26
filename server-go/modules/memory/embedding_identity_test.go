package memory

import (
	"context"
	"encoding/json"
	"strings"
	"testing"
)

func fixtureEmbeddingIdentity() EmbeddingIdentity {
	return EmbeddingIdentity{SchemaVersion: 1, Model: "fixture/model", ArtifactRevision: strings.Repeat("a", 40), TokenizerRevision: strings.Repeat("b", 40), QueryPreprocessing: "utf8-prefix-encode-v1", DocumentPreprocessing: "utf8-prefix-encode-v1", QueryPrefix: "query: café\u2028", Pooling: "mean", Normalization: "l2", Dimensions: 384, ProviderConfiguration: map[string]string{"engine": "stub", "quantization": "fp32"}}
}
func TestEmbeddingIdentityCommitment(t *testing.T) {
	baseline := fixtureEmbeddingIdentity()
	digest, err := baseline.Digest()
	if err != nil || digest != "sha256:3b693cabcab598c60cee8666c39fb077d7eb23ab256bbbd559cab93512c1b66e" {
		t.Fatal(digest, err)
	}
	for name, change := range map[string]func(*EmbeddingIdentity){
		"artifact":               func(i *EmbeddingIdentity) { i.ArtifactRevision = strings.Repeat("c", 40) },
		"tokenizer":              func(i *EmbeddingIdentity) { i.TokenizerRevision = strings.Repeat("c", 40) },
		"query-prefix":           func(i *EmbeddingIdentity) { i.QueryPrefix += " " },
		"document-prefix":        func(i *EmbeddingIdentity) { i.DocumentPrefix = "document: " },
		"query-preprocessing":    func(i *EmbeddingIdentity) { i.QueryPreprocessing = "v2" },
		"document-preprocessing": func(i *EmbeddingIdentity) { i.DocumentPreprocessing = "v2" },
		"pooling":                func(i *EmbeddingIdentity) { i.Pooling = "cls" },
		"normalization":          func(i *EmbeddingIdentity) { i.Normalization = "none" },
		"dimension":              func(i *EmbeddingIdentity) { i.Dimensions = 768 },
		"quantization": func(i *EmbeddingIdentity) {
			i.ProviderConfiguration = map[string]string{"engine": "stub", "quantization": "int8"}
		},
	} {
		t.Run(name, func(t *testing.T) {
			candidate := baseline
			change(&candidate)
			got, err := candidate.Digest()
			if err != nil || got == digest {
				t.Fatal(got, err)
			}
		})
	}
	for _, revision := range []string{"", "main", "HEAD", strings.Repeat("A", 40)} {
		bad := baseline
		bad.ArtifactRevision = revision
		if _, err := bad.Digest(); err == nil {
			t.Fatal("accepted unpinned artifact", revision)
		}
	}
	bad := baseline
	bad.ProviderConfiguration = nil
	if _, err := bad.Digest(); err == nil {
		t.Fatal("missing provider configuration accepted")
	}
	index := IndexGenerationIdentity{EmbeddingDigest: digest, ChunkingPolicy: "whole-v1", ExtractorPolicy: "memory-v1", DistancePolicy: "cosine-v1", IndexSettings: "exact-v1"}
	first, err := index.Digest()
	if err != nil {
		t.Fatal(err)
	}
	index.ChunkingPolicy = "whole-v2"
	second, err := index.Digest()
	if err != nil || first == second {
		t.Fatal("chunking change reused generation")
	}
}
func TestEmbeddingHealthCompleteIdentityAndLegacy(t *testing.T) {
	identity := fixtureEmbeddingIdentity()
	digest, _ := identity.Digest()
	health := map[string]any{"embedding_identity": identity, "embedding_identity_digest": digest, "serving_id": "legacy-compatible"}
	body, _ := json.Marshal(health)
	stub := &embedStub{reply: string(body)}
	endpoint := stub.start(t)
	out := EmbedServingID(context.Background(), 0, allowEgress, endpoint)
	if out.Error != "" || out.IdentityState != "verified" || out.ServingID != "embedding-v1:"+digest {
		t.Fatal(out)
	}
	// A provider cannot advertise one full identity and silently fall back to a
	// friendly legacy alias when its commitment is absent, malformed or changed.
	for _, bad := range []string{`{"embedding_identity":null,"serving_id":"legacy-compatible"}`, `{"embedding_identity":{},"serving_id":"legacy-compatible"}`, strings.Replace(string(body), digest, "sha256:"+strings.Repeat("0", 64), 1), `{"serving_id":"embedding-v1:` + digest + `"}`} {
		stub.reply = bad
		out = EmbedServingID(context.Background(), 0, allowEgress, endpoint)
		if out.Error == "" || out.ServingID != "" {
			t.Fatal("identity fallback accepted", out)
		}
	}
	stub.reply = `{"model":"legacy-model","dim":384}`
	out = EmbedServingID(context.Background(), 0, allowEgress, endpoint)
	if out.Error != "" || out.IdentityState != "legacy_unknown" || out.ServingID != "legacy-model" {
		t.Fatal(out)
	}
}
