/* db2_bounded_text.h — bounded, libc-free string primitives for DB2.
 *
 * DB2's link closure is ratcheted: a translation unit that does not already
 * depend on a libc string function must not start doing so, because every such
 * reference is debt the module has to carry when it is packaged as a standalone
 * process. These four cover everything the intent grammar and the record
 * validators need, and each is a plain loop, so a unit that uses them emits no
 * undefined symbol at all.
 *
 * They are bounded on purpose. Every caller is validating a fixed-size record
 * field or an already length-checked string, so a cap is always available and an
 * unbounded scan would be a bug waiting for a missing terminator.
 *
 * No includes beyond stddef.h: this header is pulled in from outside DB2 too. */
#ifndef AIMEE_DB2_BOUNDED_TEXT_H
#define AIMEE_DB2_BOUNDED_TEXT_H

#include <stddef.h>

/* Length of `s`, scanning at most `cap` bytes. Returns `cap` when no NUL is
 * found, which is how callers detect an unterminated fixed-size field. */
static inline size_t db2_bounded_len(const char *s, size_t cap)
{
   size_t n = 0;
   while (n < cap && s[n])
      ++n;
   return n;
}

/* Whole-string equality against a NUL-terminated literal. */
static inline int db2_bounded_equals(const char *s, const char *literal)
{
   size_t i = 0;
   while (s[i] && literal[i] && s[i] == literal[i])
      ++i;
   return s[i] == 0 && literal[i] == 0;
}

/* Whether `s` begins with `prefix`. */
static inline int db2_bounded_prefix(const char *s, const char *prefix)
{
   size_t i = 0;
   while (prefix[i])
   {
      if (s[i] != prefix[i])
         return 0;
      ++i;
   }
   return 1;
}

/* First occurrence of `c` in `s`, or NULL. `c` must not be NUL. */
static inline const char *db2_bounded_find(const char *s, char c)
{
   for (size_t i = 0; s[i]; ++i)
      if (s[i] == c)
         return s + i;
   return NULL;
}

#endif /* AIMEE_DB2_BOUNDED_TEXT_H */
