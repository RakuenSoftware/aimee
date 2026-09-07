#ifndef AIMEE_MEMORY_PII_PROVIDER_H
#define AIMEE_MEMORY_PII_PROVIDER_H

enum
{
   DB2_PII_CLASSIFIER_SENS_NORMAL = 0,
   DB2_PII_CLASSIFIER_SENS_PII = 1,
   DB2_PII_CLASSIFIER_SENS_SECRET = 2
};

typedef int (*db2_memory_pii_turn_classifier_fn)(const char *turn_text, int *requests_sensitive);
typedef int (*db2_memory_pii_sensitivity_batch_fn)(const char *const *rel_types, int count,
                                                   int *out);

void memory_pii_register_turn_classifier(db2_memory_pii_turn_classifier_fn classifier);
int memory_pii_turn_requests_sensitive(const char *turn_text);
int memory_pii_rel_sensitivity(const char *rel_type);
void memory_pii_register_sensitivity_batch(db2_memory_pii_sensitivity_batch_fn classifier);
int memory_pii_rel_sensitivity_batch(const char *const *rel_types, int count, int *out);

#endif
