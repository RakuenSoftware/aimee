/* Descriptor-owned ABI for DB2 model-catalog value validation. */
#ifndef AIMEE_DB2_SUPPORT_MODEL_VALIDATION_H
#define AIMEE_DB2_SUPPORT_MODEL_VALIDATION_H

#ifdef AIMEE_DB2_MODEL_VALIDATION_PREFIX
#define kb_models_endpoint_valid db2_support_models_endpoint_valid
#define kb_models_name_clean     db2_support_models_name_clean
#define kb_models_wire_valid     db2_support_models_wire_valid
#endif

int kb_models_wire_valid(const char *wire);
int kb_models_name_clean(const char *value, int max);
int kb_models_endpoint_valid(const char *endpoint, int max);

#endif
