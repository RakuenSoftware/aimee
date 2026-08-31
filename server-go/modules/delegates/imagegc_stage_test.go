package delegates

import (
	"encoding/binary"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
)

func imageGCRequest(images []SandboxImage, now int64, keepMin int, maxAge int64) []byte {
	out := make([]byte, imageGCReqHeaderLen)
	binary.LittleEndian.PutUint32(out[0:4], imageGCRequestMagic)
	out[4] = wireVersion
	binary.LittleEndian.PutUint32(out[8:12], uint32(len(images)))
	binary.LittleEndian.PutUint32(out[12:16], uint32(int32(keepMin)))
	binary.LittleEndian.PutUint64(out[16:24], uint64(now))
	binary.LittleEndian.PutUint64(out[24:32], uint64(maxAge))
	for _, img := range images {
		var hdr [12]byte
		if img.InUse {
			binary.LittleEndian.PutUint32(hdr[0:4], 1)
		}
		binary.LittleEndian.PutUint32(hdr[4:8], uint32(len(img.Tag)))
		binary.LittleEndian.PutUint32(hdr[8:12], uint32(len(img.CreatedAt)))
		out = append(out, hdr[:]...)
		out = append(out, img.Tag...)
		out = append(out, img.CreatedAt...)
	}
	return out
}

func callImageGC(t *testing.T, images []SandboxImage, now int64, keepMin int,
	maxAge int64) []SandboxImageVerdict {
	t.Helper()
	response, status := Handle(bus.ModuleInvocation{StageID: StageImageGC},
		imageGCRequest(images, now, keepMin, maxAge))
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v, want OK", status)
	}
	if len(response) < 8 || binary.LittleEndian.Uint32(response[0:4]) != imageGCResponseMagic {
		t.Fatal("bad response header")
	}
	count := int(binary.LittleEndian.Uint32(response[4:8]))
	out := make([]SandboxImageVerdict, 0, count)
	at := 8
	for i := 0; i < count; i++ {
		remove := binary.LittleEndian.Uint32(response[at:at+4]) == 1
		n := int(binary.LittleEndian.Uint32(response[at+4 : at+8]))
		at += 8
		out = append(out, SandboxImageVerdict{Remove: remove, Reason: string(response[at : at+n])})
		at += n
	}
	if at != len(response) {
		t.Fatalf("decoded %d of %d bytes", at, len(response))
	}
	return out
}

func TestImageGCStageReturnsAVerdictPerImageInOrder(t *testing.T) {
	now := int64(1784118896)
	images := []SandboxImage{
		{Tag: "newest", CreatedAt: "2026-07-15 12:34:56 +0000 UTC"},
		{Tag: "old", CreatedAt: "2026-06-15 12:34:56 +0000 UTC"},
		{Tag: "used", CreatedAt: "2020-01-01 00:00:00 +0000 UTC", InUse: true},
	}
	got := callImageGC(t, images, now, 1, 7*86400)
	if len(got) != 3 {
		t.Fatalf("got %d verdicts, want 3", len(got))
	}
	if got[0].Remove || got[0].Reason != "kept-recent" {
		t.Errorf("newest: %+v", got[0])
	}
	if !got[1].Remove || got[1].Reason != "aged-out" {
		t.Errorf("old: %+v", got[1])
	}
	if got[2].Remove || got[2].Reason != "in-use" {
		t.Errorf("used: %+v", got[2])
	}
}

// The ordering happens module-side, so the caller may send images in any order
// and still get the right ones protected.
func TestImageGCStageOrdersForTheCaller(t *testing.T) {
	now := int64(1784118896)
	images := []SandboxImage{
		{Tag: "old", CreatedAt: "2026-06-15 12:34:56 +0000 UTC"},
		{Tag: "newest", CreatedAt: "2026-07-15 12:34:56 +0000 UTC"},
	}
	got := callImageGC(t, images, now, 1, 1)
	if !got[0].Remove {
		t.Errorf("the older image was protected: %+v", got[0])
	}
	if got[1].Remove {
		t.Errorf("the newest image was removed: %+v", got[1])
	}
}

func TestImageGCStageHandlesAnEmptyInventory(t *testing.T) {
	if got := callImageGC(t, nil, 0, 2, 86400); len(got) != 0 {
		t.Errorf("got %d verdicts for no images", len(got))
	}
}

func TestImageGCStageRejectsMalformedRequests(t *testing.T) {
	good := imageGCRequest([]SandboxImage{{Tag: "a", CreatedAt: "2026-07-15 12:34:56 +0000 UTC"}},
		1784118896, 1, 86400)

	cases := map[string][]byte{
		"empty":         {},
		"short header":  good[:16],
		"trailing byte": append(append([]byte{}, good...), 0),
		"truncated":     good[:len(good)-1],
	}
	badMagic := append([]byte{}, good...)
	binary.LittleEndian.PutUint32(badMagic[0:4], 0xDEADBEEF)
	cases["wrong magic"] = badMagic

	badVersion := append([]byte{}, good...)
	badVersion[4] = wireVersion + 1
	cases["wrong version"] = badVersion

	tooMany := append([]byte{}, good...)
	binary.LittleEndian.PutUint32(tooMany[8:12], imageGCMaxImages+1)
	cases["image count over the bound"] = tooMany

	overrun := append([]byte{}, good...)
	binary.LittleEndian.PutUint32(overrun[imageGCReqHeaderLen+4:imageGCReqHeaderLen+8], 1<<20)
	cases["tag length overruns"] = overrun

	for name, request := range cases {
		if _, status := Handle(bus.ModuleInvocation{StageID: StageImageGC}, request); status != bus.ModuleStatusInvalidRequest {
			t.Errorf("%s: status = %v, want InvalidRequest", name, status)
		}
	}
}

func TestImageGCStageHonoursCancellation(t *testing.T) {
	invocation := bus.ModuleInvocation{StageID: StageImageGC, DeadlineNS: 1}
	request := imageGCRequest([]SandboxImage{{Tag: "a", CreatedAt: "2026-07-15 12:34:56 +0000 UTC"}},
		1784118896, 1, 86400)
	if _, status := Handle(invocation, request); status != bus.ModuleStatusCancelled {
		t.Errorf("status = %v, want Cancelled", status)
	}
}
