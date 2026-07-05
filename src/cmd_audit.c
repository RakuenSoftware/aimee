/* cmd_audit.c: `aimee audit` — verify / checkpoint the WORM audit store. In
 * thin-client mode this routes to /v1/audit/* (server-side, over the store on the
 * server host); this local handler backs the monolith/server-side path and
 * provides command recognition. Exit codes (verify): 0 green, 1 amber, 2 red. */
#include <stdio.h>
#include <stdlib.h>

#include "audit_worm.h"
#include "commands.h"

static void audit_sub_verify(app_ctx_t *ctx, int argc, char **argv)
{
   (void)ctx;
   (void)argc;
   (void)argv;
   char err[256] = "";
   long head = 0, ckpt = 0;
   int st = audit_worm_verify(err, sizeof err, &head, &ckpt);
   if (st == AUDIT_WORM_VERIFY_GREEN)
   {
      printf("audit verify: GREEN — chain + checkpoint MACs intact; %ld row(s), head attested by "
             "checkpoint at seq %ld\n",
             head, ckpt);
      exit(0);
   }
   if (st == AUDIT_WORM_VERIFY_AMBER)
   {
      printf("audit verify: AMBER — chain intact, but %ld row(s) after the newest checkpoint (seq "
             "%ld) are unattested; run 'aimee audit checkpoint'\n",
             head - ckpt, ckpt);
      exit(1);
   }
   printf("audit verify: RED — %s\n", err[0] ? err : "integrity break");
   exit(2);
}

static void audit_sub_checkpoint(app_ctx_t *ctx, int argc, char **argv)
{
   (void)ctx;
   (void)argc;
   (void)argv;
   if (audit_worm_checkpoint() == 0)
   {
      printf("audit checkpoint: ok\n");
      return;
   }
   fprintf(stderr, "audit checkpoint: failed\n");
   exit(1);
}

static void audit_sub_seal(app_ctx_t *ctx, int argc, char **argv)
{
   (void)ctx;
   (void)argc;
   (void)argv;
   char path[1024] = "";
   int immutable = 0;
   if (audit_worm_seal(path, sizeof path, &immutable) == 0)
   {
      printf("audit seal: ok — %s (%s)\n", path,
             immutable ? "OS-immutable" : "crypto-only (no CAP_LINUX_IMMUTABLE)");
      return;
   }
   fprintf(stderr, "audit seal: failed\n");
   exit(1);
}

static void audit_sub_snapshot(app_ctx_t *ctx, int argc, char **argv)
{
   (void)ctx;
   (void)argc;
   (void)argv;
   if (audit_worm_metric_snapshot() == 0)
   {
      printf("audit snapshot: ok\n");
      return;
   }
   fprintf(stderr, "audit snapshot: failed\n");
   exit(1);
}

static const subcmd_t audit_subcmds[] = {
    {"verify", "Verify the WORM audit chain + checkpoint MACs (exit 0=green, 1=amber, 2=red)",
     audit_sub_verify},
    {"checkpoint", "Append a checkpoint committing the current chain head under the chain key",
     audit_sub_checkpoint},
    {"seal", "Export an immutable, verifiable snapshot of the WORM store", audit_sub_seal},
    {"snapshot", "Append a hash-chained metric.snapshot row", audit_sub_snapshot},
    {NULL, NULL, NULL},
};

void cmd_audit(app_ctx_t *ctx, int argc, char **argv)
{
   if (argc < 1)
   {
      audit_sub_verify(ctx, 0, NULL);
      return;
   }
   if (subcmd_dispatch(audit_subcmds, argv[0], ctx, argc - 1, argv + 1) != 0)
      subcmd_usage("audit", audit_subcmds);
}
