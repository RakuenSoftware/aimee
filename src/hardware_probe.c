#include "hardware_probe.h"
#include "util.h"
#include "db1_client/db1.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* One shell probe, run locally (popen) and remotely (over ssh), emitting one CSV
 * line per GPU: "<index>, <name>, <vram_mb>, <vendor>". NVIDIA via nvidia-smi
 * (memory.total is already MiB with nounits); else AMD via /sys/class/drm. Only
 * double-quotes inside, and no single-quotes, so the whole string can be wrapped
 * in single-quotes for the remote `ssh <target> '<cmd>'` without local expansion. */
const char *const HARDWARE_GPU_PROBE_CMD =
    "out=$(nvidia-smi --query-gpu=index,name,memory.total --format=csv,noheader,nounits "
    "2>/dev/null); "
    "if [ -n \"$out\" ]; then echo \"$out\" | awk 'NF{print $0\", nvidia\"}'; else "
    "i=0; for d in /sys/class/drm/card[0-9]*; do "
    "[ -e \"$d/device/vendor\" ] || continue; "
    "[ \"$(cat \"$d/device/vendor\" 2>/dev/null)\" = \"0x1002\" ] || continue; "
    "n=$(cat \"$d/device/product_name\" 2>/dev/null); [ -n \"$n\" ] || n=\"AMD GPU\"; "
    "b=$(cat \"$d/device/mem_info_vram_total\" 2>/dev/null); [ -n \"$b\" ] || b=0; "
    "printf \"%s, %s, %s, amd\\n\" \"$i\" \"$n\" \"$((b/1048576))\"; "
    "i=$((i+1)); done; fi";

int hardware_probe_parse_gpu_csv_list(const char *csv, const char *vendor_hint,
                                      hardware_gpu_list_t *out)
{
   if (!out)
      return 0;
   memset(out, 0, sizeof(*out));
   if (!csv)
      return 0;

   char buf[8192];
   snprintf(buf, sizeof(buf), "%s", csv);
   char *saveptr = NULL;
   char *line = strtok_r(buf, "\r\n", &saveptr);
   while (line && out->count < HARDWARE_MAX_GPUS)
   {
      /* Parse "<index>, <name...>, <vram_mb>, <vendor>" from BOTH ends so a name
       * that itself contains a comma still splits correctly: index is before the
       * first comma, vendor after the last, vram before that, name is the middle. */
      char *first = strchr(line, ',');
      char *vend_c = strrchr(line, ',');
      if (first && vend_c && vend_c > first)
      {
         *vend_c = '\0';
         char *vendor = util_trim(vend_c + 1);
         char *vram_c = strrchr(line, ',');
         if (vram_c && vram_c > first)
         {
            *vram_c = '\0';
            char *vram = util_trim(vram_c + 1);
            *first = '\0';
            char *idx = util_trim(line);
            char *name = util_trim(first + 1);
            char *e = NULL;
            long mb = strtol(vram, &e, 10);
            if (mb > 0 && mb <= 1024L * 1024L && name[0])
            {
               hardware_gpu_t *g = &out->gpus[out->count++];
               g->index = (int)strtol(idx, NULL, 10);
               g->vram_mb = (int)mb;
               snprintf(g->name, sizeof(g->name), "%s", name);
               const char *v =
                   (vendor && vendor[0]) ? vendor : (vendor_hint ? vendor_hint : "unknown");
               snprintf(g->vendor, sizeof(g->vendor), "%s", v);
            }
         }
      }
      line = strtok_r(NULL, "\r\n", &saveptr);
   }
   return out->count;
}

/* Drain a popen stream into buf (NUL-terminated), then parse the GPU CSV. */
static int hp_read_stream_gpus(FILE *fp, hardware_gpu_list_t *out)
{
   char csv[8192] = {0};
   size_t pos = 0;
   while (pos + 1 < sizeof(csv) && fgets(csv + pos, (int)(sizeof(csv) - pos), fp))
      pos = strlen(csv);
   return hardware_probe_parse_gpu_csv_list(csv, NULL, out);
}

int hardware_probe_list_local(hardware_gpu_list_t *out)
{
   if (!out)
      return 0;
   memset(out, 0, sizeof(*out));
   FILE *fp = popen(HARDWARE_GPU_PROBE_CMD, "r");
   if (!fp)
   {
      snprintf(out->error, sizeof(out->error), "could not run local GPU probe");
      return 0;
   }
   hp_read_stream_gpus(fp, out);
   pclose(fp);
   return out->count;
}

/* Reject an ssh target that isn't a plain user@host token, so it can't inject
 * shell metacharacters into the popen'd ssh command line. */
static int hp_ssh_target_ok(const char *t)
{
   if (!t || !t[0] || strlen(t) >= 256)
      return 0;
   for (const char *p = t; *p; p++)
      if (!isalnum((unsigned char)*p) && !strchr("@._-:", *p))
         return 0;
   return 1;
}

int hardware_probe_list_remote(const char *ssh_target, int port, hardware_gpu_list_t *out)
{
   if (!out)
      return 0;
   memset(out, 0, sizeof(*out));
   if (!hp_ssh_target_ok(ssh_target))
   {
      snprintf(out->error, sizeof(out->error), "invalid ssh target");
      return 0;
   }
   const char *ssh_bin = getenv("AIMEE_SSH_BIN");
   if (!ssh_bin || !ssh_bin[0])
      ssh_bin = "ssh";
   char port_flag[24] = "";
   if (port > 0)
      snprintf(port_flag, sizeof(port_flag), "-p %d ", port);

   /* Single-quote the probe so the LOCAL shell passes it verbatim to ssh, which
    * hands it to the REMOTE shell (where the $()/for expands). The probe contains
    * no single-quote, so this wrapping is safe. */
   char cmd[9216];
   snprintf(cmd, sizeof(cmd),
            "%s -o BatchMode=yes -o ConnectTimeout=8 -o StrictHostKeyChecking=accept-new %s%s '%s'",
            ssh_bin, port_flag, ssh_target, HARDWARE_GPU_PROBE_CMD);
   FILE *fp = popen(cmd, "r");
   if (!fp)
   {
      snprintf(out->error, sizeof(out->error), "could not launch ssh to %s", ssh_target);
      return 0;
   }
   hp_read_stream_gpus(fp, out);
   int rc = pclose(fp);
   /* A non-zero ssh exit with no GPUs means the host was unreachable / auth failed;
    * surface it so the picker can show "unreachable" rather than "no GPU". (rc is
    * the raw pclose status — portable across platforms without decoding it.) */
   if (out->count == 0 && rc != 0)
      snprintf(out->error, sizeof(out->error), "ssh probe of %s failed (unreachable or auth)",
               ssh_target);
   return out->count;
}
