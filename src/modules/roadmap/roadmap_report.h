/*
 * roadmap_report.h: HTML progress report generation for spec-driven roadmaps.
 */

#ifndef DEC_ROADMAP_REPORT_H
#define DEC_ROADMAP_REPORT_H 1

#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

   /* Generate an HTML progress report for roadmap_id and write it to
    * .aimee/roadmap/reports/<roadmap_id>.html (relative to cwd). Creates
    * the directory if absent. Returns 0 on success, -1 on error. */
   int roadmap_report_write(const char *roadmap_id);

   /* Return the path where roadmap_report_write writes the report.
    * Writes into buf (len bytes). Returns buf on success, NULL on error. */
   char *roadmap_report_path(const char *roadmap_id, char *buf, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* DEC_ROADMAP_REPORT_H */
