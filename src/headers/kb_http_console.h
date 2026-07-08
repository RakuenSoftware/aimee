/* kb_http_console.h — kb HTTP routes serving the aimee-kb web console.
 *
 * The console reaches these with its scope:console-admin bearer (see
 * kb_route_acl.h). S0 registers the /v1/console/overview stub; S1 fills it with
 * an in-process telemetry fan-in. */
#ifndef KB_HTTP_CONSOLE_H
#define KB_HTTP_CONSOLE_H

/* Handle a console route under /v1/console. Returns the HTTP status if (method,
 * path) is a console route, or -1 if it is not (so the caller continues). */
int kb_http_console_route(const char *method, const char *path, const char *body, char *out_buf,
                          int out_cap);

#endif /* KB_HTTP_CONSOLE_H */
