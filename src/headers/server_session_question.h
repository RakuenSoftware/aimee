#ifndef AIMEE_SERVER_SESSION_QUESTION_H
#define AIMEE_SERVER_SESSION_QUESTION_H
/* Uses a retained current control owner and typed durable session events.
 * Returns owned answer/error text; NULL means no current browser control owner.
 * The caller's workspace and cancellation leases enclose the synchronous call. */
char *server_session_question(const char *question, int timeout_seconds);
#endif
