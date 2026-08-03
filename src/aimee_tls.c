/* Legacy test-object shim. Shipping targets get native TLS from
 * libaimee-core-connection; the implementation lives only under src/core. */
#include "core/connection/native_tls_identity.c"
#include "core/connection/native_tls_path.c"
#include "core/connection/native_tls_openssl.c"
