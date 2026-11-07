#ifndef DEC_AGENT_TUNNEL_H
#define DEC_AGENT_TUNNEL_H 1

#include "agent_types.h"

/* Initialize tunnel manager (zeroes state, inits mutex). */
void agent_tunnel_mgr_init(agent_tunnel_mgr_t *mgr);

/* Start all configured tunnels. Forks ssh processes, spawns monitor threads.
 * Returns 0 if at least one tunnel started, -1 if all failed or none configured. */
int agent_tunnel_start_all(agent_tunnel_mgr_t *mgr);

/* Stop all tunnels. Kills ssh processes, joins monitor threads. */
void agent_tunnel_stop_all(agent_tunnel_mgr_t *mgr);

/* Get the effective ssh entry for a host considering its tunnel.
 * If the host has an active tunnel, writes tunnel entry to buf.
 * Otherwise writes network->ssh_entry to buf.
 * Returns 1 if tunnel was used, 0 if fallback. */
int agent_tunnel_resolve_entry(const agent_tunnel_mgr_t *mgr, const agent_network_t *network,
                               const agent_net_host_t *host, char *buf, size_t buf_len);

#endif /* DEC_AGENT_TUNNEL_H */
