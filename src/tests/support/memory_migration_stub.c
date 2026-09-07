/* Narrow test doubles for legacy DB2 callers whose memory providers moved to Go. */
#include "aimee.h"
#include "modules/memory/memory_ontology.h"

#include <stdio.h>

const char *memory_ontology_node_kind_to_text(memory_node_kind_t kind)
{
   switch (kind)
   {
   case NODE_FILE:
      return "file";
   case NODE_FUNCTION:
      return "function";
   case NODE_STRUCT:
      return "struct";
   case NODE_MODULE:
      return "module";
   case NODE_BUG:
      return "bug";
   case NODE_COMMIT:
      return "commit";
   case NODE_PR:
      return "pr";
   case NODE_DEVELOPER:
      return "developer";
   case NODE_CONCEPT:
      return "concept";
   case NODE_EVENT:
      return "event";
   case NODE_PERSON:
      return "person";
   case NODE_PLACE:
      return "place";
   case NODE_TIME_EXPR:
      return "time_expr";
   case NODE_ORG:
      return "org";
   case NODE_DEVICE:
      return "device";
   case NODE_IP:
      return "ip";
   case NODE_SCALAR:
      return "scalar";
   case NODE_OTHER:
      return "other";
   default:
      return "unknown";
   }
}

int db2_memory_provenance_by_id(int64_t memory_id, char *kind_out, int kind_len, char *source_out,
                                int source_len, char *version_out, int version_len)
{
   (void)memory_id;
   if (kind_out && kind_len > 0)
      kind_out[0] = '\0';
   if (source_out && source_len > 0)
      source_out[0] = '\0';
   if (version_out && version_len > 0)
      version_out[0] = '\0';
   return 0;
}

int64_t db2_memory_count(void)
{
   return 0;
}

int db2_memory_count_l2(void)
{
   return 0;
}

int db2_memory_count_l3(void)
{
   return 0;
}

int db2_memory_count_orphaned_l0(void)
{
   return 0;
}
