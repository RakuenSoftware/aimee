/* http_uds_client.h: minimal HTTP/1.1 client over aimee-server's /v1 Unix
 * socket. Lets the thin client / TUI reach the server's HTTP API without the
 * legacy RPC socket and without reading server-owned files directly. */
#ifndef DEC_HTTP_UDS_CLIENT_H
#define DEC_HTTP_UDS_CLIENT_H 1

#ifdef __cplusplus
extern "C"
{
#endif

   /* Send an HTTP request to aimee-server's /v1 socket (<aimee_home>/aimee-http.sock).
    * method: "GET"/"POST"; path: e.g. "/v1/personas"; body: JSON or NULL.
    * Returns the response body (heap; caller frees) and sets *status_out to the
    * HTTP status. Returns NULL on connect/transport failure (status_out = 0). */
   char *http_uds_request(const char *method, const char *path, const char *body, int *status_out);

   /* Same request, over whichever transport THIS client is configured for: the
    * co-located Unix socket, or the remote aimee-server when one is configured
    * (aimee remote / AIMEE_API_ENDPOINT). Identical contract to
    * http_uds_request() -- heap response body (caller frees), *status_out set to
    * the HTTP status, NULL and *status_out = 0 on transport failure.
    *
    * Callers reaching a server-owned /v1 path should prefer this: calling
    * http_uds_request() directly hardcodes the local socket, so on a remote thin
    * client it fails and reports the server unreachable when the server is
    * merely not local. */
   char *cli_v1_path_request(const char *method, const char *path, const char *body,
                             int *status_out);

#ifdef __cplusplus
}
#endif

#endif /* DEC_HTTP_UDS_CLIENT_H */
