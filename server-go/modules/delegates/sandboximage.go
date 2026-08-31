package delegates

import (
	"crypto/sha256"
	"encoding/hex"
	"fmt"
	"strings"
)

// Which image a delegate's sandbox runs, and how one is built from a spec.
//
// The sandbox has no network, so its toolchain has to be baked into the image
// at build time -- a Rust repo needs cargo, a C repo gcc and make, a docs repo
// nothing. A project can therefore declare a base image plus packages, and a
// derived image is built once and reused by content.
//
// The build itself is I/O and belongs to the caller. Deciding WHAT to build --
// and refusing a specification that would smuggle shell into the build -- is a
// decision and lives here.

const sandboxTagPrefix = "aimee-sbx:"

// sandboxTagHexLen is how much of the content hash names the image. Twelve hex
// characters matches the existing tags, so images built before this code are
// still found and reused rather than silently rebuilt.
const sandboxTagHexLen = 12

// packageNameValid reports whether a package name is safe to place in a build
// RUN line.
//
// This is the injection boundary. The name goes into a shell command inside
// `docker build`, so anything outside this set -- a space, a semicolon, a
// backtick, $( -- would let a project's config file run arbitrary commands
// during the build, which happens WITH network access. Must start
// alphanumeric, then alphanumerics and . _ + : - only.
func packageNameValid(pkg string) bool {
	if pkg == "" {
		return false
	}
	if !isAlnum(pkg[0]) {
		return false
	}
	for i := 0; i < len(pkg); i++ {
		c := pkg[i]
		if isAlnum(c) || c == '.' || c == '_' || c == '+' || c == ':' || c == '-' {
			continue
		}
		return false
	}
	return true
}

// baseImageValid allows what an image reference legitimately contains. Same
// injection boundary as a package name, plus '/' for registry and repository
// segments.
func baseImageValid(base string) bool {
	if base == "" {
		return false
	}
	for i := 0; i < len(base); i++ {
		c := base[i]
		if isAlnum(c) || c == '.' || c == '_' || c == '+' || c == ':' || c == '-' || c == '/' {
			continue
		}
		return false
	}
	return true
}

// SandboxDockerfile renders the build for a base image plus packages.
//
// With no packages the result is just the FROM line: a project that names a
// base and installs nothing should not get an apt invocation it never asked
// for. The apt list is removed in the same layer so the image does not carry
// the package index around.
func SandboxDockerfile(base string, packages []string) (string, error) {
	if !baseImageValid(base) {
		return "", fmt.Errorf("invalid base image reference: %q", base)
	}
	for _, pkg := range packages {
		if !packageNameValid(pkg) {
			return "", fmt.Errorf("invalid package name: %q", pkg)
		}
	}
	if len(packages) == 0 {
		return fmt.Sprintf("FROM %s\n", base), nil
	}
	return fmt.Sprintf(
		"FROM %s\n"+
			"RUN apt-get update && apt-get install -y --no-install-recommends %s && "+
			"rm -rf /var/lib/apt/lists/*\n",
		base, strings.Join(packages, " ")), nil
}

// SandboxContentTag names an image by what it contains, so identical content
// resolves to the same tag and an already-built image is reused instead of
// rebuilt.
func SandboxContentTag(content string) string {
	sum := sha256.Sum256([]byte(content))
	return sandboxTagPrefix + hex.EncodeToString(sum[:])[:sandboxTagHexLen]
}

// SandboxImageSource is where a resolved image came from. The order is the
// precedence order, most specific first.
type SandboxImageSource int

const (
	// SandboxImageNone means nothing was configured and the caller should use
	// its default image.
	SandboxImageNone SandboxImageSource = iota
	// SandboxImageProject is the repo's own declaration -- it travels with the
	// code, because the code knows what toolchain it needs.
	SandboxImageProject
	// SandboxImageWorkspace is a per-workspace override.
	SandboxImageWorkspace
	// SandboxImageGlobal is the installation-wide default.
	SandboxImageGlobal
)

// SandboxImageCandidates is what the caller found configured. Each is the image
// reference or empty. Reading the files is the caller's job; choosing between
// them is the rule.
type SandboxImageCandidates struct {
	Project   string
	Workspace string
	Global    string
}

// ResolveSandboxImage picks the image, most specific first.
//
// The project's own declaration wins because it travels with the code: a repo
// that says it needs a Rust toolchain is right about that regardless of what
// the workspace or the installation prefers. An invalid reference is refused
// rather than silently skipped -- falling through to a less specific image
// would run the delegate with a toolchain nobody chose.
func ResolveSandboxImage(c SandboxImageCandidates) (string, SandboxImageSource, error) {
	for _, candidate := range []struct {
		ref    string
		source SandboxImageSource
	}{
		{c.Project, SandboxImageProject},
		{c.Workspace, SandboxImageWorkspace},
		{c.Global, SandboxImageGlobal},
	} {
		if candidate.ref == "" {
			continue
		}
		if !baseImageValid(candidate.ref) {
			return "", SandboxImageNone,
				fmt.Errorf("invalid sandbox image reference: %q", candidate.ref)
		}
		return candidate.ref, candidate.source, nil
	}
	return "", SandboxImageNone, nil
}
