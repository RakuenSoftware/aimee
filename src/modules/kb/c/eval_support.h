/* eval_support.h: DB2-owned scratch store lifecycle for eval/benchmark code. */
#ifndef DEC_DB2_EVAL_SUPPORT_H
#define DEC_DB2_EVAL_SUPPORT_H 1

#ifdef __cplusplus
extern "C"
{
#endif

   int db2_eval_open_temp_store(void);
   void db2_eval_close_temp_store(void);

#ifdef __cplusplus
}
#endif

#endif /* DEC_DB2_EVAL_SUPPORT_H */
