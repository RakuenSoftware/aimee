/* server_conn_io.h: the aimee-server HTTP connection I/O shim (native-TLS phase 1).
 *
 * The server's HTTP path does raw read()/write() on the accepted fd. To let a
 * connection ride TLS without threading a conn_io struct through every streaming
 * handler + the detached SSE worker, we keep the existing `int fd` signatures and
 * route I/O through a tiny fd->SSL registry: a fd with a registered SSL* reads/
 * writes via SSL_read/SSL_write, otherwise raw read/write. With nothing registered
 * (the default), behavior is byte-identical to the previous raw path.
 *
 * The TLS listener (phase 1b) registers the SSL on accept and clears+frees it when
 * the fd is closed; each fd's SSL is used by a single thread at a time (the conn
 * handler, or the SSE worker after hand-off), so no concurrent SSL op on one SSL. */
#ifndef DEC_SERVER_CONN_IO_H
#define DEC_SERVER_CONN_IO_H 1

#include <openssl/ssl.h>

#ifdef __cplusplus
extern "C"
{
#endif

   /* Register / clear the SSL backing `fd`. Setting NULL (or clearing) reverts the
    * fd to the raw-fd path. Idempotent; bounds-checked. */
   void server_conn_io_set_ssl(int fd, SSL *ssl);
   void server_conn_io_clear(int fd);

   /* read()/write-all over the fd's transport (SSL if registered, else raw). read
    * returns bytes read (0 EOF, <0 error); write_all returns `n` or -1. */
   int server_conn_io_read(int fd, void *buf, int n);
   int server_conn_io_write_all(int fd, const void *buf, int n);

   /* 1 if `fd` has a registered SSL (a TLS connection), else 0. Used to refuse
    * SSE-offload over TLS (the offload dups the fd to a thread that can't share
    * the conn's SSL). */
   int server_conn_io_has_ssl(int fd);

   /* The registered SSL for `fd` (or NULL). Used by the identity layer to read a
    * verified mTLS client cert. */
   SSL *server_conn_io_get_ssl(int fd);

#ifdef __cplusplus
}
#endif

#endif /* DEC_SERVER_CONN_IO_H */
