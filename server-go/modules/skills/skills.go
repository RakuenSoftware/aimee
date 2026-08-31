// Package skills implements the bounded skills-process wire contracts.
package skills

import (
	"bytes"
	"encoding/binary"

	"github.com/JBailes/aimee/server-go/bus"
)

const (
	EventKind     uint32 = 7681
	StageContext  uint32 = 1
	EventTrigger  uint32 = 7682
	StageTrigger  uint32 = 2
	requestMagic  uint32 = 0x58435453
	responseMagic uint32 = 0x57454956
	wireVersion   uint32 = 1
	requestLen           = 16
	responseLen          = 8

	triggerRequestMagic  uint32 = 0x51544b53
	triggerResponseMagic uint32 = 0x52544b53
	triggerHeaderLen            = 20
	triggerContentMax           = 100 * 1024
	triggerToolMax              = 255
	triggerSubjectMax           = 1024 * 1024
	triggerResponseLen          = 12
)

func handleContext(request []byte) ([]byte, bus.ModuleStatus) {
	if len(request) != requestLen ||
		binary.LittleEndian.Uint32(request[0:4]) != requestMagic ||
		binary.LittleEndian.Uint32(request[4:8]) != wireVersion {
		return nil, bus.ModuleStatusInvalidRequest
	}
	count := int32(binary.LittleEndian.Uint32(request[8:12]))
	interval := int32(binary.LittleEndian.Uint32(request[12:16]))
	var fire uint32
	if interval > 0 && count > 0 && count%interval == 0 {
		fire = 1
	}
	response := make([]byte, responseLen)
	binary.LittleEndian.PutUint32(response[0:4], responseMagic)
	binary.LittleEndian.PutUint32(response[4:8], fire)
	return response, bus.ModuleStatusOK
}

func triggerTokenByte(value byte) bool {
	return value >= 'a' && value <= 'z' || value >= 'A' && value <= 'Z' ||
		value >= '0' && value <= '9' || value == '_' || value == '-' || value == '.'
}

func triggerValueContainsToken(value []byte, needle string) bool {
	if needle == "" {
		return false
	}
	for pos := 0; pos < len(value); {
		for pos < len(value) && !triggerTokenByte(value[pos]) {
			pos++
		}
		start := pos
		for pos < len(value) && triggerTokenByte(value[pos]) {
			pos++
		}
		if string(value[start:pos]) == needle {
			return true
		}
	}
	return false
}

func triggerPatternsMatch(value []byte, subject string) bool {
	sawQuoted := false
	for pos := 0; pos < len(value); {
		for pos < len(value) && value[pos] != '\'' && value[pos] != '"' {
			pos++
		}
		if pos == len(value) {
			break
		}
		quote := value[pos]
		pos++
		start := pos
		for pos < len(value) && value[pos] != quote {
			pos++
		}
		if pos > start {
			sawQuoted = true
			end := pos
			if end-start > 127 {
				end = start + 127
			}
			if bytes.Contains([]byte(subject), value[start:end]) {
				return true
			}
		}
		if pos < len(value) {
			pos++
		}
	}
	if sawQuoted {
		return false
	}
	return triggerValueContainsToken(value, subject)
}

func triggerLineValue(line []byte, key string) ([]byte, bool) {
	line = bytes.TrimSpace(line)
	prefix := []byte(key + ":")
	if !bytes.HasPrefix(line, prefix) {
		return nil, false
	}
	return bytes.TrimSpace(line[len(prefix):]), true
}

func triggerMatches(content, toolName, subject string) bool {
	if toolName == "" || len(content) < 4 || content[:3] != "---" ||
		(content[3] != '\n' && content[3] != '\r') {
		return false
	}
	headerStart := bytes.IndexByte([]byte(content), '\n')
	if headerStart < 0 {
		return false
	}
	headerTail := []byte(content[headerStart+1:])
	headerEnd := bytes.Index(headerTail, []byte("\n---"))
	if headerEnd < 0 {
		return false
	}

	inTriggers, sawTriggers := false, false
	sawTool, toolMatch := false, false
	sawPattern, patternMatch := false, false
	for _, line := range bytes.Split(headerTail[:headerEnd], []byte{'\n'}) {
		if len(line) == 0 {
			continue
		}
		if line[0] != ' ' && line[0] != '\t' && line[0] != '\r' && line[0] != '\n' && line[0] != '\v' && line[0] != '\f' {
			if bytes.HasPrefix(line, []byte("triggers:")) {
				inTriggers, sawTriggers = true, true
			} else if inTriggers {
				break
			}
			continue
		}
		if !inTriggers {
			continue
		}
		if value, ok := triggerLineValue(line, "tool"); ok {
			sawTool = true
			toolMatch = toolMatch || triggerValueContainsToken(value, toolName)
		} else if value, ok := triggerLineValue(line, "arg_pattern"); ok {
			sawPattern = true
			patternMatch = patternMatch || triggerPatternsMatch(value, subject)
		} else if value, ok := triggerLineValue(line, "path_pattern"); ok {
			sawPattern = true
			patternMatch = patternMatch || triggerPatternsMatch(value, subject)
		}
	}
	return sawTriggers && (!sawTool || toolMatch) && (!sawPattern || patternMatch)
}

func handleTrigger(request []byte) ([]byte, bus.ModuleStatus) {
	if len(request) < triggerHeaderLen || binary.LittleEndian.Uint32(request[0:4]) != triggerRequestMagic ||
		binary.LittleEndian.Uint32(request[4:8]) != wireVersion {
		return nil, bus.ModuleStatusInvalidRequest
	}
	contentLen := uint64(binary.LittleEndian.Uint32(request[8:12]))
	toolLen := uint64(binary.LittleEndian.Uint32(request[12:16]))
	subjectLen := uint64(binary.LittleEndian.Uint32(request[16:20]))
	total := uint64(triggerHeaderLen) + contentLen + toolLen + subjectLen
	if contentLen > triggerContentMax || toolLen == 0 || toolLen > triggerToolMax ||
		subjectLen > triggerSubjectMax || total != uint64(len(request)) {
		return nil, bus.ModuleStatusInvalidRequest
	}
	contentEnd := uint64(triggerHeaderLen) + contentLen
	toolEnd := contentEnd + toolLen
	if bytes.IndexByte(request[triggerHeaderLen:total], 0) >= 0 {
		return nil, bus.ModuleStatusInvalidRequest
	}
	match := triggerMatches(string(request[triggerHeaderLen:contentEnd]),
		string(request[contentEnd:toolEnd]), string(request[toolEnd:total]))
	response := make([]byte, triggerResponseLen)
	binary.LittleEndian.PutUint32(response[0:4], triggerResponseMagic)
	binary.LittleEndian.PutUint32(response[4:8], wireVersion)
	if match {
		binary.LittleEndian.PutUint32(response[8:12], 1)
	}
	return response, bus.ModuleStatusOK
}

// Handle serves the bounded review-nudge and trigger-match decisions.
func Handle(invocation bus.ModuleInvocation, request []byte) ([]byte, bus.ModuleStatus) {
	if invocation.StageID != StageContext && invocation.StageID != StageTrigger {
		return nil, bus.ModuleStatusInvalidRequest
	}
	if invocation.Cancelled() {
		return nil, bus.ModuleStatusCancelled
	}
	if invocation.StageID == StageContext {
		return handleContext(request)
	}
	return handleTrigger(request)
}
