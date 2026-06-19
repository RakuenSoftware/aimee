/* server_http_sse.inc: cursor-resumable session-event SSE stream handler.
 *
 * Textually included by server_http.c (NOT compiled separately) so it shares
 * that translation unit's static helpers (send_response, write_sse_headers,
 * write_all_fd). Extracted from server_http.c purely to keep that file under the
 * 2000-line hard cap; the logic is unchanged. WP-5 of the server-owned turn
 * lifecycle: a reconnecting client resumes from its persisted ring cursor. */
static void handle_session_events(int fd, const char *id_in, const char *request_id)
{
   /* id_in may carry a resume position as "<sid>?cursor=N" (threaded from the
    * dispatcher's query string). Split it: `id` is the bare session id, `cursor`
    * the absolute ring position to resume from (0 = replay from oldest retained,
    * the original behavior). */
   char id[128];
   snprintf(id, sizeof(id), "%s", id_in);
   uint64_t cursor = 0;
   char *q = strchr(id, '?');
   if (q)
   {
      *q = '\0';
      const char *c = strstr(q + 1, "cursor=");
      if (c)
         cursor = (uint64_t)strtoull(c + 7, NULL, 10);
   }

   char probe[80];
   if (presence_session_json(id, probe, sizeof(probe)) == 0)
   {
      send_response(fd, 404, "{\"error\":\"no such session\"}", request_id);
      return;
   }
   write_sse_headers(fd, request_id);
   char *data = (char *)malloc(PRESENCE_EVENT_DATA_MAX + 1);
   if (!data)
      return; /* headers already sent; just drop the stream */
   char ev[PRESENCE_EVENT_NAME_MAX];
   for (;;)
   {
      presence_wait_t w =
          presence_wait(id, &cursor, 1000, ev, sizeof(ev), data, PRESENCE_EVENT_DATA_MAX + 1);
      if (w == PRESENCE_WAIT_EVENT)
      {
         /* Emit the post-advance cursor as the SSE id so the client can persist
          * it and resume from exactly here after a reconnect/remount (?cursor=). */
         char idline[40];
         int idn = snprintf(idline, sizeof(idline), "id: %llu\n", (unsigned long long)cursor);
         if (idn > 0 && write_all_fd(fd, idline, idn) < 0)
            break;
         if (ev[0])
         {
            if (write_all_fd(fd, "event: ", 7) < 0)
               break;
            if (write_all_fd(fd, ev, (int)strlen(ev)) < 0)
               break;
            if (write_all_fd(fd, "\n", 1) < 0)
               break;
         }
         if (write_all_fd(fd, "data: ", 6) < 0)
            break;
         if (write_all_fd(fd, data, (int)strlen(data)) < 0)
            break;
         if (write_all_fd(fd, "\n\n", 2) < 0)
            break;
         continue;
      }
      if (w == PRESENCE_WAIT_GONE)
         break;
      /* PRESENCE_WAIT_TIMEOUT: heartbeat; a failed write means the client
       * disconnected, so stop streaming and free the listener. */
      if (write_all_fd(fd, ": keep-alive\n\n", 13) < 0)
         break;
   }
   free(data);
}
