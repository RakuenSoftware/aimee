/* agent_config_internal.h: helpers shared between agent_config.c and
 * agent_route.c after the routing split, and deliberately not part of the
 * public agent_config.h surface. Include only from those two TUs. */
#ifndef AIMEE_AGENT_CONFIG_INTERNAL_H
#define AIMEE_AGENT_CONFIG_INTERNAL_H

/* 1 if `endpoint` points at the local machine. Used both to decide whether an
 * unauthenticated agent may inject the respond tool (agent_config.c) and to
 * classify an agent as local for prefer_local routing (agent_route.c). */
int agent_endpoint_is_localish(const char *endpoint);

#endif /* AIMEE_AGENT_CONFIG_INTERNAL_H */
