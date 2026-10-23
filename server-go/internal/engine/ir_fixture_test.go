package engine

import canonical "github.com/JBailes/aimee/server-go/ir"

// Scripted agents declare a text transport at their fixture boundary.
func testCanonicalDelegate(r DelegateResult) DelegateResult {
	r.Output = canonical.TextResponse(r.Response)
	return r
}
func testCanonicalGroup(r DelegateGroupResult) DelegateGroupResult {
	r.Output = canonical.TextResponse(r.Response)
	return r
}
