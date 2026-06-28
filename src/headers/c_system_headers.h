#ifndef AIMEE_C_SYSTEM_HEADERS_H
#define AIMEE_C_SYSTEM_HEADERS_H 1

#include <stddef.h>

/* Single source of truth for C/C++ standard-library + common system/SDK headers
 * that are NEVER a cross-repo dependency (precision-hardening H6/H7). Shared,
 * data-only header (no link dependency) so the two consumers cannot drift:
 *   - the extractor (extractors_extra.c c_import_line) drops a BARE angle include
 *     `<name>` matching this set at extraction (keeps file_imports lean, prevents
 *     the import buffer being exhausted by system headers, removes the bare-system
 *     angle-collision FP class — e.g. <process.h> vs a repo's own process.h);
 *   - the resolver (cross_repo_resolver.c) rejects the same on the import-route
 *     path. Both call aimee_c_system_header_is().
 *
 * Matching is on the FULL include string (so a PATH-QUALIFIED `<thirdparty/string.h>`
 * is a real lib header and is KEPT — only bare stdlib names match) and
 * CASE-INSENSITIVE (Windows is case-insensitive: `<Windows.h>` == `<windows.h>`).
 * `static` so each of the (few) including TUs gets its own copy — no ODR/link issue. */
static const char *const AIMEE_C_SYSTEM_HEADERS[] = {
    /* C standard library */
    "stdio.h",
    "stdlib.h",
    "string.h",
    "stddef.h",
    "stdint.h",
    "stdbool.h",
    "stdarg.h",
    "ctype.h",
    "errno.h",
    "math.h",
    "time.h",
    "assert.h",
    "limits.h",
    "wchar.h",
    "wctype.h",
    "locale.h",
    "setjmp.h",
    "inttypes.h",
    "complex.h",
    "iso646.h",
    "stdalign.h",
    "stdnoreturn.h",
    "uchar.h",
    "stdatomic.h",
    "threads.h",
    "float.h",
    "fenv.h",
    "tgmath.h",
    /* POSIX / unix */
    "unistd.h",
    "fcntl.h",
    "signal.h",
    "pthread.h",
    "dlfcn.h",
    "sched.h",
    "semaphore.h",
    "poll.h",
    "termios.h",
    "syslog.h",
    "malloc.h",
    "alloca.h",
    "strings.h",
    "memory.h",
    /* C++ standard library */
    "memory",
    "vector",
    "string",
    "string_view",
    "cstdio",
    "cstdlib",
    "cstring",
    "cstdint",
    "cstddef",
    "cstdarg",
    "cerrno",
    "cctype",
    "climits",
    "map",
    "set",
    "unordered_map",
    "unordered_set",
    "list",
    "deque",
    "queue",
    "stack",
    "algorithm",
    "numeric",
    "ranges",
    "utility",
    "tuple",
    "iostream",
    "sstream",
    "fstream",
    "iomanip",
    "mutex",
    "thread",
    "atomic",
    "condition_variable",
    "future",
    "chrono",
    "filesystem",
    "cassert",
    "cmath",
    "stdexcept",
    "array",
    "functional",
    "optional",
    "variant",
    "type_traits",
    "bitset",
    "span",
    "bit",
    "concepts",
    "coroutine",
    "format",
    "iterator",
    "limits",
    "new",
    "typeinfo",
    "initializer_list",
    "regex",
    "random",
    "ratio",
    "scoped_allocator",
    "shared_mutex",
    "system_error",
    "csignal",
    "cstdbool",
    "cwchar",
    "cwctype",
    "clocale",
    "ctime",
    "exception",
    "forward_list",
    "unordered_multimap",
    "valarray",
    "complex",
    "locale",
    "ostream",
    "istream",
    "streambuf",
    "iosfwd",
    "cfloat",
    "cfenv",
    /* Windows / Win32 SDK (angle includes on _WIN32 code paths; never cross-repo) */
    "windows.h",
    "process.h",
    "io.h",
    "direct.h",
    "conio.h",
    "tchar.h",
    "intrin.h",
    "winsock2.h",
    "ws2tcpip.h",
    "winbase.h",
    "windowsx.h",
    "shellapi.h",
    "shlwapi.h",
    "wincrypt.h",
    "winuser.h",
    "objbase.h",
    "combaseapi.h",
    "synchapi.h",
    "processthreadsapi.h",
    "fileapi.h",
    "handleapi.h",
    "errhandlingapi.h",
    "sysinfoapi.h",
    "timezoneapi.h",
    "psapi.h",
    "winnt.h",
    "minwindef.h",
    "minwinbase.h",
    "basetsd.h",
    "sal.h",
    "strsafe.h",
    "wingdi.h",
    "commctrl.h",
    "commdlg.h",
    "ole2.h",
    "oleauto.h",
    "rpc.h",
    "winreg.h",
    "winnls.h",
    "memoryapi.h",
    "libloaderapi.h",
};

/* 1 if `name` (a full include specifier) is a C/C++ stdlib or common system header.
 * Full-string, case-insensitive match (ASCII). */
static int aimee_c_system_header_is(const char *name)
{
   if (!name || !name[0])
      return 0;
   for (size_t i = 0; i < sizeof(AIMEE_C_SYSTEM_HEADERS) / sizeof(AIMEE_C_SYSTEM_HEADERS[0]); i++)
   {
      const char *a = name;
      const char *b = AIMEE_C_SYSTEM_HEADERS[i];
      while (*a && *b)
      {
         int ca = (*a >= 'A' && *a <= 'Z') ? *a + 32 : (unsigned char)*a;
         int cb = (*b >= 'A' && *b <= 'Z') ? *b + 32 : (unsigned char)*b;
         if (ca != cb)
            break;
         a++;
         b++;
      }
      if (!*a && !*b)
         return 1;
   }
   return 0;
}

#endif /* AIMEE_C_SYSTEM_HEADERS_H */
