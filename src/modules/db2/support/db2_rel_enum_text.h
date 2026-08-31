#ifndef AIMEE_DB2_REL_ENUM_TEXT_H
#define AIMEE_DB2_REL_ENUM_TEXT_H

/* Descriptor-owned numeric ABI for DB2's relationship table text columns. */
enum
{
   DB2_CORR_SUPERSEDE = 0,
   DB2_CORR_HARD_DELETE = 1,
   DB2_CORR_IMMUTABLE = 2,
   DB2_SENS_NORMAL = 0,
   DB2_SENS_PII = 1,
   DB2_SENS_SECRET = 2
};

const char *correction_behavior_to_text(int behavior);
const char *rel_sensitivity_to_text(int sensitivity);

#endif
