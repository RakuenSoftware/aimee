// Package tools implements the tool-dispatch process wire contract.
package tools

import (
	"encoding/binary"
	"strings"

	"github.com/JBailes/aimee/server-go/bus"
)

const (
	EventKind     uint32 = 6913
	StageDispatch uint32 = 1
	requestMagic  uint32 = 0x53494454
	responseMagic uint32 = 0x534c4354
	wireVersion   byte   = 1
	requestLen           = 104
	responseLen          = 8
	nameMax              = 95
)

const (
	ClassUnknown uint32 = iota
	ClassRead
	ClassWrite
	ClassExec
	ClassControl
	ClassRemote
)

var (
	execNames = map[string]struct{}{
		"bash": {}, "execute_script": {}, "test": {}, "run_tests": {},
	}
	writeNames = map[string]struct{}{
		"write_file": {}, "edit_file": {}, "edit_symbol": {}, "create_note": {},
		"rules_propose": {}, "learning_propose": {}, "git_commit": {}, "git_push": {},
		"git_branch": {}, "git_pr": {},
	}
	controlNames = map[string]struct{}{
		"request_input": {}, "clarify_start": {}, "clarify_answer": {},
		"diagnose_start": {}, "diagnose_observe": {}, "diagnose_hypothesize": {},
		"diagnose_evidence": {},
	}
	readNames = map[string]struct{}{
		"read_file": {}, "list_files": {}, "grep": {}, "code_search": {},
		"find_symbol": {}, "read_symbol": {}, "search_memory": {}, "search_docs": {},
		"web_search": {}, "web_read": {}, "list_notes": {}, "search_notes": {},
		"git_log": {}, "git_diff": {}, "git_status": {},
	}
)

func classify(name string) uint32 {
	if strings.ContainsRune(name, ':') {
		return ClassRemote
	}
	if _, ok := execNames[name]; ok {
		return ClassExec
	}
	if _, ok := writeNames[name]; ok {
		return ClassWrite
	}
	if _, ok := controlNames[name]; ok {
		return ClassControl
	}
	if _, ok := readNames[name]; ok {
		return ClassRead
	}
	return ClassUnknown
}

// Handle classifies a tool name without dispatching the tool itself.
func Handle(invocation bus.ModuleInvocation, request []byte) ([]byte, bus.ModuleStatus) {
	if invocation.StageID != StageDispatch || len(request) != requestLen ||
		binary.LittleEndian.Uint32(request[0:4]) != requestMagic || request[4] != wireVersion ||
		request[5] != 0 || request[7] != 0 || request[6] == 0 || request[6] > nameMax {
		return nil, bus.ModuleStatusInvalidRequest
	}
	if invocation.Cancelled() {
		return nil, bus.ModuleStatusCancelled
	}
	name := string(request[8 : 8+int(request[6])])
	response := make([]byte, responseLen)
	binary.LittleEndian.PutUint32(response[0:4], responseMagic)
	binary.LittleEndian.PutUint32(response[4:8], classify(name))
	return response, bus.ModuleStatusOK
}
