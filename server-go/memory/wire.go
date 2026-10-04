// Package memory is the implementation-independent caller contract for Aimee memory.
// It owns framing and wire types, never policy, storage, ranking or admission.
package memory

const (
	EventExtractIndex uint32 = 5889
	EventWrite        uint32 = 5890
	EventEmbed        uint32 = 5891
	EventRetrieve     uint32 = 5892
	EventRerank       uint32 = 5893

	StageExtractIndex uint32 = 1
	StageWrite        uint32 = 2
	StageEmbed        uint32 = 3
	StageRetrieve     uint32 = 4
	StageRerank       uint32 = 5

	RequestMagic  uint32 = 0x4b4e524d
	ResponseMagic uint32 = 0x464e434d
	WireVersion   uint32 = 1
	RequestLen           = 16
	ResponseLen          = 8

	GateRequestMagic  uint32 = 0x54524757
	GateResponseMagic uint32 = 0x56524757
	RelTypeMax               = 256
	GateRequestLen           = 20 + RelTypeMax
	GateResponseLen          = 8

	ExtractRequestMagic      uint32 = 0x51525458
	ExtractResponseMagic     uint32 = 0x53525458
	ExtractRequestHeaderLen         = 16
	ExtractResponseHeaderLen        = 8
	// Field capacities of one triple, mirroring pattern_triple_t's buffers. A
	// field is never emitted longer than these -- ExtractPatterns already trims
	// to them -- but the C decoder refuses an over-long field outright, so
	// emitting one would be a hard failure rather than a truncation.
	TripleSubjectMax = 128
	TripleRelTypeMax = 64
	TripleObjectMax  = 128

	PiiRequestMagic     uint32 = 0x51524950
	PiiResponseMagic    uint32 = 0x53524950
	PiiRequestHeaderLen        = 12
	PiiResponseLen             = 8

	ScanRequestMagic      uint32 = 0x51525452
	ScanResponseMagic     uint32 = 0x53525452
	ScanRequestHeaderLen         = 12
	ScanResponseHeaderLen        = 16

	SensRequestMagic      uint32 = 0x51525350
	SensResponseMagic     uint32 = 0x53525350
	SensRequestHeaderLen         = 12
	SensResponseHeaderLen        = 8
)

const (
	ConfidenceLow uint32 = iota + 1
	ConfidenceMedium
	ConfidenceHigh
)

const (
	EventDeclareCommands  uint32 = 5894
	StageDeclareCommands  uint32 = 6
	EventData             uint32 = 5895
	StageData             uint32 = 7
	EventCommand          uint32 = 5896
	StageCommand          uint32 = 8
	CommandsRequestMagic  uint32 = 0x444d4344
	CommandsResponseMagic uint32 = 0x524d4344
	CommandsRequestLen           = 8
	AttrMax                      = 128
	SurfaceCLI            uint32 = 1 << 0
	SurfaceRPC            uint32 = 1 << 1
	SurfaceMCP            uint32 = 1 << 2
	SurfaceACP            uint32 = 1 << 3
	MCPProminent          uint32 = 0
	MCPDiscoverable       uint32 = 1
)
