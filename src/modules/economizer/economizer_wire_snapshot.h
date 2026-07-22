/* economizer_wire_snapshot.h: immutable provider-body fence.
 *
 * The initial production registry is signed and empty, so this API can only
 * freeze pristine bytes.  Candidate ownership and proof consumption are
 * intentionally absent; adding either is a separately reviewed change. */
#ifndef DEC_ECONOMIZER_WIRE_SNAPSHOT_H
#define DEC_ECONOMIZER_WIRE_SNAPSHOT_H 1

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

   typedef enum
   {
      ECON_WIRE_OPENAI_CHAT = 1,
      ECON_WIRE_OPENAI_RESPONSES,
      ECON_WIRE_ANTHROPIC_MESSAGES
   } econ_wire_route_t;

   typedef struct econ_wire_snapshot econ_wire_snapshot_t;

   typedef struct
   {
      const uint8_t *data;
      size_t len;
   } econ_wire_bytes_t;

   /* Copy one completed provider-specific body into immutable storage.  The
    * signed production registry must be valid and empty.  On any failure no
    * snapshot is returned and the caller must not dispatch. */
   int econ_wire_snapshot_create(econ_wire_route_t route, const void *pristine, size_t pristine_len,
                                 econ_wire_snapshot_t **out);

   /* OFF bypasses allocation and registry work, returning the caller's pristine
    * bytes directly. PROOF_GATED creates one immutable snapshot. */
   int econ_wire_select(int proof_gated, econ_wire_route_t route, const void *pristine,
                        size_t pristine_len, econ_wire_snapshot_t **snapshot,
                        econ_wire_bytes_t *selected);

   econ_wire_route_t econ_wire_snapshot_route(const econ_wire_snapshot_t *snapshot);
   econ_wire_bytes_t econ_wire_snapshot_bytes(const econ_wire_snapshot_t *snapshot);
   void econ_wire_snapshot_destroy(econ_wire_snapshot_t *snapshot);

#ifdef __cplusplus
}
#endif

#endif /* DEC_ECONOMIZER_WIRE_SNAPSHOT_H */
