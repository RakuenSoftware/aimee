#ifndef AIMEE_DB2_PII_INJECT_GATE_H
#define AIMEE_DB2_PII_INJECT_GATE_H

/* Descriptor-owned numeric ABI for DB2's pre-injection sensitivity gate. */
enum
{
   DB2_PII_SENS_NORMAL = 0,
   DB2_PII_SENS_PII = 1,
   DB2_PII_SENS_SECRET = 2
};

#define DB2_PII_GATE_CONFIDENCE_FLOOR 0.4

int memory_pii_should_inject(int sensitivity, double confidence, int turn_requests_sensitive);

#endif
