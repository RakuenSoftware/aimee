package engine

import canonical "github.com/JBailes/aimee/server-go/ir"

// Workflow instructions and domain artifacts become distinct blocks where the
// runner creates a task. Neither labels nor artifact contents determine authority.
func workflowBlock(text string, origin, authority, trust int) canonical.Block {
	return canonical.Block{Type: "text", Text: &text, Context: &canonical.Context{
		Origin: origin, Authority: authority, Trust: trust, Sensitivity: 1, ModelVisible: true}}
}

func workflowHostTask(text string) canonical.Block     { return workflowBlock(text, 1, 2, 4) }
func workflowOperatorTask(text string) canonical.Block { return workflowBlock(text, 2, 2, 2) }

// Proposal is the immutable, admitted workflow-entry user request.
func workflowOriginalTask(text string) canonical.Block  { return workflowBlock(text, 3, 2, 2) }
func workflowArtifact(text string) canonical.Block      { return workflowBlock(text, 6, 1, 2) }
func workflowModelEvidence(text string) canonical.Block { return workflowBlock(text, 7, 1, 2) }
func workflowToolEvidence(text string) canonical.Block  { return workflowBlock(text, 4, 1, 2) }

func workflowRequest(persona string, blocks ...canonical.Block) *canonical.Request {
	request := canonical.TaskRequest(persona, "")
	request.Messages = []canonical.Message{{Role: "user", Blocks: append([]canonical.Block(nil), blocks...)}}
	return request
}

func appendWorkflowBlocks(request *canonical.Request, blocks ...canonical.Block) {
	message := &request.Messages[len(request.Messages)-1]
	message.ContentScalar = false
	message.Blocks = append(message.Blocks, blocks...)
}
