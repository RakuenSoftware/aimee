package postgres

import (
	"context"
	"encoding/binary"
	"errors"
	"math"

	"github.com/JBailes/aimee/server-go/bus"
	"github.com/JBailes/aimee/server-go/db3"
)

// The vector search stage: the whole of what this module does about vector
// databases, on the read side.
//
// A vector search arrives. If a vector database is provisioned and serves this
// collection, the search is handed to it. Otherwise it is answered here, by
// pgvectorscale. That is the entire rule, and everything below is the mechanics
// of saying it over the bus.

const (
	// EventVectorSearch is the event kind carrying a vector search.
	EventVectorSearch uint32 = 11267
	// StageVectorSearch is its stage id.
	StageVectorSearch uint32 = 3
)

// ErrVectorStage reports a request this stage cannot parse.
var ErrVectorStage = errors.New("postgres: malformed vector search request")

// The request body is this hop's own shape: a collection, the scope, and the
// vector. It is NOT a DB3 search request.
//
// DB3 is the postgres-to-provider hop. A caller asking postgres for a vector
// search is not addressing a provider and must not have to know one could
// exist -- it asks for rows, and whether those rows come from a vector database
// or from pgvectorscale is this module's decision, made after the request has
// already arrived. Encoding DB3 on the way IN would put that decision, and the
// vocabulary for it, in the caller.
//
// The collection is carried here because this hop needs it and the DB3 hop does
// not: a provider serves exactly one collection, so the question never has to be
// asked there. Here it decides which relation answers.
const (
	maxCollectionName = 32
	maxScopeBytes     = 64
	maxRecordType     = 32
	maxFilters        = 16
	maxLabelKey       = 32
	maxLabelValue     = 256
	maxDimension      = 4096
	maxTopK           = 256
)

// Every vector in this system has a workspace, a project, and a record type, so
// those are FIELDS rather than filters. A filter can be omitted; a field cannot,
// and a scope that went missing would widen the search to another workspace's
// rows. Filters carry whatever narrows further.

func putText(out []byte, text string) []byte {
	out = binary.LittleEndian.AppendUint16(out, uint16(len(text)))
	return append(out, text...)
}

func takeText(in []byte, limit int) (string, []byte, bool) {
	if len(in) < 2 {
		return "", nil, false
	}
	length := int(binary.LittleEndian.Uint16(in))
	if length > limit || len(in) < 2+length {
		return "", nil, false
	}
	return string(in[2 : 2+length]), in[2+length:], true
}

func encodeVectorSearch(collection string, request db3.SearchRequest) ([]byte, error) {
	if collection == "" || len(collection) > maxCollectionName ||
		len(request.Workspace) > maxScopeBytes || len(request.Project) > maxScopeBytes ||
		len(request.RecordType) > maxRecordType || len(request.Filters) > maxFilters ||
		len(request.Vector) == 0 || len(request.Vector) > maxDimension ||
		request.TopK == 0 || request.TopK > maxTopK {
		return nil, ErrVectorStage
	}
	body := make([]byte, 0, 64+len(request.Vector)*4)
	body = binary.LittleEndian.AppendUint64(body, request.RequestID)
	body = binary.LittleEndian.AppendUint64(body, request.RequiredGeneration)
	body = putText(body, collection)
	body = putText(body, request.Workspace)
	body = putText(body, request.Project)
	body = putText(body, request.RecordType)
	body = binary.LittleEndian.AppendUint32(body, request.TopK)
	body = binary.LittleEndian.AppendUint32(body, uint32(len(request.Vector)))
	for _, value := range request.Vector {
		body = binary.LittleEndian.AppendUint32(body, math.Float32bits(value))
	}
	body = binary.LittleEndian.AppendUint16(body, uint16(len(request.Filters)))
	for _, filter := range request.Filters {
		if filter.Key == "" || len(filter.Key) > maxLabelKey || len(filter.Value) > maxLabelValue {
			return nil, ErrVectorStage
		}
		body = putText(body, filter.Key)
		body = putText(body, filter.Value)
	}
	return body, nil
}

func decodeVectorSearch(body []byte) (string, db3.SearchRequest, error) {
	refuse := func() (string, db3.SearchRequest, error) {
		return "", db3.SearchRequest{}, ErrVectorStage
	}
	if len(body) < 16 {
		return refuse()
	}
	var request db3.SearchRequest
	request.RequestID = binary.LittleEndian.Uint64(body)
	request.RequiredGeneration = binary.LittleEndian.Uint64(body[8:])
	rest := body[16:]

	collection, rest, ok := takeText(rest, maxCollectionName)
	if !ok || collection == "" {
		return refuse()
	}
	if request.Workspace, rest, ok = takeText(rest, maxScopeBytes); !ok {
		return refuse()
	}
	if request.Project, rest, ok = takeText(rest, maxScopeBytes); !ok {
		return refuse()
	}
	if request.RecordType, rest, ok = takeText(rest, maxRecordType); !ok {
		return refuse()
	}
	if len(rest) < 8 {
		return refuse()
	}
	request.TopK = binary.LittleEndian.Uint32(rest)
	dimension := int(binary.LittleEndian.Uint32(rest[4:]))
	rest = rest[8:]
	if request.TopK == 0 || request.TopK > maxTopK ||
		dimension == 0 || dimension > maxDimension || len(rest) < dimension*4 {
		return refuse()
	}
	request.Vector = make([]float32, dimension)
	for index := range request.Vector {
		request.Vector[index] = math.Float32frombits(binary.LittleEndian.Uint32(rest[index*4:]))
		// A vector carrying NaN or an infinity has no nearest neighbour, and
		// every store answers it differently -- some return nothing, some
		// return everything. Refused here so the answer cannot depend on which
		// store was asked.
		if math.IsNaN(float64(request.Vector[index])) ||
			math.IsInf(float64(request.Vector[index]), 0) {
			return refuse()
		}
	}
	rest = rest[dimension*4:]

	if len(rest) < 2 {
		return refuse()
	}
	count := int(binary.LittleEndian.Uint16(rest))
	rest = rest[2:]
	if count > maxFilters {
		return refuse()
	}
	request.Filters = make([]db3.ExactLabel, 0, count)
	for range count {
		var key, value string
		if key, rest, ok = takeText(rest, maxLabelKey); !ok || key == "" {
			return refuse()
		}
		if value, rest, ok = takeText(rest, maxLabelValue); !ok {
			return refuse()
		}
		request.Filters = append(request.Filters, db3.ExactLabel{Key: key, Value: value})
	}
	if len(rest) != 0 {
		// Trailing bytes mean the caller and this decoder disagree about the
		// shape. Refused rather than ignored: the disagreement is the bug.
		return refuse()
	}
	return collection, request, nil
}

// The reply is this hop's shape too, for the reason the request is: a caller
// asked postgres for rows and gets back ids and scores. Whether a vector
// database produced them is not in the answer, because it is not the caller's
// question -- and an answer that carried the store's identity would invite a
// caller to start depending on it.
func encodeVectorReply(reply db3.SearchReply) ([]byte, error) {
	if len(reply.Candidates) > maxTopK {
		return nil, ErrVectorStage
	}
	body := make([]byte, 0, 18+len(reply.Candidates)*16)
	body = binary.LittleEndian.AppendUint64(body, reply.RequestID)
	body = binary.LittleEndian.AppendUint64(body, reply.Generation)
	body = binary.LittleEndian.AppendUint16(body, uint16(len(reply.Candidates)))
	for _, candidate := range reply.Candidates {
		body = binary.LittleEndian.AppendUint64(body, uint64(candidate.PointID))
		body = binary.LittleEndian.AppendUint64(body, math.Float64bits(candidate.Score))
	}
	return body, nil
}

func decodeVectorReply(body []byte) (db3.SearchReply, error) {
	if len(body) < 18 {
		return db3.SearchReply{}, ErrVectorStage
	}
	reply := db3.SearchReply{
		RequestID:  binary.LittleEndian.Uint64(body),
		Generation: binary.LittleEndian.Uint64(body[8:]),
	}
	count := int(binary.LittleEndian.Uint16(body[16:]))
	rest := body[18:]
	if count > maxTopK || len(rest) != count*16 {
		return db3.SearchReply{}, ErrVectorStage
	}
	reply.Candidates = make([]db3.Candidate, count)
	for index := range reply.Candidates {
		reply.Candidates[index] = db3.Candidate{
			PointID: int64(binary.LittleEndian.Uint64(rest[index*16:])),
			Score:   math.Float64frombits(binary.LittleEndian.Uint64(rest[index*16+8:])),
		}
	}
	return reply, nil
}

// VectorSearchHandler answers vector searches for the deployment as provisioned.
//
// Built once, at boot, from the grant. The routers are fixed for the life of
// the process: a collection either has a provider or it does not, and it cannot
// acquire one later, because grants load at start.
type VectorSearchHandler struct {
	routers map[string]*VectorRouter
}

// NewVectorSearchHandler builds the routers for every collection this
// deployment can answer.
//
// The provisioned provider gets its ONE collection. Every other collection gets
// a router with no provider, which answers in-database and always will. That is
// the rule as a data structure: hand off what can be handed off, do the rest
// locally, and decide which is which once rather than per request.
func NewVectorSearchHandler(provider VectorProvider, searcher ProviderSearcher) (*VectorSearchHandler, error) {
	routers := make(map[string]*VectorRouter, len(collectionTable))
	for collection := range collectionTable {
		fallback, err := NewPGVectorSearch(collection)
		if err != nil {
			return nil, err
		}
		principal := uint32(0)
		if collection == provider.Collection {
			principal = provider.Principal
		}
		router, err := NewVectorRouter(principal, searcher, fallback)
		if err != nil {
			return nil, err
		}
		routers[collection] = router
	}
	return &VectorSearchHandler{routers: routers}, nil
}

// Handle answers one vector search.
func (h *VectorSearchHandler) Handle(invocation bus.ModuleInvocation, frame []byte) ([]byte, bus.ModuleStatus) {
	if h == nil || invocation.StageID != StageVectorSearch {
		return nil, bus.ModuleStatusInvalidRequest
	}
	collection, request, err := decodeVectorSearch(frame)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	router := h.routers[collection]
	if router == nil {
		// An unknown collection is refused rather than guessed. The set is
		// closed because the alternative is a caller-supplied name reaching a
		// relation name.
		return nil, bus.ModuleStatusInvalidRequest
	}

	ctx, cancel := invocationContext(invocation)
	defer cancel()
	reply, _, err := router.Search(ctx, request)
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	encoded, err := encodeVectorReply(reply)
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	return encoded, bus.ModuleStatusOK
}

// invocationContext bounds the search by whatever deadline the caller sent.
//
// A caller that has stopped waiting must not keep PostgreSQL busy, and a search
// with no deadline must not run forever.
func invocationContext(invocation bus.ModuleInvocation) (context.Context, context.CancelFunc) {
	remaining := invocation.Remaining(vectorSearchDeadline)
	if remaining <= 0 {
		ctx, cancel := context.WithCancel(context.Background())
		cancel()
		return ctx, cancel
	}
	return context.WithTimeout(context.Background(), remaining)
}
