/* Strict, deterministic JSON whitespace compaction for authenticated local tool output. */
#ifndef DEC_ECONOMIZER_JSON_H
#define DEC_ECONOMIZER_JSON_H 1

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

   typedef enum
   {
      ECON_JSON_OK = 0,
      ECON_JSON_INVALID_ARGUMENT,
      ECON_JSON_TOO_LARGE,
      ECON_JSON_TOO_DEEP,
      ECON_JSON_INVALID_UTF8,
      ECON_JSON_INVALID_SYNTAX,
      ECON_JSON_DUPLICATE_KEY,
      ECON_JSON_NOT_SHORTER,
      ECON_JSON_NO_MEMORY
   } econ_json_result_t;

#define ECON_JSON_MAX_INPUT          (16u * 1024u * 1024u)
#define ECON_JSON_MAX_DEPTH          64u
#define ECON_JSON_MAX_OBJECT_MEMBERS 65536u

   /* Remove only RFC 8259 whitespace outside strings. Every non-whitespace
    * source byte is retained in source order. The input must be one complete,
    * strict JSON value and may not contain duplicate decoded object names. */
   econ_json_result_t econ_json_compact(const void *input, size_t input_len, uint8_t **output,
                                        size_t *output_len);
   const char *econ_json_result_str(econ_json_result_t result);

#ifdef __cplusplus
}
#endif

#endif
