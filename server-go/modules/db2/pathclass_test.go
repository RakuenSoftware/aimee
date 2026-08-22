package db2

import "testing"

func TestLanguageFromPath(t *testing.T) {
	// The cases are the ones test_cross_repo_deps.c asserts, so the two
	// classifiers answer the same thing for the same paths. A file's language
	// decides whether a cross-repo corroboration counts, so a disagreement
	// between them would change which definitions resolve.
	for _, testCase := range []struct{ path, want, why string }{
		{"src/a.c", "c", "a bare .c"},
		{"inc/a.h", "c", "a bare .h is C, not C++"},
		{"src/a.cpp", "cpp", "unambiguous C++"},
		{"src/a.hpp", "cpp", "a C++ header"},
		{"src/a.cc", "cpp", "another C++ spelling"},
		{"src/a.cxx", "cpp", "and another"},
		{"src/a.hh", "cpp", "a C++ header spelling"},
		{"src/a.hxx", "cpp", "and another"},
		{"m/lib.rs", "rust", "rust"},
		{"m/main.go", "go", "go"},
		{"ui/app.ts", "ts", "typescript"},
		{"ui/app.tsx", "ts", "typescript with jsx"},
		{"ui/app.js", "js", "javascript"},
		{"ui/app.mjs", "js", "an es module"},
		{"ui/app.cjs", "js", "a commonjs module"},
		{"ui/app.jsx", "js", "javascript with jsx"},
		{"svc/app.py", "python", "python"},
		{"README.md", "unknown", "not source this classifies"},
		{"", "unknown", "nothing classifies as nothing"},
		{"src/a.C", "unknown", "the match is case sensitive"},
		{"noextension", "unknown", "no extension at all"},
	} {
		if got := languageFromPath(testCase.path); got != testCase.want {
			t.Errorf("languageFromPath(%q) = %q, want %q -- %s",
				testCase.path, got, testCase.want, testCase.why)
		}
	}
}

func TestLanguageFromPathPrefersCppBeforeC(t *testing.T) {
	// The order is load-bearing rather than incidental. Testing C first would
	// claim every .cc and .cxx for C, because neither ends in .c -- but a
	// classifier that tested ".c" as a substring rather than a suffix would,
	// and this is the assertion that separates the two mistakes.
	if languageFromPath("src/a.cc") != "cpp" {
		t.Error("a .cc file is being claimed by C")
	}
	if languageFromPath("src/a.h") != "c" {
		t.Error("a bare .h is being claimed by C++")
	}
}

func TestPathIsVendoredMatchesWholeSegments(t *testing.T) {
	// A substring match would call "vendored_thing/x.c" third-party, and a
	// definition under a vendored subtree loses to a first-party one -- so a
	// misclassification here changes which definition wins a collision.
	for _, testCase := range []struct {
		path string
		want bool
		why  string
	}{
		{"vendor/x.c", true, "a vendored segment"},
		{"src/third_party/x.c", true, "not only at the root"},
		{"src/third-party/x.c", true, "the hyphenated spelling"},
		{"node_modules/pkg/index.js", true, "javascript dependencies"},
		{"a/_deps/x.c", true, "the CMake FetchContent cache"},
		{"a/.venv/lib/x.py", true, "a python environment"},
		{"a/site-packages/x.py", true, "installed python packages"},
		{"vendored_thing/x.c", false, "a segment that merely starts with one"},
		{"src/myvendor/x.c", false, "a segment that merely contains one"},
		{"src/main.c", false, "ordinary first-party source"},
		{"", false, "nothing is not vendored"},
	} {
		if got := pathIsVendored(testCase.path); got != testCase.want {
			t.Errorf("pathIsVendored(%q) = %v, want %v -- %s",
				testCase.path, got, testCase.want, testCase.why)
		}
	}
}
