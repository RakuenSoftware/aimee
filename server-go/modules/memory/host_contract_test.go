package memory

import (
	"os"
	"reflect"
	"regexp"
	"strconv"
	"strings"
	"testing"
)

// C hosts retain their own wire identifiers. They must agree with the Go owner
// without importing or compiling any native code into the memory process.
func TestNativeHostStageIdentifiersMatchGo(t *testing.T) {
	text, err := os.ReadFile("../../../src/headers/memory_stage_contract.h")
	if err != nil {
		t.Fatal(err)
	}
	got := map[string]uint32{}
	for _, match := range regexp.MustCompile(`(?m)^#define\s+AIMEE_MEMORY_((?:EVENT|STAGE)_\w+)\s+(\d+)u$`).FindAllStringSubmatch(string(text), -1) {
		value, err := strconv.ParseUint(match[2], 10, 32)
		if err != nil {
			t.Fatal(err)
		}
		got[match[1]] = uint32(value)
	}
	want := map[string]uint32{
		"EVENT_EXTRACT_INDEX": EventExtractIndex, "STAGE_EXTRACT_INDEX": StageExtractIndex,
		"EVENT_WRITE": EventWrite, "STAGE_WRITE": StageWrite,
		"EVENT_EMBED": EventEmbed, "STAGE_EMBED": StageEmbed,
		"EVENT_RETRIEVE": EventRetrieve, "STAGE_RETRIEVE": StageRetrieve,
		"EVENT_RERANK": EventRerank, "STAGE_RERANK": StageRerank,
		"EVENT_DECLARE_COMMANDS": EventDeclareCommands, "STAGE_DECLARE_COMMANDS": StageDeclareCommands,
		"EVENT_DATA": EventData, "STAGE_DATA": StageData,
		"EVENT_COMMAND": EventCommand, "STAGE_COMMAND": StageCommand,
	}
	if !reflect.DeepEqual(got, want) {
		t.Fatalf("host IDs %v != Go IDs %v", got, want)
	}
}

func TestNativeStorageGraphCodesMatchGo(t *testing.T) {
	text, err := os.ReadFile("../../../src/modules/db2/c/graph_kinds.h")
	if err != nil {
		t.Fatal(err)
	}
	for prefix, names := range map[string]map[int]string{"NODE": ontologyNodeNames, "REL": ontologyRelationNames} {
		got := map[int]string{}
		pattern := regexp.MustCompile(`(?m)^\s+` + prefix + `_(\w+)\s*=\s*(\d+)`)
		for _, match := range pattern.FindAllStringSubmatch(string(text), -1) {
			value, _ := strconv.Atoi(match[2])
			got[value] = strings.ToLower(match[1])
		}
		want := map[int]string{99: "other"}
		for value, name := range names {
			want[value] = name
		}
		if !reflect.DeepEqual(got, want) {
			t.Fatalf("%s storage codes %v != Go codes %v", prefix, got, want)
		}
	}
}
