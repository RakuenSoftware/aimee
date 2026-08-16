package db3

import "encoding/binary"

const (
	MaxEncodedApply  = applyHeader + (MaxCollectionBytes - 1) + 4*MaxDimension
	ApplyChunkHeader = applyChunkHeader
)

type OperationSet uint32

const (
	OperationSearch OperationSet = 1 << iota
	OperationApply
	operationKnown = OperationSearch | OperationApply
)

type MetricSet uint32

const (
	MetricCosine MetricSet = 1 << iota
	MetricL2
	MetricDot
	metricKnown = MetricCosine | MetricL2 | MetricDot
)

type FilterSet uint32

const (
	FilterExact FilterSet = 1 << iota
	filterKnown           = FilterExact
)

type Capabilities struct {
	Generation   uint64
	Operations   OperationSet
	Metrics      MetricSet
	Filters      FilterSet
	MaxDimension uint32
	MaxBatch     uint32
	MaxTopK      uint32
	Ready        bool
}

func (c Capabilities) Validate() error {
	if c.Operations == 0 || c.Operations&^operationKnown != 0 || c.Metrics&^metricKnown != 0 ||
		c.Filters&^filterKnown != 0 || c.MaxDimension > MaxDimension || c.MaxTopK > MaxTopK {
		return ErrMalformed
	}
	if c.Ready && c.Generation == 0 {
		return ErrMalformed
	}
	if c.Operations&OperationSearch != 0 &&
		(c.Metrics == 0 || c.MaxDimension == 0 || c.MaxTopK == 0) {
		return ErrMalformed
	}
	if c.Operations&OperationApply != 0 && c.MaxBatch == 0 {
		return ErrMalformed
	}
	return nil
}

func EncodeCapabilities(c Capabilities) ([]byte, error) {
	if c.Validate() != nil {
		return nil, ErrMalformed
	}
	out := make([]byte, capabilitiesHeader)
	binary.LittleEndian.PutUint32(out[0:4], capabilitiesMagic)
	binary.LittleEndian.PutUint16(out[4:6], WireVersion)
	binary.LittleEndian.PutUint16(out[6:8], capabilitiesHeader)
	binary.LittleEndian.PutUint64(out[8:16], c.Generation)
	binary.LittleEndian.PutUint32(out[16:20], uint32(c.Operations))
	binary.LittleEndian.PutUint32(out[20:24], uint32(c.Metrics))
	binary.LittleEndian.PutUint32(out[24:28], uint32(c.Filters))
	if c.Ready {
		binary.LittleEndian.PutUint32(out[28:32], 1)
	}
	binary.LittleEndian.PutUint32(out[32:36], c.MaxDimension)
	binary.LittleEndian.PutUint32(out[36:40], c.MaxBatch)
	binary.LittleEndian.PutUint32(out[40:44], c.MaxTopK)
	return out, nil
}

func DecodeCapabilities(input []byte) (Capabilities, error) {
	if len(input) != capabilitiesHeader || binary.LittleEndian.Uint32(input[0:4]) != capabilitiesMagic ||
		binary.LittleEndian.Uint16(input[4:6]) != WireVersion ||
		binary.LittleEndian.Uint16(input[6:8]) != capabilitiesHeader ||
		binary.LittleEndian.Uint32(input[28:32]) > 1 || binary.LittleEndian.Uint32(input[44:48]) != 0 {
		return Capabilities{}, ErrMalformed
	}
	c := Capabilities{
		Generation:   binary.LittleEndian.Uint64(input[8:16]),
		Operations:   OperationSet(binary.LittleEndian.Uint32(input[16:20])),
		Metrics:      MetricSet(binary.LittleEndian.Uint32(input[20:24])),
		Filters:      FilterSet(binary.LittleEndian.Uint32(input[24:28])),
		Ready:        binary.LittleEndian.Uint32(input[28:32]) == 1,
		MaxDimension: binary.LittleEndian.Uint32(input[32:36]),
		MaxBatch:     binary.LittleEndian.Uint32(input[36:40]),
		MaxTopK:      binary.LittleEndian.Uint32(input[40:44]),
	}
	if c.Validate() != nil {
		return Capabilities{}, ErrMalformed
	}
	return c, nil
}

type AppliedResult uint32

const (
	AppliedOK AppliedResult = iota
	AppliedRetryable
	AppliedRejected
	AppliedInternal
)

type Applied struct {
	OperationID uint64
	Generation  uint64
	Watermark   uint64
	Result      AppliedResult
	Lag         uint32
}

func (a Applied) Validate() error {
	if a.OperationID == 0 || a.Generation == 0 || a.Result > AppliedInternal ||
		(a.Result == AppliedOK && a.Watermark < a.Generation) {
		return ErrMalformed
	}
	return nil
}

func EncodeApplied(a Applied) ([]byte, error) {
	if a.Validate() != nil {
		return nil, ErrMalformed
	}
	out := make([]byte, appliedHeader)
	binary.LittleEndian.PutUint32(out[0:4], appliedMagic)
	binary.LittleEndian.PutUint16(out[4:6], WireVersion)
	binary.LittleEndian.PutUint16(out[6:8], appliedHeader)
	binary.LittleEndian.PutUint64(out[8:16], a.OperationID)
	binary.LittleEndian.PutUint64(out[16:24], a.Generation)
	binary.LittleEndian.PutUint64(out[24:32], a.Watermark)
	binary.LittleEndian.PutUint32(out[32:36], uint32(a.Result))
	binary.LittleEndian.PutUint32(out[36:40], a.Lag)
	return out, nil
}

func DecodeApplied(input []byte) (Applied, error) {
	if len(input) != appliedHeader || binary.LittleEndian.Uint32(input[0:4]) != appliedMagic ||
		binary.LittleEndian.Uint16(input[4:6]) != WireVersion ||
		binary.LittleEndian.Uint16(input[6:8]) != appliedHeader {
		return Applied{}, ErrMalformed
	}
	a := Applied{
		OperationID: binary.LittleEndian.Uint64(input[8:16]),
		Generation:  binary.LittleEndian.Uint64(input[16:24]),
		Watermark:   binary.LittleEndian.Uint64(input[24:32]),
		Result:      AppliedResult(binary.LittleEndian.Uint32(input[32:36])),
		Lag:         binary.LittleEndian.Uint32(input[36:40]),
	}
	if a.Validate() != nil {
		return Applied{}, ErrMalformed
	}
	return a, nil
}

type SearchFailureCode uint32

const (
	SearchFailureInvalidRequest SearchFailureCode = iota + 1
	SearchFailureUnavailable
	SearchFailureRetryable
	SearchFailureInternal
)

type SearchFailure struct {
	RequestID uint64
	Code      SearchFailureCode
}

func (f SearchFailure) Validate() error {
	if f.RequestID == 0 || f.Code < SearchFailureInvalidRequest || f.Code > SearchFailureInternal {
		return ErrMalformed
	}
	return nil
}

func EncodeSearchFailure(f SearchFailure) ([]byte, error) {
	if f.Validate() != nil {
		return nil, ErrMalformed
	}
	out := make([]byte, searchFailureHeader)
	binary.LittleEndian.PutUint32(out[0:4], searchFailureMagic)
	binary.LittleEndian.PutUint16(out[4:6], WireVersion)
	binary.LittleEndian.PutUint16(out[6:8], searchFailureHeader)
	binary.LittleEndian.PutUint64(out[8:16], f.RequestID)
	binary.LittleEndian.PutUint32(out[16:20], uint32(f.Code))
	return out, nil
}

func DecodeSearchFailure(input []byte) (SearchFailure, error) {
	if len(input) != searchFailureHeader || binary.LittleEndian.Uint32(input[0:4]) != searchFailureMagic ||
		binary.LittleEndian.Uint16(input[4:6]) != WireVersion ||
		binary.LittleEndian.Uint16(input[6:8]) != searchFailureHeader ||
		binary.LittleEndian.Uint32(input[20:24]) != 0 {
		return SearchFailure{}, ErrMalformed
	}
	f := SearchFailure{RequestID: binary.LittleEndian.Uint64(input[8:16]), Code: SearchFailureCode(binary.LittleEndian.Uint32(input[16:20]))}
	if f.Validate() != nil {
		return SearchFailure{}, ErrMalformed
	}
	return f, nil
}

type RouteAction uint8

const (
	RouteQuery RouteAction = iota + 1
	RouteSelect
	RouteClear
)

type RouteRequest struct {
	RequestID            uint64
	Action               RouteAction
	Principal            uint32
	CapabilityGeneration uint64
	Fallback             bool
}

func (r RouteRequest) Validate() error {
	if r.RequestID == 0 || r.Action < RouteQuery || r.Action > RouteClear {
		return ErrMalformed
	}
	if r.Action == RouteSelect {
		if r.Principal == 0 || r.CapabilityGeneration == 0 {
			return ErrMalformed
		}
	} else if r.Principal != 0 || r.CapabilityGeneration != 0 || r.Fallback {
		return ErrMalformed
	}
	return nil
}

func EncodeRouteRequest(r RouteRequest) ([]byte, error) {
	if r.Validate() != nil {
		return nil, ErrMalformed
	}
	out := make([]byte, routeRequestHeader)
	binary.LittleEndian.PutUint32(out[0:4], routeRequestMagic)
	binary.LittleEndian.PutUint16(out[4:6], WireVersion)
	binary.LittleEndian.PutUint16(out[6:8], routeRequestHeader)
	binary.LittleEndian.PutUint64(out[8:16], r.RequestID)
	out[16] = byte(r.Action)
	if r.Fallback {
		out[17] = 1
	}
	binary.LittleEndian.PutUint32(out[20:24], r.Principal)
	binary.LittleEndian.PutUint64(out[24:32], r.CapabilityGeneration)
	return out, nil
}

func DecodeRouteRequest(input []byte) (RouteRequest, error) {
	if len(input) != routeRequestHeader || binary.LittleEndian.Uint32(input[0:4]) != routeRequestMagic ||
		binary.LittleEndian.Uint16(input[4:6]) != WireVersion ||
		binary.LittleEndian.Uint16(input[6:8]) != routeRequestHeader || input[17] > 1 ||
		binary.LittleEndian.Uint16(input[18:20]) != 0 || binary.LittleEndian.Uint64(input[32:40]) != 0 {
		return RouteRequest{}, ErrMalformed
	}
	r := RouteRequest{
		RequestID:            binary.LittleEndian.Uint64(input[8:16]),
		Action:               RouteAction(input[16]),
		Fallback:             input[17] == 1,
		Principal:            binary.LittleEndian.Uint32(input[20:24]),
		CapabilityGeneration: binary.LittleEndian.Uint64(input[24:32]),
	}
	if r.Validate() != nil {
		return RouteRequest{}, ErrMalformed
	}
	return r, nil
}

type RouteResult uint32

const (
	RouteOK RouteResult = iota
	RouteNotFound
	RouteNotReady
	RouteGenerationConflict
	RouteInvalid
)

type RouteReply struct {
	RequestID          uint64
	Result             RouteResult
	SelectedPrincipal  uint32
	ProviderGeneration uint64
	Fallback           bool
}

func (r RouteReply) Validate() error {
	if r.RequestID == 0 || r.Result > RouteInvalid ||
		(r.SelectedPrincipal == 0 && (r.ProviderGeneration != 0 || r.Fallback)) ||
		(r.SelectedPrincipal != 0 && r.ProviderGeneration == 0) {
		return ErrMalformed
	}
	return nil
}

func EncodeRouteReply(r RouteReply) ([]byte, error) {
	if r.Validate() != nil {
		return nil, ErrMalformed
	}
	out := make([]byte, routeReplyHeader)
	binary.LittleEndian.PutUint32(out[0:4], routeReplyMagic)
	binary.LittleEndian.PutUint16(out[4:6], WireVersion)
	binary.LittleEndian.PutUint16(out[6:8], routeReplyHeader)
	binary.LittleEndian.PutUint64(out[8:16], r.RequestID)
	binary.LittleEndian.PutUint32(out[16:20], uint32(r.Result))
	binary.LittleEndian.PutUint32(out[20:24], r.SelectedPrincipal)
	if r.Fallback {
		binary.LittleEndian.PutUint32(out[24:28], 1)
	}
	binary.LittleEndian.PutUint64(out[32:40], r.ProviderGeneration)
	return out, nil
}

func DecodeRouteReply(input []byte) (RouteReply, error) {
	if len(input) != routeReplyHeader || binary.LittleEndian.Uint32(input[0:4]) != routeReplyMagic ||
		binary.LittleEndian.Uint16(input[4:6]) != WireVersion ||
		binary.LittleEndian.Uint16(input[6:8]) != routeReplyHeader ||
		binary.LittleEndian.Uint32(input[24:28]) > 1 || binary.LittleEndian.Uint32(input[28:32]) != 0 {
		return RouteReply{}, ErrMalformed
	}
	r := RouteReply{
		RequestID:          binary.LittleEndian.Uint64(input[8:16]),
		Result:             RouteResult(binary.LittleEndian.Uint32(input[16:20])),
		SelectedPrincipal:  binary.LittleEndian.Uint32(input[20:24]),
		Fallback:           binary.LittleEndian.Uint32(input[24:28]) == 1,
		ProviderGeneration: binary.LittleEndian.Uint64(input[32:40]),
	}
	if r.Validate() != nil {
		return RouteReply{}, ErrMalformed
	}
	return r, nil
}

type ApplyChunk struct {
	OperationID uint64
	Total       uint32
	Offset      uint32
	Data        []byte
}

func (c ApplyChunk) Validate() error {
	end := uint64(c.Offset) + uint64(len(c.Data))
	if c.OperationID == 0 || c.Total == 0 || c.Total > MaxEncodedApply || len(c.Data) == 0 ||
		end > uint64(c.Total) || uint64(len(c.Data)) > uint64(^uint32(0)) {
		return ErrMalformed
	}
	return nil
}

func EncodeApplyChunk(c ApplyChunk) ([]byte, error) {
	if c.Validate() != nil {
		return nil, ErrMalformed
	}
	out := make([]byte, applyChunkHeader+len(c.Data))
	binary.LittleEndian.PutUint32(out[0:4], applyChunkMagic)
	binary.LittleEndian.PutUint16(out[4:6], WireVersion)
	binary.LittleEndian.PutUint16(out[6:8], applyChunkHeader)
	binary.LittleEndian.PutUint64(out[8:16], c.OperationID)
	binary.LittleEndian.PutUint32(out[16:20], c.Total)
	binary.LittleEndian.PutUint32(out[20:24], c.Offset)
	binary.LittleEndian.PutUint32(out[24:28], uint32(len(c.Data)))
	copy(out[applyChunkHeader:], c.Data)
	return out, nil
}

func DecodeApplyChunk(input []byte) (ApplyChunk, error) {
	if len(input) < applyChunkHeader || binary.LittleEndian.Uint32(input[0:4]) != applyChunkMagic ||
		binary.LittleEndian.Uint16(input[4:6]) != WireVersion ||
		binary.LittleEndian.Uint16(input[6:8]) != applyChunkHeader ||
		binary.LittleEndian.Uint32(input[28:32]) != 0 {
		return ApplyChunk{}, ErrMalformed
	}
	length := binary.LittleEndian.Uint32(input[24:28])
	if uint64(applyChunkHeader)+uint64(length) != uint64(len(input)) {
		return ApplyChunk{}, ErrMalformed
	}
	c := ApplyChunk{
		OperationID: binary.LittleEndian.Uint64(input[8:16]),
		Total:       binary.LittleEndian.Uint32(input[16:20]),
		Offset:      binary.LittleEndian.Uint32(input[20:24]),
		Data:        append([]byte(nil), input[applyChunkHeader:]...),
	}
	if c.Validate() != nil {
		return ApplyChunk{}, ErrMalformed
	}
	return c, nil
}
