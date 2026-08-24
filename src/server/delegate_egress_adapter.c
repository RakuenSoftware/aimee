/* delegate_egress_adapter.c — legacy server handoff to the Go sole-egress module.
 *
 * This file opens no network sockets and makes no destination decision. The C HTTP
 * listener has already consumed the request head, so it passes that head on stdin
 * and the accepted AF_UNIX connection as fd 3 to aimee-delegate-egress. DNS,
 * allowlisting, resolved-address checks, dialing, header stripping, byte/time bounds,
 * and tunnelling all live in server-go/modules/sandbox. */

#include "delegate_egress_adapter.h"

#include "log.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static int write_all(int fd, const char *p, size_t n)
{
   size_t off = 0;
   while (off < n)
   {
      ssize_t written = write(fd, p + off, n - off);
      if (written < 0 && errno == EINTR)
         continue;
      if (written <= 0)
         return -1;
      off += (size_t)written;
   }
   return 0;
}
int delegate_egress_adapter_serve(int client_fd, int is_uds, const char *head, size_t head_len,
                                  const char *tag)
{
   if (!is_uds || client_fd < 0 || !head || head_len == 0)
   {
      aimee_log(LOG_ERROR, "sandbox-proxy",
                "refusing package egress adapter without a verified Unix client socket");
      return -1;
   }

   const char *helper = getenv("AIMEE_DELEGATE_EGRESS_BIN");
   if (!helper || !helper[0])
      helper = "aimee-delegate-egress";
   if (!tag || !tag[0])
      tag = "sandbox";

   int input[2];
   if (pipe(input) != 0)
   {
      aimee_log(LOG_ERROR, "sandbox-proxy", "could not create Go egress input pipe: %s",
                strerror(errno));
      return -1;
   }

   /* Keep the inherited client clear of stdin, fd 3, and both pipe ends. A
    * daemon may start with stdin closed, so the accepted socket is not
    * guaranteed to have a conventional descriptor number. */
   int inherited_client = fcntl(client_fd, F_DUPFD_CLOEXEC, 4);
   if (inherited_client < 0)
   {
      close(input[0]);
      close(input[1]);
      aimee_log(LOG_ERROR, "sandbox-proxy", "could not duplicate Unix client fd: %s",
                strerror(errno));
      return -1;
   }

   pid_t pid = fork();
   if (pid < 0)
   {
      close(input[0]);
      close(input[1]);
      close(inherited_client);
      aimee_log(LOG_ERROR, "sandbox-proxy", "could not start Go egress module: %s",
                strerror(errno));
      return -1;
   }
   if (pid == 0)
   {
      /* Only async-signal-safe operations between fork and exec: this server is
       * multithreaded. dup2 also clears close-on-exec on the target descriptors. */
      if (dup2(input[0], STDIN_FILENO) < 0 || dup2(inherited_client, 3) < 0)
         _exit(126);
      if (input[0] != STDIN_FILENO && input[0] != 3)
         close(input[0]);
      if (input[1] != STDIN_FILENO && input[1] != 3)
         close(input[1]);
      if (client_fd != STDIN_FILENO && client_fd != 3)
         close(client_fd);
      if (inherited_client != STDIN_FILENO && inherited_client != 3)
         close(inherited_client);
      char *const argv[] = {(char *)helper,
                            (char *)"proxy",
                            (char *)"--fd",
                            (char *)"3",
                            (char *)"--tag",
                            (char *)tag,
                            NULL};
      execvp(helper, argv);
      _exit(127);
   }

   close(inherited_client);
   close(input[0]);
   int sent = write_all(input[1], head, head_len);
   close(input[1]);
   int status = 0;
   while (waitpid(pid, &status, 0) < 0)
      if (errno != EINTR)
         return -1;
   if (sent != 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0)
   {
      aimee_log(LOG_WARN, "sandbox-proxy",
                "%s: Go sole-egress module refused or failed (status=%d)", tag, status);
      return -1;
   }
   return 0;
}
