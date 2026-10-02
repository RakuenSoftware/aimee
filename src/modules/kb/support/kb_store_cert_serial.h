/* Descriptor-owned ABI for KB_STORE certificate-serial canonicalization. */
#ifndef AIMEE_KB_STORE_SUPPORT_CERT_SERIAL_H
#define AIMEE_KB_STORE_SUPPORT_CERT_SERIAL_H

#include <stddef.h>

#ifdef AIMEE_KB_STORE_CERT_SERIAL_PREFIX
#define kb_cert_serial_normalize kb_store_support_cert_serial_normalize
#endif

int kb_cert_serial_normalize(const char *serial, char *out, size_t cap);

#endif
