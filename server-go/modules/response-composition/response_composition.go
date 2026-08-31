// Package responsecomposition implements the response-composition process wire contract.
package responsecomposition

import (
	"bytes"
	"encoding/binary"
	"fmt"

	"github.com/JBailes/aimee/server-go/bus"
)

const (
	EventKind        uint32 = 7937
	StageCompose     uint32 = 1
	requestMagic     uint32 = 0x59454b52
	responseMagic    uint32 = 0x504d4f43
	wireVersion      byte   = 1
	fieldCount              = 9
	requestHeaderLen        = 12
	keyMax                  = 255
	fnvBasis         uint64 = 1469598103934665603
	fnvSecondBasis   uint64 = 0x84222325cbf29ce4
	fnvPrime         uint64 = 1099511628211
	fnvSecondSeed    uint64 = 0x9e3779b97f4a7c15
)

func fnv1a(value []byte) uint64 {
	hash := uint64(fnvBasis)
	for _, item := range value {
		hash ^= uint64(item)
		hash *= fnvPrime
	}
	return hash
}

func fnvField(hash uint64, seed uint64, value []byte) uint64 {
	hash ^= seed
	for _, item := range value {
		hash ^= uint64(item)
		hash *= fnvPrime
	}
	hash ^= 0x1e
	return hash * fnvPrime
}

func decodeRequest(request []byte) ([fieldCount][]byte, bool, bool) {
	var fields [fieldCount][]byte
	if len(request) < requestHeaderLen || binary.LittleEndian.Uint32(request[0:4]) != requestMagic ||
		request[4] != wireVersion || request[5] != 0 || request[6] != fieldCount ||
		request[7] != 0 {
		return fields, false, false
	}
	streamValue := binary.LittleEndian.Uint32(request[8:12])
	if streamValue > 1 {
		return fields, false, false
	}
	offset := requestHeaderLen
	for index := range fields {
		if len(request)-offset < 4 {
			return fields, false, false
		}
		length := binary.LittleEndian.Uint32(request[offset : offset+4])
		offset += 4
		if uint64(length) > uint64(len(request)-offset) {
			return fields, false, false
		}
		fields[index] = request[offset : offset+int(length)]
		offset += int(length)
	}
	if offset != len(request) || len(fields[0]) > 128 {
		return fields, false, false
	}
	return fields, streamValue != 0, true
}

// Handle creates the deterministic response deduplication key.
func Handle(invocation bus.ModuleInvocation, request []byte) ([]byte, bus.ModuleStatus) {
	if invocation.StageID != StageCompose {
		return nil, bus.ModuleStatusInvalidRequest
	}
	fields, stream, ok := decodeRequest(request)
	if !ok {
		return nil, bus.ModuleStatusInvalidRequest
	}
	if invocation.Cancelled() {
		return nil, bus.ModuleStatusCancelled
	}

	hashes := fmt.Sprintf("%016x%016x%016x", fnv1a(fields[6]), fnv1a(fields[7]), fnv1a(fields[8]))
	first, second := uint64(fnvBasis), uint64(fnvSecondBasis)
	for index := 1; index <= 4; index++ {
		first = fnvField(first, 0, fields[index])
		second = fnvField(second, fnvSecondSeed, fields[index])
	}
	streamByte := byte('0')
	if stream {
		streamByte = '1'
	}
	first = fnvField(first, 0, []byte{streamByte})
	second = fnvField(second, fnvSecondSeed, []byte{streamByte})
	first = fnvField(first, 0, fields[5])
	second = fnvField(second, fnvSecondSeed, fields[5])
	first = fnvField(first, 0, []byte(hashes))
	second = fnvField(second, fnvSecondSeed, []byte(hashes))

	principal := fields[0]
	if terminator := bytes.IndexByte(principal, 0); terminator >= 0 {
		principal = principal[:terminator]
	}
	if len(principal) == 0 {
		principal = []byte("anon")
	}
	key := fmt.Sprintf("%s|%016x%016x", principal, first, second)
	if len(key) > keyMax {
		return nil, bus.ModuleStatusInternal
	}
	response := make([]byte, 4+len(key))
	binary.LittleEndian.PutUint32(response[0:4], responseMagic)
	copy(response[4:], key)
	return response, bus.ModuleStatusOK
}
