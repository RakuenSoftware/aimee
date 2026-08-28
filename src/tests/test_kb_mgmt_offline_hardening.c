#include "kb/kb_mgmt_offline_hardening.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
   const char header[] = "Filename\tType\tSize\tUsed\tPriority\n";
   const char active[] = "Filename\tType\tSize\tUsed\tPriority\n/dev/zram0 partition 1024 0 100\n";
   const char malformed[] = "Filename\n";

   assert(kb_mgmt_offline_swaps_text_active(header, strlen(header)) == 0);
   assert(kb_mgmt_offline_swaps_text_active(active, strlen(active)) == 1);
   assert(kb_mgmt_offline_swaps_text_active(malformed, strlen(malformed)) == -1);
   assert(kb_mgmt_offline_swaps_text_active("", 0) == -1);
   assert(kb_mgmt_offline_swaps_text_active(NULL, 1) == -1);

   assert(kb_mgmt_offline_cgroup_swap_text_disabled("0\n", 2) == 1);
   assert(kb_mgmt_offline_cgroup_swap_text_disabled("0", 1) == 1);
   assert(kb_mgmt_offline_cgroup_swap_text_disabled("0 \t\n", 4) == 1);
   assert(kb_mgmt_offline_cgroup_swap_text_disabled("max\n", 4) == 0);
   assert(kb_mgmt_offline_cgroup_swap_text_disabled("1\n", 2) == 0);
   assert(kb_mgmt_offline_cgroup_swap_text_disabled("00\n", 3) == 0);
   assert(kb_mgmt_offline_cgroup_swap_text_disabled("", 0) == 0);
   assert(kb_mgmt_offline_cgroup_swap_text_disabled(NULL, 1) == 0);
   printf("test_kb_mgmt_offline_hardening: all passed\n");
   return 0;
}
