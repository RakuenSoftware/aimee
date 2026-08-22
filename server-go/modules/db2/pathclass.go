package db2

import "strings"

// languageFromPath classifies a file by its extension, mirroring
// xrepo_lang_from_path.
//
// The order matters and is the C's. C++ extensions are tested first because
// they are unambiguous, and .h is then claimed by C -- a header could belong to
// either, and the choice is that a bare .h is C. Getting that backwards would
// reclassify every C header in the index.
//
// The comparison is a case-sensitive suffix match, again the C's: a file named
// README.C is not C source on the systems this indexes, and lowercasing would
// silently start treating it as one.
func languageFromPath(path string) string {
	if path == "" {
		return "unknown"
	}
	for _, candidate := range []struct {
		name       string
		extensions []string
	}{
		{"cpp", []string{".cpp", ".cc", ".cxx", ".hpp", ".hh", ".hxx"}},
		{"c", []string{".c", ".h"}},
		{"rust", []string{".rs"}},
		{"go", []string{".go"}},
		{"ts", []string{".tsx", ".ts"}},
		{"js", []string{".jsx", ".js", ".mjs", ".cjs"}},
		{"python", []string{".py"}},
	} {
		for _, extension := range candidate.extensions {
			if strings.HasSuffix(path, extension) {
				return candidate.name
			}
		}
	}
	return "unknown"
}

// vendoredDirectories are the path segments that mark third-party code, from
// xrepo_path_is_vendored.
var vendoredDirectories = map[string]bool{
	"vendor": true, "third_party": true, "third-party": true, "extern": true,
	"external": true, "deps": true, ".deps": true, "_deps": true,
	"subprojects": true, "Pods": true, "node_modules": true,
	"bower_components": true, ".venv": true, "venv": true, "site-packages": true,
}

// pathIsVendored reports whether a path runs through a third-party subtree.
//
// A whole segment has to match, which is the point: "vendored_thing/x.c" is
// first-party and "vendor/x.c" is not. A substring match would misclassify the
// first, and a definition under a vendored subtree is not its repository's own
// API -- the resolver prefers a non-vendored definer and routes vendored-only
// collisions to ambiguous, so the flag decides which definition wins.
func pathIsVendored(path string) bool {
	if path == "" {
		return false
	}
	for _, segment := range strings.Split(path, "/") {
		if vendoredDirectories[segment] {
			return true
		}
	}
	return false
}
