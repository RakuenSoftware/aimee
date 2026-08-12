package delegates

import (
	"encoding/binary"

	"github.com/JBailes/aimee/server-go/bus"
)

// Which built sandbox images may be deleted.
//
// The whole inventory goes over at once, because the decision is positional:
// "keep the keep_min most recent" cannot be answered one image at a time. A
// per-image call would force the caller to do the ordering, which is the part
// most likely to be got wrong and the part with no local symptom when it is.

const (
	StageImageGC uint32 = 16
	EventImageGC uint32 = 6672

	imageGCRequestMagic  uint32 = 0x51434744 /* "DGCQ" */
	imageGCResponseMagic uint32 = 0x53434744 /* "DGCS" */
	imageGCReqHeaderLen         = 32

	imageGCMaxImages = 4096
	imageGCStringMax = 512
)

// handleImageGC returns one verdict per image, in the order they were sent.
func handleImageGC(invocation bus.ModuleInvocation, request []byte) ([]byte, bus.ModuleStatus) {
	if len(request) < imageGCReqHeaderLen ||
		binary.LittleEndian.Uint32(request[0:4]) != imageGCRequestMagic ||
		request[4] != wireVersion {
		return nil, bus.ModuleStatusInvalidRequest
	}
	count := int(binary.LittleEndian.Uint32(request[8:12]))
	if count > imageGCMaxImages {
		return nil, bus.ModuleStatusInvalidRequest
	}
	keepMin := int(int32(binary.LittleEndian.Uint32(request[12:16])))
	now := int64(binary.LittleEndian.Uint64(request[16:24]))
	maxAge := int64(binary.LittleEndian.Uint64(request[24:32]))

	c := &economicsCursor{buf: request, at: imageGCReqHeaderLen}
	images := make([]SandboxImage, 0, count)
	for i := 0; i < count; i++ {
		inUse := c.u32() == 1
		tagLen := c.u32()
		createdLen := c.u32()
		if tagLen > imageGCStringMax || createdLen > imageGCStringMax {
			return nil, bus.ModuleStatusInvalidRequest
		}
		images = append(images, SandboxImage{
			Tag:       c.str(tagLen),
			CreatedAt: c.str(createdLen),
			InUse:     inUse,
		})
	}
	if c.bad || c.at != len(request) {
		return nil, bus.ModuleStatusInvalidRequest
	}
	if invocation.Cancelled() {
		return nil, bus.ModuleStatusCancelled
	}

	verdicts := JudgeSandboxImages(images, now, keepMin, maxAge)

	total := 8
	for _, v := range verdicts {
		total += 8 + len(v.Reason)
	}
	response := make([]byte, 8, total)
	binary.LittleEndian.PutUint32(response[0:4], imageGCResponseMagic)
	binary.LittleEndian.PutUint32(response[4:8], uint32(len(verdicts)))
	for _, v := range verdicts {
		var hdr [8]byte
		if v.Remove {
			binary.LittleEndian.PutUint32(hdr[0:4], 1)
		}
		binary.LittleEndian.PutUint32(hdr[4:8], uint32(len(v.Reason)))
		response = append(response, hdr[:]...)
		response = append(response, v.Reason...)
	}
	return response, bus.ModuleStatusOK
}
