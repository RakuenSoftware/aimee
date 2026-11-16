package roundtable

import canonical "github.com/JBailes/aimee/server-go/ir"

// All public canonical input is caller data, including all-capability callers.
// Transport authorization never supplies platform provenance or revision proof.
func attenuateReviewRequest(request *canonical.Request) {
	attenuate := func(block *canonical.Block, system int) {
		context := block.Context
		if context == nil {
			context = &canonical.Context{ModelVisible: true}
			block.Context = context
		}
		if context.Origin == 0 && context.Authority == 0 && context.Trust == 0 && context.Sensitivity == 0 && context.RevisionDomain == "" && context.RevisionScope == "" && (context.RevisionEpoch == "" || context.RevisionEpoch == "0") {
			context.ModelVisible = true // preserve effective visibility of unclassified legacy IR
		}
		changed := context.Origin == 1 || context.Origin == 2 || context.Authority == 3 || context.Trust > 2
		if system == 1 && context.Authority != 1 && context.Origin != 7 && context.Origin != 5 && context.Origin != 4 && context.Origin != 6 {
			context.Origin = 2
			context.Authority = 3
			context.Trust = 2
			changed = true
		} else {
			if context.Origin == 1 || context.Origin == 2 {
				context.Origin = 3
			}
			if system != 0 {
				changed = changed || context.Authority != 1
				context.Authority = 1
			} else if context.Authority == 3 {
				context.Authority = 2
			}
			if context.Trust > 2 {
				context.Trust = 2
			}
		}
		if changed {
			context.RevisionDomain = ""
			context.RevisionScope = ""
			context.RevisionEpoch = "0"
		}
	}
	for i := range request.System {
		attenuate(&request.System[i], 1)
	}
	for i := range request.Messages {
		message := &request.Messages[i]
		system := 0
		if message.Role == "system" || message.Role == "developer" {
			system = 2
		}
		for j := range message.Blocks {
			attenuate(&message.Blocks[j], system)
		}
	}
}
