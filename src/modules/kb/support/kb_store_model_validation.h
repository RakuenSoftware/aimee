/* Descriptor-owned ABI for KB_STORE model-catalog value validation. */
#ifndef AIMEE_KB_STORE_SUPPORT_MODEL_VALIDATION_H
#define AIMEE_KB_STORE_SUPPORT_MODEL_VALIDATION_H

#ifdef AIMEE_KB_STORE_MODEL_VALIDATION_PREFIX
#define kb_models_endpoint_valid kb_store_support_models_endpoint_valid
#define kb_models_name_clean     kb_store_support_models_name_clean
#define kb_models_wire_valid     kb_store_support_models_wire_valid
#endif

int kb_models_wire_valid(const char *wire);
int kb_models_name_clean(const char *value, int max);
int kb_models_endpoint_valid(const char *endpoint, int max);

#endif
