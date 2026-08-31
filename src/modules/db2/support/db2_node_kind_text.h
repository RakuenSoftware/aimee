#ifndef AIMEE_DB2_NODE_KIND_TEXT_H
#define AIMEE_DB2_NODE_KIND_TEXT_H

/* Descriptor-owned numeric ABI for persisted memory node kinds. */
enum
{
   DB2_NODE_FILE = 0,
   DB2_NODE_FUNCTION = 1,
   DB2_NODE_STRUCT = 2,
   DB2_NODE_MODULE = 3,
   DB2_NODE_BUG = 4,
   DB2_NODE_COMMIT = 5,
   DB2_NODE_PR = 6,
   DB2_NODE_DEVELOPER = 7,
   DB2_NODE_CONCEPT = 8,
   DB2_NODE_EVENT = 9,
   DB2_NODE_PERSON = 10,
   DB2_NODE_PLACE = 11,
   DB2_NODE_TIME_EXPR = 12,
   DB2_NODE_DEVICE = 13,
   DB2_NODE_ORG = 14,
   DB2_NODE_IP = 15,
   DB2_NODE_SCALAR = 16,
   DB2_NODE_OTHER = 99
};

const char *memory_ontology_node_kind_to_text(int kind);

#endif
