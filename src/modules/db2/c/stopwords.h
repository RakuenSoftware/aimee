/* db2/stopwords.h: promoted stopwords table — DB2 subsystem.
 *
 * Pure domain API. No backend types or handles in any signature. */
#ifndef DEC_DB2_STOPWORDS_H
#define DEC_DB2_STOPWORDS_H 1

#ifdef __cplusplus
extern "C"
{
#endif

   /* Load up to `max` stopwords into `out`, where each row is a fixed
    * 32-byte buffer. Returns the number of rows written. Each row is
    * NUL-terminated. */
   int db2_stopwords_list(char out[][32], int max);

#ifdef __cplusplus
}
#endif

#endif /* DEC_DB2_STOPWORDS_H */
