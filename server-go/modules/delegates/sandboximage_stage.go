package delegates

import (
	"encoding/binary"

	"github.com/JBailes/aimee/server-go/bus"
)

// Naming and describing the image a delegate's sandbox is built from.
//
// Two answers, together, because they are one decision: the Dockerfile text and
// the tag that names it. The tag is a hash OF that text, so computing them apart
// is how an image gets built under a name that describes different content --
// and the whole point of a content tag is that identical content resolves to the
// same tag and an already-built image is reused rather than rebuilt.
//
// The package and base names are an INJECTION BOUNDARY: they are interpolated
// into a Dockerfile that `docker build` then executes, with a network. That is
// why the validation lives with the rule that uses it rather than at the caller.

const (
	StageImageSpec uint32 = 13
	EventImageSpec uint32 = 6669

	imageSpecRequestMagic  uint32 = 0x51494d44 /* "DMIQ" */
	imageSpecResponseMagic uint32 = 0x53494d44 /* "DMIS" */
	imageSpecReqHeaderLen         = 16

	imageSpecBaseMax       = 512
	imageSpecPackageMax    = 256
	imageSpecMaxPackages   = 512
	imageSpecDockerfileMax = 1 << 20
)

// handleImageSpec renders the Dockerfile for a base plus packages, and the tag
// that names its content.
//
// A rejected base or package yields NOTHING, not a Dockerfile with the bad name
// dropped. Silently building a narrower image than was asked for would leave the
// delegate missing a tool with nothing to say why -- and quietly accepting the
// name is how the injection lands.
func handleImageSpec(invocation bus.ModuleInvocation, request []byte) ([]byte, bus.ModuleStatus) {
	if len(request) < imageSpecReqHeaderLen ||
		binary.LittleEndian.Uint32(request[0:4]) != imageSpecRequestMagic ||
		request[4] != wireVersion {
		return nil, bus.ModuleStatusInvalidRequest
	}
	count := int(binary.LittleEndian.Uint32(request[8:12]))
	if count > imageSpecMaxPackages {
		return nil, bus.ModuleStatusInvalidRequest
	}

	c := &economicsCursor{buf: request, at: imageSpecReqHeaderLen}
	readString := func(max int) string {
		n := c.u32()
		if n > max {
			c.bad = true
			return ""
		}
		return c.str(n)
	}

	base := readString(imageSpecBaseMax)
	packages := make([]string, 0, count)
	for i := 0; i < count; i++ {
		packages = append(packages, readString(imageSpecPackageMax))
	}
	// A Dockerfile the operator wrote and committed, carried whole. It is not
	// rendered from anything, so there is nothing here to validate -- but it
	// still has to be NAMED here, or the tag's shape exists in two places and
	// the same content resolves to two different names, which is precisely the
	// reuse the content tag is for.
	verbatim := readString(imageSpecDockerfileMax)
	if c.bad || c.at != len(request) {
		return nil, bus.ModuleStatusInvalidRequest
	}
	if verbatim != "" && (base != "" || len(packages) > 0) {
		// Two descriptions of one image. Refuse rather than silently pick.
		return nil, bus.ModuleStatusInvalidRequest
	}
	if invocation.Cancelled() {
		return nil, bus.ModuleStatusCancelled
	}

	dockerfile := verbatim
	if dockerfile == "" {
		rendered, err := SandboxDockerfile(base, packages)
		if err != nil {
			return nil, bus.ModuleStatusInvalidRequest
		}
		dockerfile = rendered
	}
	tag := SandboxContentTag(dockerfile)

	response := make([]byte, 12, 12+len(tag)+len(dockerfile))
	binary.LittleEndian.PutUint32(response[0:4], imageSpecResponseMagic)
	binary.LittleEndian.PutUint32(response[4:8], uint32(len(tag)))
	binary.LittleEndian.PutUint32(response[8:12], uint32(len(dockerfile)))
	response = append(response, tag...)
	response = append(response, dockerfile...)
	return response, bus.ModuleStatusOK
}
