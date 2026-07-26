/* platform_process.c: process spawning, exec, and signal handling (Windows) */
#include "platform_process.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

static char *quote_win_arg(const char *arg)
{
   size_t len = strlen(arg);
   int needs_quotes = (len == 0);
   for (size_t i = 0; i < len; i++)
   {
      if (arg[i] == ' ' || arg[i] == '\t' || arg[i] == '"')
      {
         needs_quotes = 1;
         break;
      }
   }

   size_t cap = len * 2 + 3;
   char *out = malloc(cap);
   if (!out)
      return NULL;

   char *p = out;
   if (!needs_quotes)
   {
      memcpy(out, arg, len + 1);
      return out;
   }

   *p++ = '"';
   size_t backslashes = 0;
   for (size_t i = 0; i < len; i++)
   {
      if (arg[i] == '\\')
      {
         backslashes++;
         continue;
      }
      if (arg[i] == '"')
      {
         while (backslashes-- > 0)
            *p++ = '\\';
         *p++ = '\\';
         *p++ = '"';
         backslashes = 0;
         continue;
      }
      while (backslashes-- > 0)
         *p++ = '\\';
      *p++ = arg[i];
   }
   while (backslashes-- > 0)
      *p++ = '\\';
   *p++ = '"';
   *p = '\0';
   return out;
}

static char *build_command_line(const char *const argv[])
{
   size_t cap = 256;
   size_t len = 0;
   char *cmdline = malloc(cap);
   if (!cmdline)
      return NULL;
   cmdline[0] = '\0';

   for (size_t i = 0; argv && argv[i]; i++)
   {
      char *quoted = quote_win_arg(argv[i]);
      if (!quoted)
      {
         free(cmdline);
         return NULL;
      }
      size_t qlen = strlen(quoted);
      if (len + qlen + 2 > cap)
      {
         while (len + qlen + 2 > cap)
            cap *= 2;
         char *tmp = realloc(cmdline, cap);
         if (!tmp)
         {
            free(quoted);
            free(cmdline);
            return NULL;
         }
         cmdline = tmp;
      }
      if (len > 0)
         cmdline[len++] = ' ';
      memcpy(cmdline + len, quoted, qlen);
      len += qlen;
      cmdline[len] = '\0';
      free(quoted);
   }

   return cmdline;
}

pid_t platform_spawn_daemon(const char *const argv[])
{
   if (!argv || !argv[0])
      return -1;

   char *cmdline = build_command_line(argv);
   if (!cmdline)
      return -1;

   HANDLE nul = CreateFileA("NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                            NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
   if (nul == INVALID_HANDLE_VALUE)
   {
      free(cmdline);
      return -1;
   }

   STARTUPINFOA si;
   PROCESS_INFORMATION pi;
   ZeroMemory(&si, sizeof(si));
   ZeroMemory(&pi, sizeof(pi));
   si.cb = sizeof(si);
   si.dwFlags = STARTF_USESTDHANDLES;
   si.hStdInput = nul;
   si.hStdOutput = nul;
   si.hStdError = nul;

   BOOL ok = CreateProcessA(NULL, cmdline, NULL, NULL, TRUE, DETACHED_PROCESS | CREATE_NO_WINDOW,
                            NULL, NULL, &si, &pi);
   CloseHandle(nul);
   free(cmdline);
   if (!ok)
      return -1;

   pid_t pid = (pid_t)pi.dwProcessId;
   CloseHandle(pi.hThread);
   CloseHandle(pi.hProcess);
   return pid;
}

int platform_exec_capture_cancellable(const char *cmd, char **out, size_t *out_len, int timeout_ms,
                                      platform_process_cancel_fn_t cancel_fn, void *cancel_ctx)
{
   if (!cmd || !out)
      return -1;
   *out = NULL;
   if (out_len)
      *out_len = 0;

   SECURITY_ATTRIBUTES sa;
   ZeroMemory(&sa, sizeof(sa));
   sa.nLength = sizeof(sa);
   sa.bInheritHandle = TRUE;

   HANDLE read_pipe = NULL, write_pipe = NULL;
   if (!CreatePipe(&read_pipe, &write_pipe, &sa, 0))
      return -1;
   SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0);

   size_t full_len = strlen(cmd) + 16;
   char *cmdline = malloc(full_len);
   if (!cmdline)
   {
      CloseHandle(read_pipe);
      CloseHandle(write_pipe);
      return -1;
   }
   snprintf(cmdline, full_len, "cmd.exe /c %s", cmd);

   STARTUPINFOA si;
   PROCESS_INFORMATION pi;
   ZeroMemory(&si, sizeof(si));
   ZeroMemory(&pi, sizeof(pi));
   si.cb = sizeof(si);
   si.dwFlags = STARTF_USESTDHANDLES;
   si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
   si.hStdOutput = write_pipe;
   si.hStdError = write_pipe;

   BOOL ok =
       CreateProcessA(NULL, cmdline, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
   free(cmdline);
   CloseHandle(write_pipe);
   if (!ok)
   {
      CloseHandle(read_pipe);
      return -1;
   }

   size_t cap = 4096;
   size_t len = 0;
   char *buf = malloc(cap);
   if (!buf)
   {
      TerminateProcess(pi.hProcess, 1);
      CloseHandle(pi.hThread);
      CloseHandle(pi.hProcess);
      CloseHandle(read_pipe);
      return -1;
   }

   for (;;)
   {
      if (len + 1024 > cap)
      {
         cap *= 2;
         char *tmp = realloc(buf, cap);
         if (!tmp)
         {
            free(buf);
            TerminateProcess(pi.hProcess, 1);
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            CloseHandle(read_pipe);
            return -1;
         }
         buf = tmp;
      }

      DWORD avail = 0;
      if (!PeekNamedPipe(read_pipe, NULL, 0, NULL, &avail, NULL))
      {
         DWORD err = GetLastError();
         if (err == ERROR_BROKEN_PIPE)
            break;
      }

      if (avail > 0)
      {
         DWORD to_read = (DWORD)((cap - len - 1) < avail ? (cap - len - 1) : avail);
         DWORD nread = 0;
         if (!ReadFile(read_pipe, buf + len, to_read, &nread, NULL))
         {
            DWORD err = GetLastError();
            if (err == ERROR_BROKEN_PIPE)
               break;
            free(buf);
            TerminateProcess(pi.hProcess, 1);
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            CloseHandle(read_pipe);
            return -1;
         }
         len += (size_t)nread;
         continue;
      }

      DWORD wait_ms = 50;
      if (timeout_ms == 0)
         wait_ms = INFINITE;
      if (cancel_fn)
         wait_ms = 50;

      DWORD wait_rc = WaitForSingleObject(pi.hProcess, wait_ms);
      if (wait_rc == WAIT_OBJECT_0)
      {
         for (;;)
         {
            DWORD nread = 0;
            if (!ReadFile(read_pipe, buf + len, (DWORD)(cap - len - 1), &nread, NULL))
            {
               DWORD err = GetLastError();
               if (err == ERROR_BROKEN_PIPE)
                  break;
               free(buf);
               CloseHandle(pi.hThread);
               CloseHandle(pi.hProcess);
               CloseHandle(read_pipe);
               return -1;
            }
            if (nread == 0)
               break;
            len += (size_t)nread;
            if (len + 1024 > cap)
            {
               cap *= 2;
               char *tmp = realloc(buf, cap);
               if (!tmp)
               {
                  free(buf);
                  CloseHandle(pi.hThread);
                  CloseHandle(pi.hProcess);
                  CloseHandle(read_pipe);
                  return -1;
               }
               buf = tmp;
            }
         }
         break;
      }
      if (wait_rc == WAIT_TIMEOUT && timeout_ms > 0)
      {
         TerminateProcess(pi.hProcess, 1);
         WaitForSingleObject(pi.hProcess, INFINITE);
         break;
      }
      if (cancel_fn && cancel_fn(cancel_ctx))
      {
         TerminateProcess(pi.hProcess, 1);
         WaitForSingleObject(pi.hProcess, INFINITE);
         break;
      }
      if (wait_rc == WAIT_FAILED)
      {
         free(buf);
         CloseHandle(pi.hThread);
         CloseHandle(pi.hProcess);
         CloseHandle(read_pipe);
         return -1;
      }
   }

   DWORD exit_code = 0;
   if (!GetExitCodeProcess(pi.hProcess, &exit_code))
      exit_code = (DWORD)-1;

   buf[len] = '\0';
   *out = buf;
   if (out_len)
      *out_len = len;

   CloseHandle(pi.hThread);
   CloseHandle(pi.hProcess);
   CloseHandle(read_pipe);

   return (int)exit_code;
}

int platform_exec_capture(const char *cmd, char **out, size_t *out_len, int timeout_ms)
{
   return platform_exec_capture_cancellable(cmd, out, out_len, timeout_ms, NULL, NULL);
}

/* Bounded subprocess execution (Windows). Mirrors the POSIX contract in
 * platform_process.h: one monotonic deadline across write/read/wait, a cap on
 * captured output, descendant containment, and no orphaned processes.
 *
 * The three defects fixed here are the same ones the POSIX side had: writing all
 * input before reading any output (deadlocks once both directions exceed pipe
 * capacity), an unbounded wait, and a buffer doubled with no ceiling.
 *
 * Containment uses a Job Object with KILL_ON_JOB_CLOSE, which is the Windows
 * equivalent of the POSIX process group: closing the job kills the child AND its
 * descendants, so a grandchild cannot survive holding the pipe.
 *
 * NOT VERIFIED ON WINDOWS. The author had no Windows host; this is written to the
 * documented API contracts and reviewed by reading, and the POSIX side is the one
 * covered by unit-test-exec-pipe-bounds. Treat it as unproven until exercised on
 * a Windows runner.
 */
int platform_exec_pipe_bounded(const char *cmd, const char *input, size_t input_len, char **out,
                               size_t *out_len, int timeout_ms, size_t max_output)
{
   if (out)
      *out = NULL;
   if (out_len)
      *out_len = 0;
   if (!cmd || !out || timeout_ms <= 0 || max_output == 0)
      return PLATFORM_EXEC_ERR_SPAWN;

   SECURITY_ATTRIBUTES sa;
   ZeroMemory(&sa, sizeof(sa));
   sa.nLength = sizeof(sa);
   sa.bInheritHandle = TRUE;

   HANDLE stdin_rd = NULL, stdin_wr = NULL, stdout_rd = NULL, stdout_wr = NULL;
   if (!CreatePipe(&stdin_rd, &stdin_wr, &sa, 0))
      return PLATFORM_EXEC_ERR_SPAWN;
   if (!CreatePipe(&stdout_rd, &stdout_wr, &sa, 0))
   {
      CloseHandle(stdin_rd);
      CloseHandle(stdin_wr);
      return PLATFORM_EXEC_ERR_SPAWN;
   }
   SetHandleInformation(stdin_wr, HANDLE_FLAG_INHERIT, 0);
   SetHandleInformation(stdout_rd, HANDLE_FLAG_INHERIT, 0);

   /* Non-blocking on our ends: a blocking WriteFile to a full stdin pipe while the
    * child blocks on a full stdout pipe is exactly the deadlock. */
   DWORD nowait = PIPE_NOWAIT;
   SetNamedPipeHandleState(stdin_wr, &nowait, NULL, NULL);
   SetNamedPipeHandleState(stdout_rd, &nowait, NULL, NULL);

   HANDLE job = CreateJobObjectA(NULL, NULL);
   if (job)
   {
      JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli;
      ZeroMemory(&jeli, sizeof(jeli));
      jeli.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
      SetInformationJobObject(job, JobObjectExtendedLimitInformation, &jeli, sizeof(jeli));
   }

   size_t full_len = strlen(cmd) + 16;
   char *cmdline = malloc(full_len);
   if (!cmdline)
   {
      if (job)
         CloseHandle(job);
      CloseHandle(stdin_rd);
      CloseHandle(stdin_wr);
      CloseHandle(stdout_rd);
      CloseHandle(stdout_wr);
      return PLATFORM_EXEC_ERR_SPAWN;
   }
   snprintf(cmdline, full_len, "cmd.exe /c %s", cmd);

   STARTUPINFOA si;
   PROCESS_INFORMATION pi;
   ZeroMemory(&si, sizeof(si));
   ZeroMemory(&pi, sizeof(pi));
   si.cb = sizeof(si);
   si.dwFlags = STARTF_USESTDHANDLES;
   si.hStdInput = stdin_rd;
   si.hStdOutput = stdout_wr;
   si.hStdError = stdout_wr;

   BOOL ok = CreateProcessA(NULL, cmdline, NULL, NULL, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED,
                            NULL, NULL, &si, &pi);
   free(cmdline);
   CloseHandle(stdin_rd);
   CloseHandle(stdout_wr);
   if (!ok)
   {
      if (job)
         CloseHandle(job);
      CloseHandle(stdin_wr);
      CloseHandle(stdout_rd);
      return PLATFORM_EXEC_ERR_SPAWN;
   }
   /* Assign before resuming so descendants are captured from the first instruction. */
   if (job)
      AssignProcessToJobObject(job, pi.hProcess);
   ResumeThread(pi.hThread);

   size_t cap = 4096, len = 0, off = 0;
   char *buf = malloc(cap);
   if (!buf)
   {
      if (job)
         CloseHandle(job); /* KILL_ON_JOB_CLOSE takes the tree down */
      else
         TerminateProcess(pi.hProcess, 1);
      CloseHandle(pi.hThread);
      CloseHandle(pi.hProcess);
      CloseHandle(stdin_wr);
      CloseHandle(stdout_rd);
      return PLATFORM_EXEC_ERR_SPAWN;
   }

   const ULONGLONG deadline = GetTickCount64() + (ULONGLONG)timeout_ms;
   int in_open = 1, timed_out = 0, limit_hit = 0, child_done = 0;
   if (!input || input_len == 0)
   {
      CloseHandle(stdin_wr);
      in_open = 0;
   }

   for (;;)
   {
      if (GetTickCount64() >= deadline)
      {
         timed_out = 1;
         break;
      }

      if (in_open)
      {
         DWORD written = 0;
         if (WriteFile(stdin_wr, input + off, (DWORD)(input_len - off), &written, NULL))
         {
            off += written;
            if (off >= input_len)
            {
               CloseHandle(stdin_wr);
               in_open = 0;
            }
         }
         else if (GetLastError() != ERROR_NO_DATA)
         {
            CloseHandle(stdin_wr);
            in_open = 0;
         }
      }

      DWORD avail = 0;
      if (PeekNamedPipe(stdout_rd, NULL, 0, NULL, &avail, NULL) && avail > 0)
      {
         if (len + 1024 > cap)
         {
            size_t ncap = cap * 2;
            if (ncap > max_output + 1)
               ncap = max_output + 1;
            if (ncap <= cap)
            {
               limit_hit = 1;
               break;
            }
            char *tmp = realloc(buf, ncap);
            if (!tmp)
            {
               limit_hit = 1;
               break;
            }
            buf = tmp;
            cap = ncap;
         }
         DWORD nread = 0;
         if (ReadFile(stdout_rd, buf + len, (DWORD)(cap - len - 1), &nread, NULL) && nread > 0)
         {
            len += nread;
            if (len >= max_output)
            {
               limit_hit = 1;
               break;
            }
            continue;
         }
      }

      if (child_done)
         break; /* drained after exit */
      DWORD w = WaitForSingleObject(pi.hProcess, 5);
      if (w == WAIT_OBJECT_0)
         child_done = 1; /* one more pass to drain the pipe */
   }

   if (in_open)
      CloseHandle(stdin_wr);

   DWORD code = 0;
   int rc;
   if (timed_out || limit_hit)
   {
      if (job)
         CloseHandle(job); /* kills the whole tree */
      else
         TerminateProcess(pi.hProcess, 1);
      WaitForSingleObject(pi.hProcess, 2000);
      rc = timed_out ? PLATFORM_EXEC_ERR_TIMEOUT : PLATFORM_EXEC_ERR_OUTPUT_LIMIT;
   }
   else
   {
      ULONGLONG now = GetTickCount64();
      DWORD remaining = now >= deadline ? 0 : (DWORD)(deadline - now);
      if (WaitForSingleObject(pi.hProcess, remaining) != WAIT_OBJECT_0)
      {
         if (job)
            CloseHandle(job);
         else
            TerminateProcess(pi.hProcess, 1);
         WaitForSingleObject(pi.hProcess, 2000);
         rc = PLATFORM_EXEC_ERR_TIMEOUT;
      }
      else
      {
         GetExitCodeProcess(pi.hProcess, &code);
         rc = (int)code;
         if (job)
            CloseHandle(job);
      }
   }

   CloseHandle(pi.hThread);
   CloseHandle(pi.hProcess);
   CloseHandle(stdout_rd);

   buf[len] = '\0';
   if (rc >= 0)
   {
      *out = buf;
      if (out_len)
         *out_len = len;
   }
   else
   {
      free(buf);
   }
   return rc;
}

int platform_exec_pipe(const char *cmd, const char *input, size_t input_len, char **out,
                       size_t *out_len)
{
   return platform_exec_pipe_bounded(cmd, input, input_len, out, out_len,
                                     PLATFORM_EXEC_DEFAULT_TIMEOUT_MS,
                                     PLATFORM_EXEC_DEFAULT_MAX_OUTPUT);
}

int platform_get_exe_path(char *buf, size_t size)
{
   if (!buf || size == 0)
      return -1;
   DWORD len = GetModuleFileNameA(NULL, buf, (DWORD)size);
   if (len == 0 || len >= size)
      return -1;
   return 0;
}

int platform_setenv(const char *name, const char *value)
{
   if (!name)
      return -1;
   return SetEnvironmentVariableA(name, value ? value : "") ? 0 : -1;
}

unsigned int platform_getuid(void)
{
   return 0; /* No direct equivalent on Windows */
}

static platform_signal_handler_t g_term_handler = NULL;
static platform_signal_handler_t g_int_handler = NULL;
static LONG g_console_handler_installed = 0;

static BOOL WINAPI aimee_console_handler(DWORD ctrl_type)
{
   if ((ctrl_type == CTRL_C_EVENT || ctrl_type == CTRL_BREAK_EVENT) && g_int_handler)
   {
      g_int_handler(2);
      return TRUE;
   }
   if ((ctrl_type == CTRL_CLOSE_EVENT || ctrl_type == CTRL_SHUTDOWN_EVENT ||
        ctrl_type == CTRL_LOGOFF_EVENT) &&
       g_term_handler)
   {
      g_term_handler(15);
      return TRUE;
   }
   return FALSE;
}

static void ensure_console_handler(void)
{
   if (InterlockedCompareExchange(&g_console_handler_installed, 1, 0) == 0)
      SetConsoleCtrlHandler(aimee_console_handler, TRUE);
}

void platform_signal_term(platform_signal_handler_t handler)
{
   g_term_handler = handler;
   ensure_console_handler();
}

void platform_signal_int(platform_signal_handler_t handler)
{
   g_int_handler = handler;
   ensure_console_handler();
}

int platform_signal_send_term(int pid)
{
   HANDLE proc = OpenProcess(PROCESS_TERMINATE, FALSE, (DWORD)pid);
   if (!proc)
      return -1;
   BOOL ok = TerminateProcess(proc, 1);
   CloseHandle(proc);
   return ok ? 0 : -1;
}
