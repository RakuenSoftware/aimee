/* server_http_conn_worker.inc: the per-connection worker + offload, split out of
 * server_http.c for its line budget. Included from server_http.c after handle_conn,
 * conn_job_t, g_conn_live/CONN_LIVE_MAX, and the server_tls/server_conn_io decls it
 * relies on. */

static void *conn_worker(void *arg)
{
   conn_job_t *j = (conn_job_t *)arg;
   /* TLS handshake runs HERE (in the worker, never blocking the accept loop) and
    * registers the SSL on the conn-io shim; this worker owns it end to end
    * (SSE-offload is refused over TLS, so it never crosses threads). */
   SSL *ssl = j->is_tls ? server_tls_begin(j->fd) : NULL;
   if (!(j->is_tls && !ssl)) /* skip handle_conn only when the TLS handshake failed */
      handle_conn(j->fd, j->is_tcp);
   server_tls_end(j->fd, ssl);
   close(j->fd);
   atomic_fetch_sub(&g_conn_live, 1);
   free(j);
   return NULL;
}

/* Hand an accepted connection to a detached worker (which closes fd). Returns 1
 * if offloaded, 0 if the caller should handle it inline (cap hit / no resources). */
static int conn_offload(int fd, int is_tcp, int is_tls)
{
   if (atomic_fetch_add(&g_conn_live, 1) >= CONN_LIVE_MAX)
   {
      atomic_fetch_sub(&g_conn_live, 1);
      return 0;
   }
   conn_job_t *j = (conn_job_t *)malloc(sizeof(*j));
   if (!j)
   {
      atomic_fetch_sub(&g_conn_live, 1);
      return 0;
   }
   j->fd = fd;
   j->is_tcp = is_tcp;
   j->is_tls = is_tls;
   pthread_attr_t attr;
   pthread_attr_t *ap = NULL;
   if (pthread_attr_init(&attr) == 0)
   {
      if (pthread_attr_setstacksize(&attr, (size_t)32 * 1024 * 1024) == 0)
         ap = &attr;
   }
   pthread_t t;
   int rc = pthread_create(&t, ap, conn_worker, j);
   if (ap)
      pthread_attr_destroy(&attr);
   if (rc != 0)
   {
      free(j);
      atomic_fetch_sub(&g_conn_live, 1);
      return 0;
   }
   pthread_detach(t);
   return 1;
}
