package responsecomposition

import (
	"encoding/binary"
	"strings"
	"testing"

	"github.com/RakuenSoftware/ouroboros-ring/go/ring"
)

func compositionRequest(fields [fieldCount]string, stream bool) []byte {
	length := requestHeaderLen + 4*fieldCount
	for _, field := range fields {
		length += len(field)
	}
	request := make([]byte, length)
	binary.LittleEndian.PutUint32(request[0:4], requestMagic)
	request[4] = wireVersion
	request[6] = fieldCount
	if stream {
		binary.LittleEndian.PutUint32(request[8:12], 1)
	}
	offset := requestHeaderLen
	for _, field := range fields {
		binary.LittleEndian.PutUint32(request[offset:offset+4], uint32(len(field)))
		offset += 4
		copy(request[offset:], field)
		offset += len(field)
	}
	return request
}

func responseKey(t *testing.T, fields [fieldCount]string, stream bool) string {
	t.Helper()
	response, status := Handle(ring.ModuleInvocation{StageID: StageCompose},
		compositionRequest(fields, stream))
	if status != ring.ModuleStatusOK || len(response) < 4 ||
		binary.LittleEndian.Uint32(response[0:4]) != responseMagic {
		t.Fatalf("response = %x, status = %d", response, status)
	}
	return string(response[4:])
}

func TestCompositionKeyVectorAndIsolation(t *testing.T) {
	fields := [fieldCount]string{
		"uid:1", "openai-ingress", "openai", "gpt-4o", "/v1/chat/completions",
		"idem-a", "{\"x\":1}", "ctx", "cs0 rc0",
	}
	const want = "uid:1|45fd46a03cb4a28da3227155fec20a71"
	if got := responseKey(t, fields, false); got != want {
		t.Fatalf("key = %q, want %q", got, want)
	}
	changed := fields
	changed[6] = "{\"x\":2}"
	if responseKey(t, changed, false) == want || responseKey(t, fields, true) == want {
		t.Fatal("body or streaming mode did not isolate the key")
	}
	embeddedNUL := fields
	embeddedNUL[0] = "uid:1\x00ignored"
	if got := responseKey(t, embeddedNUL, false); got != want {
		t.Fatalf("C-string principal parity = %q, want %q", got, want)
	}
	// Every discriminator separates keys: a different principal must never read
	// another account's entry, and a request resolved to a different provider,
	// model or behaviour configuration must not collide with this one.
	for _, discriminator := range []struct {
		name  string
		field int
	}{{"principal", 0}, {"provider", 2}, {"model", 3}, {"context", 7}, {"behavior flags", 8}} {
		varied := fields
		varied[discriminator.field] += "-other"
		if responseKey(t, varied, false) == want {
			t.Fatalf("a different %s produced the same key", discriminator.name)
		}
	}
	if got := responseKey(t, fields, false); got != want {
		t.Fatalf("identical inputs gave %q, want %q", got, want)
	}
	// Long discriminators are digested to a fixed width, so the key still fits a
	// cache slot and an identical retry derives the identical key.
	long := fields
	long[0] = strings.Repeat("P", 99)
	long[3] = strings.Repeat("M", 199)
	long[5] = strings.Repeat("K", 299)
	first := responseKey(t, long, false)
	if len(first) >= 192 || responseKey(t, long, false) != first {
		t.Fatalf("long-input key = %q (len %d), not bounded and stable", first, len(first))
	}
	fields[0] = ""
	if got := responseKey(t, fields, false); !strings.HasPrefix(got, "anon|") {
		t.Fatalf("anonymous key = %q", got)
	}
}

func TestCompositionRejectsMalformedAndExpiredRequests(t *testing.T) {
	fields := [fieldCount]string{"uid:1", "source", "provider", "model", "endpoint"}
	request := compositionRequest(fields, false)
	request[6]--
	if _, status := Handle(ring.ModuleInvocation{StageID: StageCompose}, request); status != ring.ModuleStatusInvalidRequest {
		t.Fatalf("field-count status = %d", status)
	}
	request = compositionRequest(fields, false)
	request = append(request, 0)
	if _, status := Handle(ring.ModuleInvocation{StageID: StageCompose}, request); status != ring.ModuleStatusInvalidRequest {
		t.Fatalf("trailing-byte status = %d", status)
	}
	fields[0] = strings.Repeat("p", 129)
	if _, status := Handle(ring.ModuleInvocation{StageID: StageCompose},
		compositionRequest(fields, false)); status != ring.ModuleStatusInvalidRequest {
		t.Fatalf("principal-length status = %d", status)
	}
	fields[0] = "uid:1"
	if _, status := Handle(ring.ModuleInvocation{StageID: StageCompose, DeadlineNS: 1},
		compositionRequest(fields, false)); status != ring.ModuleStatusCancelled {
		t.Fatalf("expired invocation status = %d", status)
	}
}
