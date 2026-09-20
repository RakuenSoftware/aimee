package memory

import (
	"encoding/json"
	"math"

	"github.com/JBailes/aimee/server-go/bus"
)

func handleCorrectionProposalCommand(options handlerOptions, invocation bus.ModuleInvocation, verb string, args commandArgs) ([]byte, bus.ModuleStatus) {
	request := DataRequest{Operation: "correction-proposals", IncludeAll: true, Limit: 50}
	if verb == "review_correction" {
		if !verifiedRetryCaller(options.commandContext) || !options.commandContext.UserAuthority {
			return commandResult(commandError("forbidden", "correction review requires authenticated user authority"))
		}
		review := &correctionReviewRequest{ProposalID: args.stringOr("proposal_id", ""), Digest: args.stringOr("payload_digest", ""), Action: args.stringOr("action", "")}
		if json.Unmarshal(args["expected_version"], &review.Expected) != nil || !review.valid() {
			return commandResult(commandError("invalid_argument", "review requires proposal_id, payload_digest, approve/reject action and exact expected_version"))
		}
		request.Operation, request.Authority, request.CorrectionReview = "correction-review", AuthorityUser, review
	} else {
		request.ProposalID = args.stringOr("proposal_id", "")
		_, supplied := args["proposal_id"]
		_, isString := args.stringValue("proposal_id")
		if supplied && (!isString || !validProposalID(request.ProposalID)) {
			return commandResult(commandError("invalid_argument", "invalid proposal_id"))
		}
		if _, exists := args["limit"]; exists {
			n, ok := args.number("limit")
			if !ok || math.Trunc(n) != n || n < 1 || n > 100 {
				return commandResult(commandError("invalid_argument", "limit must be from 1 to 100"))
			}
			request.Limit = int(n)
		}
	}
	commandScope(args, &request)
	raw, err := json.Marshal(request)
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	raw, status := handleData(options, invocation, raw)
	if status != bus.ModuleStatusOK {
		return commandResult(commandError("unavailable", "memory correction review unavailable"))
	}
	var response DataResponse
	if json.Unmarshal(raw, &response) != nil {
		return nil, bus.ModuleStatusInternal
	}
	if refusal := commandMutationRefusal(response.Code, response.Proposal); refusal != nil {
		return commandResult(refusal)
	}
	if len(response.Payload) == 0 {
		return nil, bus.ModuleStatusInternal
	}
	return commandResult(response.Payload)
}
