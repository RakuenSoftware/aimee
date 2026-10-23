package panel

import canonical "github.com/JBailes/aimee/server-go/ir"

// Scripted seats declare application text; production seats require admitted IR.
func testCanonicalSeat(r SeatResult) SeatResult {
	r.Output = canonical.TextResponse(r.Response)
	return r
}
