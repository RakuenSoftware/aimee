#ifndef AIMEE_KB_MANAGEMENT_CERT_STORAGE_H
#define AIMEE_KB_MANAGEMENT_CERT_STORAGE_H

#include <stddef.h>
#include <stdint.h>

typedef enum
{
   KB_MANAGEMENT_STORAGE_OK = 0,
   KB_MANAGEMENT_STORAGE_MISSING,
   KB_MANAGEMENT_STORAGE_UNAVAILABLE,
   KB_MANAGEMENT_STORAGE_INTEGRITY,
   KB_MANAGEMENT_STORAGE_CONFLICT
} kb_management_cert_storage_result_t;

typedef struct
{
   int dir_fd;
} kb_management_cert_storage_t;

kb_management_cert_storage_result_t kb_management_cert_storage_open(const char *,
                                                                    kb_management_cert_storage_t *);
void kb_management_cert_storage_close(kb_management_cert_storage_t *);
kb_management_cert_storage_result_t kb_management_cert_storage_stage(kb_management_cert_storage_t *,
                                                                     const char *, const char[65],
                                                                     const void *, size_t);
kb_management_cert_storage_result_t kb_management_cert_storage_read(kb_management_cert_storage_t *,
                                                                    const char *, const char[65],
                                                                    uint8_t *, size_t, size_t *);
kb_management_cert_storage_result_t
kb_management_cert_storage_promote(kb_management_cert_storage_t *, const void *, size_t);
kb_management_cert_storage_result_t
kb_management_cert_storage_current(kb_management_cert_storage_t *, uint8_t *, size_t, size_t *);
/* pending is a single O(1) recovery coordinate, not an immutable operation record. */
kb_management_cert_storage_result_t
kb_management_cert_storage_pending_publish(kb_management_cert_storage_t *, const void *, size_t);
kb_management_cert_storage_result_t
kb_management_cert_storage_pending_read(kb_management_cert_storage_t *, uint8_t *, size_t,
                                        size_t *);
kb_management_cert_storage_result_t
kb_management_cert_storage_pending_clear_exact(kb_management_cert_storage_t *, const void *,
                                               size_t);
/* Durable, storage-owned O(1) cleanup coordination. Prepare derives every
 * operation and digest from checked current/pending/immutable records. */
kb_management_cert_storage_result_t
kb_management_cert_storage_cleanup_prepare_promotion(kb_management_cert_storage_t *);
kb_management_cert_storage_result_t
kb_management_cert_storage_cleanup_prepare_terminal(kb_management_cert_storage_t *);
kb_management_cert_storage_result_t
kb_management_cert_storage_cleanup_prepare_intent(kb_management_cert_storage_t *, const void *,
                                                  size_t);
kb_management_cert_storage_result_t
kb_management_cert_storage_cleanup_finish_intent(kb_management_cert_storage_t *, const void *,
                                                 size_t);
kb_management_cert_storage_result_t
kb_management_cert_storage_cleanup_apply(kb_management_cert_storage_t *);

#endif
