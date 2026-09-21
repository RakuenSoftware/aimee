package memory

import (
	"crypto/sha256"
	"encoding/hex"
	"testing"
	"time"
)

func TestMemoryFactEvidenceUsesExactByteSpan(t *testing.T) {
	content := "prefix exact support suffix"
	actor := modelFactActor()
	evidence := memoryFactEvidence(content, 7, 20, actor, "2026-09-22 00:00:00", 9, 12)
	wantHash := sha256.Sum256([]byte("exact support"))
	if evidence.SourceSpan != "bytes:7-20" || evidence.EvidenceHash != hex.EncodeToString(wantHash[:]) {
		t.Fatalf("unexpected evidence: %+v", evidence)
	}
}

func TestMemoryFactGroundingParity(t *testing.T) {
	note := normalizeFactText("The KB server has hostname aimee-kb and IP 10.20.0.15.", 4096)
	for _, value := range []string{"KB server", "kb_server", "aimee-kb", "10.20.0.15"} {
		if !factGrounded(value, note) {
			t.Errorf("expected %q to be grounded in %q", value, note)
		}
	}
	for _, value := range []string{"Rakuen Software", "Jonathan Bailes"} {
		if factGrounded(value, note) {
			t.Errorf("did not expect %q to be grounded in %q", value, note)
		}
	}
	note = normalizeFactText("Ingrid mentors two of the junior engineers.", 4096)
	if !factGrounded("two of the junior engineers", note) ||
		!factGrounded("junior engineers", note) || factGrounded("senior architects", note) {
		t.Fatalf("majority-word grounding mismatch for %q", note)
	}
}

func TestParseModelFactCandidatesGroundsCanonicalizesAndAssignsKinds(t *testing.T) {
	content := "I work at Acme. My workstation has IP 10.0.0.4."
	response := `prefix {"facts":[` +
		`{"subject":"user","relation":"works_at","object":"Acme","confidence":0.0,"source_start":0,"source_end":14},` +
		`{"subject":"user","relation":"knows","object":"Invented Person","confidence":0.99,"source_start":0,"source_end":14}` +
		`]} suffix`
	candidates, err := parseModelFactCandidates(response, content, "2026-09-22 00:00:00", 4, 7,
		FactActor{Principal: "user:1", Role: "user", Rank: 30})
	if err != nil {
		t.Fatal(err)
	}
	if len(candidates) != 1 {
		t.Fatalf("got %d candidates: %+v", len(candidates), candidates)
	}
	got := candidates[0]
	if got.Relation != "works_for" || got.SubjectKind != NodePerson || got.ObjectKind != NodeOrg {
		t.Fatalf("canonical candidate mismatch: %+v", got)
	}
	if got.Actor.Rank != 10 || got.Evidence.ActorPrincipal != "user:1" {
		t.Fatalf("model authority or source provenance mismatch: %+v", got)
	}
}

func TestParseModelFactCandidatesRejectsInvalidSpans(t *testing.T) {
	for _, response := range []string{
		`{"facts":[{"subject":"user","relation":"works_for","object":"Acme","source_start":-1,"source_end":4}]}`,
		`{"facts":[{"subject":"user","relation":"works_for","object":"Acme","source_start":0.5,"source_end":4}]}`,
		`{"facts":[{"subject":"user","relation":"works_for","object":"Acme","source_start":0,"source_end":999}]}`,
	} {
		got, err := parseModelFactCandidates(response, "I work at Acme", "now", 1, 2, modelFactActor())
		if err != nil {
			t.Fatal(err)
		}
		if len(got) != 0 {
			t.Fatalf("invalid span produced candidates: %+v", got)
		}
	}
}

func TestParseModelFactCandidatesTreatsMalformedAndBareEmptyAsAbstention(t *testing.T) {
	for _, response := range []string{"[]", "not json", `{}`, `{"facts":[]}`} {
		got, err := parseModelFactCandidates(response, "a note", "now", 1, 2, modelFactActor())
		if err != nil || len(got) != 0 {
			t.Fatalf("response %q: got=%+v err=%v", response, got, err)
		}
	}
}

func TestPatternFactCandidatesUseCapturedActorAndSeedKinds(t *testing.T) {
	actor := FactActor{Principal: "user:1", Role: "user", Rank: 30, Authenticated: 1}
	got := patternFactCandidates("my age is 41", "now", 2, 3, actor)
	if len(got) != 1 || got[0].SubjectKind != NodePerson || got[0].ObjectKind != NodeScalar ||
		got[0].Actor.Principal != actor.Principal {
		t.Fatalf("unexpected pattern candidates: %+v", got)
	}
}

func TestMemoryFactRetryIsExponentialCappedAndJittered(t *testing.T) {
	for attempts, want := range map[int]time.Duration{
		-1: 30 * time.Second, 1: 30 * time.Second, 2: time.Minute,
		3: 2 * time.Minute, 8: time.Hour, 20: time.Hour,
	} {
		if got := memoryFactRetryBase(attempts); got != want {
			t.Errorf("attempts %d: base=%v want=%v", attempts, got, want)
		}
		for range 32 {
			got := memoryFactRetryDelay(attempts)
			if got < want-want/10 || got > want+want/10 {
				t.Fatalf("attempts %d: jittered delay %v outside +/-10%% of %v", attempts, got, want)
			}
		}
	}
}

func TestMemoryFactProviderUnavailableParity(t *testing.T) {
	for _, reason := range []string{
		"provider HTTP 503", "provider HTTP 429", "provider HTTP -1",
		"llm-chat: HTTP 503 from http://aimee-llm",
		`{"error":{"code":"provider_unavailable"}}`,
		"synth upstream circuit is open",
		"request to http://aimee-llm failed after 1 tries: timed out",
		"no synthesis endpoint configured: set SYNTHESIS_ENDPOINT",
	} {
		if !memoryFactProviderUnavailable(reason) {
			t.Errorf("expected provider-unavailable classification for %q", reason)
		}
	}
	for _, reason := range []string{
		"", "provider HTTP 400", "provider HTTP 422", "sidecar returned non-JSON",
		"kb_documents row not found", "artifact write failed", "local parser timed out",
	} {
		if memoryFactProviderUnavailable(reason) {
			t.Errorf("did not expect provider-unavailable classification for %q", reason)
		}
	}
}
