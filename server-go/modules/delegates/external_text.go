package delegates

import (
	"errors"
	canonical "github.com/JBailes/aimee/server-go/ir"
	"unicode/utf8"
)

// The declared generic protocol is UTF-8 application text on stdout, exit zero
// meaning transport completion. It is not SDK JSON, governed tool telemetry or
// a fallback for native providers. stderr is always a separate diagnostic.
func externalTextObservation(output []byte) (*canonical.Response, []canonical.Message, error) {
	if !utf8.Valid(output) {
		return nil, nil, errors.New("external text adapter output is not UTF-8")
	}
	response := canonical.TextResponse(string(output))
	_, err := response.SemanticText()
	return response, []canonical.Message{{Role: "assistant", Blocks: []canonical.Block{response.Blocks[0].Block}}}, err
}
