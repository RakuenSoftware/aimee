package panel

import (
	"encoding/json"
	"fmt"
	canonical "github.com/JBailes/aimee/server-go/ir"
	"strings"
)

// reviewInput retains the original conversation and controls. The review
// procedure and immutable artifact originate here as separate canonical blocks.
func reviewInput(run Run, persona, procedure string, evidence ...string) (*canonical.Request, error) {
	source := run.Request
	if source == nil {
		source = canonical.TaskRequest("", "")
		source.Messages = []canonical.Message{} // independent analysis task origin
	}

	encoded, err := json.Marshal(source)
	if err != nil {
		return nil, err
	}
	result, err := canonical.DecodeRequest(encoded)
	if err != nil {
		return nil, err
	}
	profile := canonical.TaskRequest(persona, "")
	result.System = append(result.System, profile.System...)
	blocks := []canonical.Block{{Type: "text", Text: &procedure,
		Context: &canonical.Context{Origin: 1, Authority: 2, Trust: 4, Sensitivity: 1, ModelVisible: true}}}
	if run.Focus != "" {
		focus := "Review focus: " + run.Focus
		blocks = append(blocks, canonical.Block{Type: "text", Text: &focus, Context: &canonical.Context{Origin: 3, Authority: 2, Trust: 2, Sensitivity: 1, ModelVisible: true}})
	}
	for _, data := range evidence {
		retained := data
		blocks = append(blocks, canonical.Block{Type: "text", Text: &retained, Context: &canonical.Context{Origin: 7, Authority: 1, Trust: 2, Sensitivity: 1, ModelVisible: true}})
	}

	if run.Reviewed.Content != "" {
		artifact := "BEGIN_ARTIFACT_DATA (" + run.Reviewed.Stage + ")\n" + run.Reviewed.Content + "\nEND_ARTIFACT_DATA"
		blocks = append(blocks, canonical.Block{Type: "text", Text: &artifact,
			Context: &canonical.Context{Origin: 5, Authority: 1, Trust: 2, Sensitivity: 1, ModelVisible: true}})
	}
	if len(run.Brief) > 0 {
		brief := "BEGIN_REVIEW_BRIEF_DATA\n" + string(run.Brief) + "\nEND_REVIEW_BRIEF_DATA"
		blocks = append(blocks, canonical.Block{Type: "text", Text: &brief, Context: &canonical.Context{Origin: 3, Authority: 1, Trust: 2, Sensitivity: 1, ModelVisible: true}})
	}
	result.Messages = append(result.Messages, canonical.Message{Role: "user", Blocks: blocks})
	if !result.Valid() {
		return nil, fmt.Errorf("canonical review request exceeds supported bounds")
	}
	return result, nil
}

// A review needs an admitted original conversation, not an empty text alias.
func hasReviewSource(request *canonical.Request) bool {
	if request == nil || !request.Valid() {
		return false
	}
	for _, message := range request.Messages {
		if message.Role != "user" {
			continue
		}
		for _, block := range message.Blocks {
			if c := block.Context; c != nil {
				classified := c.Origin != 0 || c.Authority != 0 || c.Trust != 0 || c.Sensitivity != 0 || c.RevisionDomain != "" || c.RevisionScope != "" || (c.RevisionEpoch != "" && c.RevisionEpoch != "0")
				if c.Sensitivity >= 3 || (classified && !c.ModelVisible) {
					continue
				}
			}
			if block.Type != "text" || (block.Text != nil && strings.TrimSpace(*block.Text) != "") {
				return true
			}
		}
	}
	return false
}
