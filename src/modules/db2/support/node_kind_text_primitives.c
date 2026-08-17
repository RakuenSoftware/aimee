#include "db2_node_kind_text.h"

const char *memory_ontology_node_kind_to_text(int kind)
{
   switch (kind)
   {
   case DB2_NODE_FILE:
      return "file";
   case DB2_NODE_FUNCTION:
      return "function";
   case DB2_NODE_STRUCT:
      return "struct";
   case DB2_NODE_MODULE:
      return "module";
   case DB2_NODE_BUG:
      return "bug";
   case DB2_NODE_COMMIT:
      return "commit";
   case DB2_NODE_PR:
      return "pr";
   case DB2_NODE_DEVELOPER:
      return "developer";
   case DB2_NODE_CONCEPT:
      return "concept";
   case DB2_NODE_EVENT:
      return "event";
   case DB2_NODE_PERSON:
      return "person";
   case DB2_NODE_PLACE:
      return "place";
   case DB2_NODE_TIME_EXPR:
      return "time_expr";
   case DB2_NODE_DEVICE:
      return "device";
   case DB2_NODE_ORG:
      return "org";
   case DB2_NODE_IP:
      return "ip";
   case DB2_NODE_SCALAR:
      return "scalar";
   }
   return "other";
}
