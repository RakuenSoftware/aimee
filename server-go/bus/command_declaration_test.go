package bus

import (
	"encoding/binary"
	"testing"
)

func TestCommandDeclarationWire(t *testing.T) {
	request := []byte{'D', 'C', 'M', 'D', 2, 0, 0, 0}
	definitions := []CommandDefinition{{Group: "memory", Verb: "stats", Summary: "Statistics", Surfaces: 2, Visibility: 1}}
	frame, err := EncodeCommandDeclaration(request, 8, definitions)
	if err != nil {
		t.Fatal(err)
	}
	if string(frame[:4]) != "DCMR" || binary.LittleEndian.Uint32(frame[4:]) != 2 || binary.LittleEndian.Uint32(frame[8:]) != 1 || binary.LittleEndian.Uint32(frame[12:]) != 8 {
		t.Fatalf("header=%x", frame)
	}
	if binary.LittleEndian.Uint32(frame[16:]) != 2 || binary.LittleEndian.Uint32(frame[20:]) != 1 || binary.LittleEndian.Uint16(frame[24:]) != 6 || binary.LittleEndian.Uint16(frame[26:]) != 5 || binary.LittleEndian.Uint16(frame[28:]) != 10 || binary.LittleEndian.Uint16(frame[30:]) != 0 || string(frame[32:]) != "memorystatsStatistics" {
		t.Fatalf("record=%x", frame)
	}
	for _, bad := range []CommandDefinition{
		{Group: "Memory", Verb: "get", Surfaces: 2},
		{Group: "memory", Verb: "with.dot", Surfaces: 2},
		{Group: "memory", Verb: "get", Surfaces: 4},
		{Group: "memory", Verb: "get", Surfaces: 16},
		{Group: "memory", Verb: "get", Surfaces: 2, Visibility: 2},
	} {
		if _, err := EncodeCommandDeclaration(request, 8, []CommandDefinition{bad}); err == nil {
			t.Fatal(bad)
		}
	}
	if _, err := EncodeCommandDeclaration(request, 8, append(definitions, definitions[0])); err == nil {
		t.Fatal("accepted duplicate command")
	}
	for _, stage := range []uint32{0, 255, 256} {
		if _, err := EncodeCommandDeclaration(request, stage, nil); err == nil {
			t.Fatal("accepted stage", stage)
		}
	}
	for _, bad := range [][]byte{nil, request[:4], append(append([]byte{}, request...), 0), {'D', 'C', 'M', 'D', 1, 0, 0, 0}} {
		if _, err := EncodeCommandDeclaration(bad, 8, definitions); err == nil {
			t.Fatal("accepted malformed request")
		}
	}
}

func TestInternalCommandDeclaration(t *testing.T) {
	request := []byte{'D', 'C', 'M', 'D', 2, 0, 0, 0}
	frame, err := EncodeCommandDeclaration(request, 8, []CommandDefinition{{Group: "memory", Verb: "embed"}})
	if err != nil || binary.LittleEndian.Uint32(frame[16:]) != 0 {
		t.Fatalf("internal declaration: %x %v", frame, err)
	}
}
