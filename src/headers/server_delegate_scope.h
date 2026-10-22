#ifndef AIMEE_SERVER_DELEGATE_SCOPE_H
#define AIMEE_SERVER_DELEGATE_SCOPE_H
/* Borrowed execution bookmark. A native listener is nested inside the host
 * delegate/workspace lease and joins its callbacks before that lease ends. */
typedef struct
{
   char parent[64];
   int depth;
   void *mailbox;
} server_delegate_scope_t;
void server_delegate_scope_capture(server_delegate_scope_t *out);
void server_delegate_scope_restore(const server_delegate_scope_t *scope);
#endif
