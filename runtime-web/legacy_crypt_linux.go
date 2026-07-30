//go:build linux && cgo

package main

/*
#cgo LDFLAGS: -lcrypt
#define _GNU_SOURCE
#include <crypt.h>
#include <stdlib.h>
#include <string.h>

static int verify_crypt(const char *password, const char *expected) {
    struct crypt_data data;
    memset(&data, 0, sizeof(data));
    char *actual = crypt_r(password, expected, &data);
    int ok = actual != NULL && strlen(actual) == strlen(expected);
    if (ok) {
        unsigned char diff = 0;
        for (size_t i = 0; expected[i] != '\0'; i++)
            diff |= (unsigned char)(actual[i] ^ expected[i]);
        ok = diff == 0;
    }
    volatile unsigned char *p = (volatile unsigned char *)&data;
    for (size_t i = 0; i < sizeof(data); i++) p[i] = 0;
    return ok;
}
*/
import "C"

import "unsafe"

func verifyLegacyPassword(password, expected string) bool {
	p := C.CString(password)
	e := C.CString(expected)
	defer C.free(unsafe.Pointer(p))
	defer C.free(unsafe.Pointer(e))
	return C.verify_crypt(p, e) == 1
}
