#ifndef KB_MGMT_ENDPOINT_H
#define KB_MGMT_ENDPOINT_H
/* Validate a registered management endpoint before DNS/connect. Returns 0 only
 * for an https origin with an explicit non-private hostname/port shape. */
int kb_mgmt_endpoint_validate(const char *endpoint);
#endif
