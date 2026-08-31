/* economizer_wire_snapshot.h: immutable provider-body fence.
 *
 * The initial production registry is signed and empty, so this API can only
 * freeze pristine bytes.  Candidate ownership and proof consumption are
 * intentionally absent; adding either is a separately reviewed change. */
#ifndef DEC_WIRE_FENCE_H
#define DEC_WIRE_FENCE_H 1

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

   typedef enum
   {
      WIRE_FENCE_OPENAI_CHAT = 1,
      WIRE_FENCE_OPENAI_RESPONSES,
      WIRE_FENCE_ANTHROPIC_MESSAGES
   } wire_fence_route_t;

   typedef struct wire_fence wire_fence_t;

   typedef struct
   {
      const uint8_t *data;
      size_t len;
   } wire_fence_bytes_t;

   /* Copy one completed provider-specific body into immutable storage.  The
    * signed production registry must be valid and empty.  On any failure no
    * snapshot is returned and the caller must not dispatch. */
   int wire_fence_create(wire_fence_route_t route, const void *pristine, size_t pristine_len,
                         wire_fence_t **out);

   /* OFF bypasses allocation and registry work, returning the caller's pristine
    * bytes directly. PROOF_GATED creates one immutable snapshot. */
   int wire_fence_select(int proof_gated, wire_fence_route_t route, const void *pristine,
                         size_t pristine_len, wire_fence_t **snapshot,
                         wire_fence_bytes_t *selected);

   wire_fence_route_t econ_wire_snapshot_route(const wire_fence_t *snapshot);
   wire_fence_bytes_t wire_fence_bytes(const wire_fence_t *snapshot);
   void wire_fence_destroy(wire_fence_t *snapshot);

#ifdef __cplusplus
}
#endif

#endif /* DEC_WIRE_FENCE_H */
