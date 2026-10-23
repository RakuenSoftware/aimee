package delegate

import canonical "github.com/JBailes/aimee/server-go/ir"

func testCanonicalInvocation(r InvocationResult) InvocationResult {
	r.Output = canonical.TextResponse(r.Response)
	r.History = []canonical.Message{{Role: "assistant", Blocks: []canonical.Block{r.Output.Blocks[0].Block}}}
	return r
}
