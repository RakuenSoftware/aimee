package economizer

import (
	"encoding/binary"
	"path/filepath"
	"strings"

	"github.com/JBailes/aimee/server-go/bus"
)

const (
	auxWireVersion   uint16 = 1
	compactMagic     uint32 = 0x504d434a // "JCMP" little-endian
	recallMagic      uint32 = 0x4c435254 // "TRCL" little-endian
	statsMagic       uint32 = 0x41545354 // "TSTA" little-endian
	auxHeaderLen            = 12
	recallRequestLen        = 16
	statsResponseLen        = 72
	maxSpillDirLen          = 4096
	maxSpillRefLen          = 64
)

const (
	recallOK uint16 = iota
	recallInvalidRef
	recallExpired
)

func auxResponse(magic uint32, result uint16, payload []byte) []byte {
	out := make([]byte, auxHeaderLen+len(payload))
	binary.LittleEndian.PutUint32(out[0:4], magic)
	binary.LittleEndian.PutUint16(out[4:6], auxWireVersion)
	binary.LittleEndian.PutUint16(out[6:8], result)
	binary.LittleEndian.PutUint32(out[8:12], uint32(len(payload)))
	copy(out[auxHeaderLen:], payload)
	return out
}

func handleJSONCompact(invocation bus.ModuleInvocation, request []byte) ([]byte, bus.ModuleStatus) {
	if len(request) > JSONMaxInput {
		return auxResponse(compactMagic, uint16(JSONTooLarge), nil), bus.ModuleStatusOK
	}
	out, result := JSONCompact(request)
	if invocation.Cancelled() {
		return nil, bus.ModuleStatusCancelled
	}
	return auxResponse(compactMagic, uint16(result), out), bus.ModuleStatusOK
}

// Tool-recall requests are binary so paths and recalled output remain byte
// preserving. Layout: magic, version, reserved, dir length, ref length, bytes.
func handleToolRecall(invocation bus.ModuleInvocation, request []byte) ([]byte, bus.ModuleStatus) {
	if len(request) < recallRequestLen || binary.LittleEndian.Uint32(request[0:4]) != recallMagic ||
		binary.LittleEndian.Uint16(request[4:6]) != auxWireVersion {
		return nil, bus.ModuleStatusInvalidRequest
	}
	dirLen := int(binary.LittleEndian.Uint32(request[8:12]))
	refLen := int(binary.LittleEndian.Uint32(request[12:16]))
	if dirLen <= 0 || dirLen > maxSpillDirLen || refLen <= 0 || refLen > maxSpillRefLen ||
		dirLen > len(request)-recallRequestLen || refLen != len(request)-recallRequestLen-dirLen {
		return nil, bus.ModuleStatusInvalidRequest
	}
	dir := string(request[recallRequestLen : recallRequestLen+dirLen])
	ref := string(request[recallRequestLen+dirLen:])
	if !filepath.IsAbs(dir) || filepath.Clean(dir) != dir || strings.IndexByte(dir, 0) >= 0 ||
		strings.IndexByte(ref, 0) >= 0 {
		return nil, bus.ModuleStatusInvalidRequest
	}
	if !TCRefValid(ref) {
		return auxResponse(recallMagic, recallInvalidRef, nil), bus.ModuleStatusOK
	}
	out, err := TCRecall(dir, ref)
	if err != nil {
		return auxResponse(recallMagic, recallExpired, nil), bus.ModuleStatusOK
	}
	if invocation.Cancelled() {
		return nil, bus.ModuleStatusCancelled
	}
	return auxResponse(recallMagic, recallOK, []byte(out)), bus.ModuleStatusOK
}

func handleToolStats(invocation bus.ModuleInvocation, request []byte) ([]byte, bus.ModuleStatus) {
	if len(request) != 0 {
		return nil, bus.ModuleStatusInvalidRequest
	}
	totals := TCStatsSnapshot()
	values := [...]int64{
		totals.Recognized, totals.Applied, totals.AppliedRaw, totals.AppliedFinal,
		totals.FamilyTest, totals.FamilyDiag, totals.Recovered, totals.RecoveredBytes,
	}
	out := make([]byte, statsResponseLen)
	binary.LittleEndian.PutUint32(out[0:4], statsMagic)
	binary.LittleEndian.PutUint16(out[4:6], auxWireVersion)
	for i, value := range values {
		binary.LittleEndian.PutUint64(out[8+i*8:16+i*8], uint64(value))
	}
	if invocation.Cancelled() {
		return nil, bus.ModuleStatusCancelled
	}
	return out, bus.ModuleStatusOK
}
