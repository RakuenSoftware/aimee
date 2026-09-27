/* kb_store/stopwords.h: promoted stopwords table — KB_STORE subsystem.
 *
 * Pure domain API. No backend types or handles in any signature. */
#ifndef DEC_KB_STORE_STOPWORDS_H
#define DEC_KB_STORE_STOPWORDS_H 1

#ifdef __cplusplus
extern "C"
{
#endif

   /* Load up to `max` stopwords into `out`, where each row is a fixed
    * 32-byte buffer. Returns the number of rows written. Each row is
    * NUL-terminated. */
   int kb_store_stopwords_list(char out[][32], int max);

#ifdef __cplusplus
}
#endif

#endif /* DEC_KB_STORE_STOPWORDS_H */
