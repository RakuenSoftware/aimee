package ir

import (
	"encoding/json"
	"errors"
)

// RepairRequest continues the owned canonical task and completed assistant
// content. The response's native sidecar is output replay metadata, not a
// request message. Typed blocks (including opaque inputs and reasoning) retain
// their exact values; repair policy is a separate host block.
func RepairRequest(input *Request, output *Response, instruction string) (*Request, error) {
	if input == nil || !input.Valid() || output == nil || instruction == "" {
		return nil, errors.New("repair requires canonical request, response and policy")
	}
	if _, err := output.SemanticText(); err != nil {
		return nil, err
	}
	raw, err := json.Marshal(input)
	if err != nil {
		return nil, err
	}
	request, err := DecodeRequest(raw)
	if err != nil {
		return nil, err
	}
	response, err := output.Clone()
	if err != nil {
		return nil, err
	}
	blocks := make([]Block, len(response.Blocks))
	for i, block := range response.Blocks {
		blocks[i] = block.Block
	}
	request.Messages = append(request.Messages, Message{Role: "assistant", Blocks: blocks},
		Message{Role: "user", Blocks: []Block{{Type: "text", Text: &instruction,
			Context: &Context{Origin: 1, Authority: 3, Trust: 4, Sensitivity: 1, ModelVisible: true}}}})
	if !request.Valid() {
		return nil, errors.New("invalid canonical repair request")
	}
	return request, nil
}
