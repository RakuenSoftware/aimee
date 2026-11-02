package delegate

import canonical "github.com/JBailes/aimee/server-go/ir"

func testCanonicalInvocation(r InvocationResult) InvocationResult {
	r.History = []canonical.Message{{Role: "assistant", Blocks: []canonical.Block{r.Output.Blocks[0].Block}}}
	return r
}
