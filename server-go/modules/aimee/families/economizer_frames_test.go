package families

import (
	"encoding/binary"
	"testing"
)

// encodeFrame builds a db1-keyed-blob-v1 request the way the shipped client
// does. It is spelled out here rather than reused from the client package so a
// change to one side cannot silently move both: the round-trip test drives the
// real client, and these frames are the independent check on that wire.
func encodeFrame(op uint32, key, payload string) []byte {
	frame := make([]byte, 0, 12+len(key)+len(payload))
	var scratch [4]byte
	put := func(v uint32) {
		binary.LittleEndian.PutUint32(scratch[:], v)
		frame = append(frame, scratch[:]...)
	}
	put(op)
	put(uint32(len(key)))
	frame = append(frame, key...)
	put(uint32(len(payload)))
	frame = append(frame, payload...)
	return frame
}

func decodeStatus(t *testing.T, response []byte) uint32 {
	t.Helper()
	if len(response) < 8 {
		t.Fatalf("response is %d bytes, too short to carry a status", len(response))
	}
	length := binary.LittleEndian.Uint32(response[4:8])
	if uint64(length) != uint64(len(response)-8) {
		t.Fatalf("response declares %d payload bytes but carries %d", length, len(response)-8)
	}
	return binary.LittleEndian.Uint32(response[0:4])
}

// malformedFrame produces frames whose declared lengths disagree with what is
// actually present. Each one would be an out-of-range slice in a decoder that
// trusted the header.
func malformedFrame(kind string) []byte {
	switch kind {
	case "short":
		return []byte{1, 0, 0, 0, 0, 0}
	case "key-length-lies":
		frame := encodeFrame(opStateLoad, "conv-1", "")
		binary.LittleEndian.PutUint32(frame[4:8], 4096)
		return frame
	case "payload-length-lies":
		frame := encodeFrame(opStateSave, "conv-1", "{}")
		binary.LittleEndian.PutUint32(frame[8+6:8+6+4], 4096)
		return frame
	case "trailing-bytes":
		return append(encodeFrame(opStateLoad, "conv-1", ""), 0xff)
	default:
		panic("unknown malformed frame kind " + kind)
	}
}
