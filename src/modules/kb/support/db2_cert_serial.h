/* Descriptor-owned ABI for DB2 certificate-serial canonicalization. */
#ifndef AIMEE_DB2_SUPPORT_CERT_SERIAL_H
#define AIMEE_DB2_SUPPORT_CERT_SERIAL_H

#include <stddef.h>

#ifdef AIMEE_DB2_CERT_SERIAL_PREFIX
#define kb_cert_serial_normalize db2_support_cert_serial_normalize
#endif

int kb_cert_serial_normalize(const char *serial, char *out, size_t cap);

#endif
