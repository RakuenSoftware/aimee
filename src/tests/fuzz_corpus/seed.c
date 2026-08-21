#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct
{
   char *host;
   int port;
} legacy_config_record;

struct server
{
   legacy_config_record cfg;
   int running;
};

static legacy_config_record default_config(void)
{
   legacy_config_record c = {.host = "localhost", .port = 8080};
   return c;
}

int server_start(struct server *s)
{
   printf("Starting on %s:%d\n", s->cfg.host, s->cfg.port);
   s->running = 1;
   return 0;
}

#define MAX_CONNECTIONS 100
