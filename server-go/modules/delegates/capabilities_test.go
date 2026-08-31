package delegates

import (
	"encoding/binary"
	"strings"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
)

func capRequest(prompt string, tools bool) []byte {
	request := make([]byte, capHeaderLen+len(prompt))
	binary.LittleEndian.PutUint32(request[0:4], capRequestMagic)
	request[4] = wireVersion
	if tools {
		request[5] = 1
	}
	binary.LittleEndian.PutUint32(request[8:12], uint32(len(prompt)))
	copy(request[capHeaderLen:], prompt)
	return request
}

func capCall(t *testing.T, prompt string, tools bool) (uint32, int) {
	t.Helper()
	response, status := Handle(bus.ModuleInvocation{StageID: StageCapabilities},
		capRequest(prompt, tools))
	if status != bus.ModuleStatusOK || len(response) != capResponseLen ||
		binary.LittleEndian.Uint32(response[0:4]) != capResponseMagic {
		t.Fatalf("response = %x, status = %d", response, status)
	}
	return binary.LittleEndian.Uint32(response[4:8]),
		int(binary.LittleEndian.Uint32(response[8:12]))
}

// Ported one-for-one from test_cmd_delegate.c. A prompt that DESCRIBES audio
// work in code must not force an audio-capable model; only a reference to an
// actual audio file may. The bare word "audio" appears in any prompt about
// building speech features and means nothing about what the model must decode.
func TestAudioComesFromFileExtensionsNotTheWord(t *testing.T) {
	describes := []string{
		"Implement an STT (speech-to-text) dispatcher and audio routing module.",
		"Add audio support to the gateway — implement the audio platform adapter.",
	}
	for _, prompt := range describes {
		if caps, _ := capCall(t, prompt, false); caps&CapAudio != 0 {
			t.Errorf("%q required audio", prompt)
		}
	}

	references := []string{
		"Transcribe recording.mp3 into text.",
		"Process speech.wav and output captions.",
		"Convert podcast.m4a to a transcript.",
	}
	for _, prompt := range references {
		if caps, _ := capCall(t, prompt, false); caps&CapAudio == 0 {
			t.Errorf("%q did not require audio", prompt)
		}
	}
}

func TestModalitiesAndContextAreInferredTogether(t *testing.T) {
	long := "Analyze image screenshot.png and report pdf coverage. " + strings.Repeat("a", 20031)
	caps, minContext := capCall(t, long, true)
	for _, want := range []struct {
		bit  uint32
		name string
	}{{CapTools, "tools"}, {CapVision, "vision"}, {CapPDF, "pdf"}} {
		if caps&want.bit == 0 {
			t.Errorf("missing %s", want.name)
		}
	}
	if minContext <= 0 {
		t.Errorf("min context = %d, want a bound for a %d-char prompt", minContext, len(long))
	}
}

// A short prompt must not constrain the window at all: filtering on it would
// rule out models that would have been perfectly adequate.
func TestShortPromptLeavesTheWindowUnconstrained(t *testing.T) {
	if _, minContext := capCall(t, "fix the typo", false); minContext != 0 {
		t.Errorf("min context = %d, want 0", minContext)
	}
	// Just over the threshold, it does bind.
	if _, minContext := capCall(t, strings.Repeat("a", 4097*4), false); minContext == 0 {
		t.Error("a materially large prompt left the window unconstrained")
	}
}

// Tools is the one capability that is asserted rather than inferred, so it must
// not depend on the prompt.
func TestToolsIsAssertedByTheCallerNotReadFromThePrompt(t *testing.T) {
	if caps, _ := capCall(t, "", true); caps != CapTools {
		t.Errorf("empty prompt with tools = %#x, want exactly CapTools", caps)
	}
	if caps, _ := capCall(t, "", false); caps != 0 {
		t.Errorf("empty prompt without tools = %#x, want none", caps)
	}
}

// Markdown image syntax appears in any diff or doc under review and never means
// the model must decode an image.
func TestMarkdownImageSyntaxIsNotAVisionTrigger(t *testing.T) {
	if caps, _ := capCall(t, "review this diff: ![alt](docs/diagram)", false); caps&CapVision != 0 {
		t.Error("markdown image syntax required vision")
	}
}

func TestCapabilitiesRejectsInvalidEnvelope(t *testing.T) {
	short := capRequest("x", false)[:capHeaderLen-1]
	if _, status := Handle(bus.ModuleInvocation{StageID: StageCapabilities}, short); status != bus.ModuleStatusInvalidRequest {
		t.Fatalf("truncated-header status = %d", status)
	}

	lying := capRequest("x", false)
	binary.LittleEndian.PutUint32(lying[8:12], 64)
	if _, status := Handle(bus.ModuleInvocation{StageID: StageCapabilities}, lying); status != bus.ModuleStatusInvalidRequest {
		t.Fatalf("length-mismatch status = %d", status)
	}

	badTools := capRequest("x", false)
	badTools[5] = 2
	if _, status := Handle(bus.ModuleInvocation{StageID: StageCapabilities}, badTools); status != bus.ModuleStatusInvalidRequest {
		t.Fatalf("out-of-range tools flag status = %d", status)
	}

	if _, status := Handle(bus.ModuleInvocation{StageID: StageCapabilities, DeadlineNS: 1},
		capRequest("x", false)); status != bus.ModuleStatusCancelled {
		t.Fatalf("expired status = %d", status)
	}
}
