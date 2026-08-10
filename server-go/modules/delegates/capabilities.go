package delegates

import (
	"encoding/binary"
	"strings"

	"github.com/JBailes/aimee/server-go/bus"
)

// What a prompt implies a model must be able to do.
//
// This is a routing heuristic, not a fact about the request: it reads a prompt
// for signs that the work needs a modality the text alone cannot carry, so the
// router can refuse a model that would silently ignore half the input.

const (
	StageCapabilities uint32 = 2
	EventCapabilities uint32 = 6658

	capRequestMagic  uint32 = 0x50414344 /* "DCAP" */
	capResponseMagic uint32 = 0x53414344 /* "DCAS" */
	capPromptMax            = 1 << 20
	capHeaderLen            = 12
	capResponseLen          = 12

	// Mirrors model_registry.h. A model capability is a property of the model,
	// so the numbering is the registry's and not this module's to choose.
	CapTools  uint32 = 1 << 1
	CapVision uint32 = 1 << 2
	CapPDF    uint32 = 1 << 3
	CapAudio  uint32 = 1 << 4
)

// Markdown image syntax ("![") is deliberately absent: it appears in any doc or
// diff under review and never means the model must decode an image.
var visionMarkers = []string{".png", ".jpg", ".jpeg", ".webp", ".gif", "screenshot", "image_url"}

var pdfMarkers = []string{".pdf", " pdf "}

// Audio is required only for actual audio file formats. The bare word "audio"
// was removed: a prompt implementing speech features in code contains it while
// needing nothing of the model.
var audioMarkers = []string{".mp3", ".wav", ".m4a", ".ogg", ".flac", ".aac"}

func containsAny(haystack string, needles []string) bool {
	for _, needle := range needles {
		if strings.Contains(haystack, needle) {
			return true
		}
	}
	return false
}

// InferCapabilities reports the capabilities a prompt implies and the smallest
// context window that can hold it. min is 0 when the prompt is short enough not
// to constrain the choice.
func InferCapabilities(prompt string, toolsEnabled bool) (required uint32, minContext int) {
	if toolsEnabled {
		required |= CapTools
	}
	if prompt == "" {
		return required, 0
	}

	lower := strings.ToLower(prompt)
	if containsAny(lower, visionMarkers) {
		required |= CapVision
	}
	if containsAny(lower, pdfMarkers) {
		required |= CapPDF
	}
	if containsAny(lower, audioMarkers) {
		required |= CapAudio
	}

	// Roughly one token per four characters. Only a materially large prompt
	// constrains the window; enforcing a minimum on short prompts would filter
	// out models that would have been fine.
	estimated := len(prompt)/4 + 1
	if estimated > 4096 {
		minContext = estimated + 1024
	}
	return required, minContext
}

func handleCapabilities(invocation bus.ModuleInvocation, request []byte) ([]byte, bus.ModuleStatus) {
	if len(request) < capHeaderLen ||
		binary.LittleEndian.Uint32(request[0:4]) != capRequestMagic || request[4] != wireVersion {
		return nil, bus.ModuleStatusInvalidRequest
	}
	tools := request[5]
	if tools > 1 {
		return nil, bus.ModuleStatusInvalidRequest
	}
	length := int(binary.LittleEndian.Uint32(request[8:12]))
	if length > capPromptMax || len(request) != capHeaderLen+length {
		return nil, bus.ModuleStatusInvalidRequest
	}
	if invocation.Cancelled() {
		return nil, bus.ModuleStatusCancelled
	}

	required, minContext := InferCapabilities(string(request[capHeaderLen:]), tools == 1)
	response := make([]byte, capResponseLen)
	binary.LittleEndian.PutUint32(response[0:4], capResponseMagic)
	binary.LittleEndian.PutUint32(response[4:8], required)
	binary.LittleEndian.PutUint32(response[8:12], uint32(minContext))
	return response, bus.ModuleStatusOK
}
