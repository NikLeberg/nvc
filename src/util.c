//
//  Copyright (C) 2011-2022  Nick Gasson
//
//  This program is free software: you can redistribute it and/or modify
//  it under the terms of the GNU General Public License as published by
//  the Free Software Foundation, either version 3 of the License, or
//  (at your option) any later version.
//
//  This program is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//  GNU General Public License for more details.
//
//  You should have received a copy of the GNU General Public License
//  along with this program.  If not, see <http://www.gnu.org/licenses/>.
//

#if defined(__MINGW32__)
#define WINVER 0x0A00
#define _WIN32_WINNT 0x0A00
#include <windows.h>
#include <DbgHelp.h>
#include <fileapi.h>
#include <psapi.h>
#endif

#include "util.h"
#include "ident.h"
#include "loc.h"
#include "debug.h"
#include "array.h"

#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <errno.h>
#include <string.h>
#include <stdbool.h>
#include <signal.h>
#include <stdint.h>
#include <inttypes.h>
#include <unistd.h>
#include <ctype.h>
#include <assert.h>
#include <limits.h>
#include <time.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#ifdef HAVE_SYS_PTRACE_H
#include <sys/ptrace.h>
#endif
#ifdef __APPLE__
#include <sys/sysctl.h>
#endif
#ifndef __MINGW32__
#include <sys/mman.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <sys/file.h>
#include <fcntl.h>
#endif

#ifdef HAVE_SYS_PRCTL_H
#include <sys/prctl.h>
#endif

#ifdef __CYGWIN__
#include <process.h>
#endif

#if defined(HAVE_UCONTEXT_H)
#include <ucontext.h>
#elif defined(HAVE_SYS_UCONTEXT_H)
#include <sys/ucontext.h>
#endif

#ifdef HAVE_PTHREAD
#include <pthread.h>
#endif

#define N_TRACE_DEPTH   16
#define ERROR_SZ        1024
#define PAGINATE_RIGHT  72
#define TRACE_MAX_LINE  256

#define ANSI_RESET      0
#define ANSI_BOLD       1
#define ANSI_FG_BLACK   30
#define ANSI_FG_RED     31
#define ANSI_FG_GREEN   32
#define ANSI_FG_YELLOW  33
#define ANSI_FG_BLUE    34
#define ANSI_FG_MAGENTA 35
#define ANSI_FG_CYAN    36
#define ANSI_FG_WHITE   37

#define MAX_FMT_BUFS    32
#define MAX_PRINTF_BUFS 8

typedef void (*print_fn_t)(const char *fmt, ...);

static void show_hint(void);
static char *color_vasprintf(const char *fmt, va_list ap, bool force_plain);

typedef struct guard guard_t;
typedef struct option option_t;
typedef struct hint hint_t;

typedef enum {
   OPTION_INT,
   OPTION_STRING
} option_kind_t;

typedef union {
   int   i;
   char *s;
} optval_t;

struct option {
   option_t      *next;
   option_kind_t  kind;
   ident_t        key;
   optval_t       value;
};

struct hint {
   hint_fn_t func;
   char     *str;
   void     *context;
   loc_t     loc;
   hint_t   *next;
};

struct color_escape {
   const char *name;
   int         value;
};

struct guard {
   const char *tag;
   uintptr_t   base;
   uintptr_t   limit;
   guard_t    *next;
};

struct text_buf {
   char  *buf;
   size_t alloc;
   size_t len;
};

struct _nvc_thread {
   unsigned   id;
   char      *name;
#if defined HAVE_PTHREAD
   pthread_t  handle;
#elif defined _WIN32
   HANDLE     handle;
#endif
};

struct _nvc_mutex {
#if defined HAVE_PTHREAD
   pthread_mutex_t mutex;
#endif
};

static error_fn_t      error_fn = NULL;
static fatal_fn_t      fatal_fn = NULL;
static bool            want_color = false;
static option_t       *options = NULL;
static guard_t        *guards;
static message_style_t message_style = MESSAGE_FULL;
static hint_t         *hints = NULL;
static unsigned        n_errors = 0;

static volatile sig_atomic_t crashing = 0;

static const struct color_escape escapes[] = {
   { "",        ANSI_RESET },
   { "bold",    ANSI_BOLD },
   { "black",   ANSI_FG_BLACK },
   { "red",     ANSI_FG_RED },
   { "green",   ANSI_FG_GREEN },
   { "yellow",  ANSI_FG_YELLOW },
   { "blue",    ANSI_FG_BLUE },
   { "magenta", ANSI_FG_MAGENTA },
   { "cyan",    ANSI_FG_CYAN },
   { "white",   ANSI_FG_WHITE },
};

void *xmalloc(size_t size)
{
   void *p = malloc(size);
   if (p == NULL)
      fatal("memory exhausted (malloc %lu)", (long unsigned)size);
   return p;
}

void *xmalloc_flex(size_t fixed, size_t nelems, size_t size)
{
   size_t bytes;
   if (__builtin_mul_overflow(nelems, size, &bytes))
      fatal_trace("array size overflow: requested %zd * %zd bytes",
                  nelems, size);

   return xmalloc(fixed + bytes);
}

void *xmalloc_array(size_t nelems, size_t size)
{
   return xmalloc_flex(0, nelems, size);
}

void *xcalloc(size_t size)
{
   void *p = calloc(1, size);
   if (p == NULL)
      fatal("memory exhausted (calloc %lu)", (long unsigned)size);
   return p;
}

void *xcalloc_flex(size_t fixed, size_t nelems, size_t size)
{
   size_t bytes;
   if (__builtin_mul_overflow(nelems, size, &bytes))
      fatal_trace("array size overflow: requested %zd * %zd bytes",
                  nelems, size);

   return xcalloc(fixed + bytes);
}

void *xcalloc_array(size_t nelems, size_t size)
{
   return xcalloc_flex(0, nelems, size);
}

void *xrealloc(void *ptr, size_t size)
{
   ptr = realloc(ptr, size);
   if (ptr == NULL)
      fatal("memory exhausted (realloc %lu)", (long unsigned)size);
   return ptr;
}

void *xrealloc_array(void *ptr, size_t nelems, size_t size)
{
   size_t bytes;
   if (__builtin_mul_overflow(nelems, size, &bytes))
      fatal_trace("array size overflow: requested %zd * %zd bytes",
                  nelems, size);

   return xrealloc(ptr, bytes);
}

char *xstrdup(const char *str)
{
   char *copy = strdup(str);
   if (copy == NULL)
      fatal("memory exhausted (strdup %p)", str);
   return copy;
}

char *xvasprintf(const char *fmt, va_list ap)
{
   char *strp = NULL;
   if (vasprintf(&strp, fmt, ap) < 0)
      abort();
   return strp;
}

char *xasprintf(const char *fmt, ...)
{
   va_list ap;
   va_start(ap, fmt);
   char *strp = xvasprintf(fmt, ap);
   va_end(ap);
   return strp;
}

static void paginate_msg(text_buf_t *tb, const char *fmt, va_list ap,
                         int start, int left, int right)
{
   char *strp LOCAL = color_vasprintf(fmt, ap, false);

   const char *p = strp, *begin = strp;
   int col = start;
   bool escape = false;
   while (*p != '\0') {
      if ((*p == '\n') || (*p == '\r') || (isspace((int)*p) && col >= right)) {
         // Can break line here
         if (begin < p) tb_catn(tb, begin, p - begin);
         tb_append(tb, '\n');
         if (*p == '\r')
            col = 0;
         else {
            tb_repeat(tb, ' ', left);
            col = left;
         }
         if ((*p == '\n' || *p == '\r') && isspace((int)*(p + 1)))
            right = INT_MAX;    // Don't paginate after leading whitespace
         begin = ++p;
      }
      else {
         if (*p == '\033')
            escape = true;
         else if (escape) {
            if (*p == 'm')
               escape = false;
         }
         else
            ++col;
         ++p;
      }
   }
   if (begin < p) tb_catn(tb, begin, p - begin);
   tb_append(tb, '\n');
}

static void fmt_color(int color, const char *prefix,
                      const char *fmt, va_list ap)
{
   LOCAL_TEXT_BUF tb = tb_new();
   if (want_color)
      tb_printf(tb, "\033[%dm", color);
   if (message_style == MESSAGE_COMPACT)
      tb_printf(tb, "%c%s: ", tolower((int)prefix[0]), prefix + 1);
   else
      tb_printf(tb, "** %s: ", prefix);
   if (want_color)
      tb_printf(tb, "\033[%dm", ANSI_RESET);
   paginate_msg(tb, fmt, ap, strlen(prefix) + 5, 10,
                (message_style == MESSAGE_COMPACT) ? INT_MAX : PAGINATE_RIGHT);

   fputs(tb_get(tb), stderr);
#ifdef __MINGW32__
   fflush(stderr);
#endif
}

void errorf(const char *fmt, ...)
{
   va_list ap;
   va_start(ap, fmt);
   fmt_color(ANSI_FG_RED, "Error", fmt, ap);
   va_end(ap);
   show_hint();
}

void warnf(const char *fmt, ...)
{
   va_list ap;
   va_start(ap, fmt);
   fmt_color(ANSI_FG_YELLOW, "Warning", fmt, ap);
   va_end(ap);
   show_hint();
}

void notef(const char *fmt, ...)
{
   va_list ap;
   va_start(ap, fmt);
   fmt_color(ANSI_RESET, "Note", fmt, ap);
   va_end(ap);
   show_hint();
}

static char *color_vasprintf(const char *fmt, va_list ap, bool force_plain)
{
   // Replace color strings like "$red$foo$$bar" with ANSI escaped
   // strings like "\033[31mfoo\033[0mbar"

   static int override = 0;

   if (strchr(fmt, '$') == NULL)
      return xvasprintf(fmt, ap);

   LOCAL_TEXT_BUF tb = tb_new();
   const char *escape_start = NULL;

   while (*fmt != '\0') {
      if (*fmt == '$') {
         if (escape_start == NULL)
            escape_start = fmt;
         else {
            const char *e = escape_start + 1;
            size_t len = fmt - e;

            bool bold;
            if ((bold = (*e == '!')))
               ++e, --len;

            if ((*e == '<' || *e == '>') && *(e + 1) == '$') {
               override += *e == '<' ? -1 : 1;
               escape_start = NULL;
            }
            else if (want_color && !force_plain && override >= 0) {
               bool found = false;

               if (*e == '#') {
                  char *eptr;
                  int code = strtoul(e + 1, &eptr, 10);
                  if (eptr == e + len) {
                     if (bold)
                        tb_printf(tb, "\033[1;38;5;%dm", code);
                     else
                        tb_printf(tb, "\033[38;5;%dm", code);
                     found = true;
                  }
               }

               for (int i = 0; !found && i < ARRAY_LEN(escapes); i++) {
                  if (strncmp(e, escapes[i].name, len) == 0) {
                     if (bold)
                        tb_printf(tb, "\033[1;%dm", escapes[i].value);
                     else
                        tb_printf(tb, "\033[%dm", escapes[i].value);
                     found = true;
                     break;
                  }
               }

               if (!found) {
                  tb_catn(tb, escape_start, len + 1 + bold);
                  escape_start = fmt;
               }
               else
                  escape_start = NULL;
            }
            else
               escape_start = NULL;
         }
      }
      else if (escape_start == NULL)
         tb_append(tb, *fmt);

      ++fmt;
   }

   if (escape_start != NULL)
      tb_cat(tb, escape_start);

   return xvasprintf(tb_get(tb), ap);
}

static int color_vfprintf(FILE *f, const char *fmt, va_list ap)
{
   char *strp LOCAL = color_vasprintf(fmt, ap, false);

   bool escape = false;
   int len = 0;
   for (const char *p = strp; *p != '\0'; p++) {
      if (*p == '\033')
         escape = true;
      if (escape)
         escape = (*p != 'm');
      else
         len += 1;
   }

   fputs(strp, f);
   return len;
}

int color_fprintf(FILE *f, const char *fmt, ...)
{
   va_list ap;
   va_start(ap, fmt);
   const int len = color_vfprintf(f, fmt, ap);
   va_end(ap);
   return len;
}

int color_printf(const char *fmt, ...)
{
   va_list ap;
   va_start(ap, fmt);
   int rc = color_vprintf(fmt, ap);
   va_end(ap);
   return rc;
}

int color_vprintf(const char *fmt, va_list ap)
{
   return color_vfprintf(stdout, fmt, ap);
}

char *color_asprintf(const char *fmt, ...)
{
   va_list ap;
   va_start(ap, fmt);
   char *str = color_vasprintf(fmt, ap, false);
   va_end(ap);
   return str;
}

static bool catch_in_unit_test(const loc_t *loc, const char *fmt, va_list ap)
{
   if (error_fn != NULL) {
      char *strp LOCAL = color_vasprintf(fmt, ap, true);
      error_fn(strp, loc != NULL ? loc : &LOC_INVALID);
      return true;
   }
   else
      return false;
}

static void default_hint_fn(void *arg)
{
   hint_t *h = arg;
   note_at(&(h->loc), "%s", h->str);
}

static void pop_hint(void)
{
   hint_t *tmp = hints->next;
   free(hints->str);
   free(hints);
   hints = tmp;
}

static void show_hint(void)
{
   static bool inside = false;

   if (inside)
      return;

   inside = true;

   while (hints != NULL) {
      (*hints->func)(hints->context);
      pop_hint();
   }

   inside = false;
}

void set_hint_fn(hint_fn_t fn, void *context)
{
   hint_t *h = xmalloc(sizeof(hint_t));
   h->func = fn;
   h->str = NULL;
   h->context = context;
   h->next = hints;
   h->loc = LOC_INVALID;

   hints = h;
}

void clear_hint(void)
{
   while (hints != NULL)
      pop_hint();
}

void hint_at(const loc_t *loc, const char *fmt, ...)
{
   va_list ap;
   va_start(ap, fmt);

   hint_t *h = xmalloc(sizeof(hint_t));
   h->func = default_hint_fn;
   h->str = color_vasprintf(fmt, ap, false);
   h->context = h;
   h->loc = loc ? *loc : LOC_INVALID;
   h->next = hints;

   va_end(ap);

   hints = h;
}

void error_at(const loc_t *loc, const char *fmt, ...)
{
   va_list ap;
   va_start(ap, fmt);
   if (!catch_in_unit_test(loc, fmt, ap)) {
      if (message_style == MESSAGE_COMPACT)
         fmt_loc(stderr, loc);
      fmt_color(ANSI_FG_RED, "Error", fmt, ap);
      if (message_style == MESSAGE_FULL)
         fmt_loc(stderr, loc);
   }
   show_hint();
   n_errors++;

   va_end(ap);

   if (n_errors == opt_get_int("error-limit"))
      fatal("too many errors, giving up");
}

void warn_at(const loc_t *loc, const char *fmt, ...)
{
   va_list ap;
   va_start(ap, fmt);
   if (!catch_in_unit_test(loc, fmt, ap)) {
      if (message_style == MESSAGE_COMPACT)
         fmt_loc(stderr, loc);
      fmt_color(ANSI_FG_YELLOW, "Warning", fmt, ap);
      if (message_style == MESSAGE_FULL)
         fmt_loc(stderr, loc);
   }
   show_hint();
   va_end(ap);

   if (opt_get_int("unit-test"))
      n_errors++;
}

void note_at(const loc_t *loc, const char *fmt, ...)
{
   va_list ap;
   va_start(ap, fmt);
   if (!catch_in_unit_test(loc, fmt, ap)) {
      if (message_style == MESSAGE_COMPACT)
         fmt_loc(stderr, loc);
      fmt_color(ANSI_RESET, "Note", fmt, ap);
      if (message_style == MESSAGE_FULL)
         fmt_loc(stderr, loc);
   }
   show_hint();
   va_end(ap);

   if (opt_get_int("unit-test"))
      n_errors++;
}

__attribute__((noreturn))
static void fatal_exit(int status)
{
   if (crashing)
      _exit(status);
   else
      exit(status);
}

void fatal_at(const loc_t *loc, const char *fmt, ...)
{
   va_list ap;
   va_start(ap, fmt);
   if (!catch_in_unit_test(loc, fmt, ap)) {
      if (message_style == MESSAGE_COMPACT)
         fmt_loc(stderr, loc);
      fmt_color(ANSI_FG_RED, "Fatal", fmt, ap);
      if (message_style == MESSAGE_FULL)
         fmt_loc(stderr, loc);
   }
   show_hint();
   va_end(ap);

   if (fatal_fn != NULL)
      (*fatal_fn)();

   fatal_exit(EXIT_FAILURE);
}

void set_error_fn(error_fn_t fn)
{
   error_fn = fn;
}

void set_fatal_fn(fatal_fn_t fn)
{
   fatal_fn = fn;
}

void fatal(const char *fmt, ...)
{
   va_list ap;
   va_start(ap, fmt);
   fmt_color(ANSI_FG_RED, "Fatal", fmt, ap);
   show_hint();
   va_end(ap);

   if (fatal_fn != NULL)
      (*fatal_fn)();

   fatal_exit(EXIT_FAILURE);
}

void fatal_trace(const char *fmt, ...)
{
   va_list ap;
   va_start(ap, fmt);
   fmt_color(ANSI_FG_RED, "Fatal", fmt, ap);
   va_end(ap);

   show_stacktrace();

   fatal_exit(EXIT_FAILURE);
}

void fatal_errno(const char *fmt, ...)
{
   char *fmt_err LOCAL = xasprintf("%s: %s", fmt, last_os_error());

   va_list ap;
   va_start(ap, fmt);
   fmt_color(ANSI_FG_RED, "Fatal", fmt_err, ap);
   va_end(ap);

   exit(EXIT_FAILURE);
}

const char *last_os_error(void)
{
#ifdef __MINGW32__
   static __thread LPSTR mbuf = NULL;

   if (mbuf != NULL)
      LocalFree(mbuf);

   FormatMessage(
      FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM
      | FORMAT_MESSAGE_IGNORE_INSERTS,
      NULL,
      GetLastError(),
      MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
      (LPSTR)&mbuf, 0, NULL);

   return mbuf;
#else
   return strerror(errno);
#endif
}

static void trace_one_frame(uintptr_t pc, const char *module,
                            const char *srcfile, const char *symbol,
                            unsigned lineno, unsigned colno,
                            ptrdiff_t disp, frame_kind_t kind)
{
   color_fprintf(stderr, "[$green$%p$$] ", (void *)pc);
   if (kind == FRAME_LIB)
      color_fprintf(stderr, "($red$%s$$) ", module);
   if (srcfile != NULL)
      color_fprintf(stderr, "%s:%d ", srcfile, lineno);
   if (symbol != NULL) {
      color_fprintf(stderr, "$yellow$%s$$", symbol);
      if (srcfile == NULL && disp != 0)
         color_fprintf(stderr, "$yellow$+0x%"PRIxPTR"$$", disp);
   }
   if (kind == FRAME_VHDL)
      color_fprintf(stderr, " $magenta$[VHDL]$$");
   fprintf(stderr, "\n");

   if (srcfile != NULL) {
      FILE *f = fopen(srcfile, "r");
      if (f != NULL) {
         char buf[TRACE_MAX_LINE];
         for (int i = 0; i < lineno + 1 &&
                 fgets(buf, sizeof(buf), f); i++) {
            if (i < lineno - 2)
               continue;

            const size_t len = strlen(buf);
            if (len <= 1)
               continue;
            else if (buf[len - 1] == '\n')
               buf[len - 1] = '\0';

            if (i == lineno - 1)
               color_fprintf(stderr, "$cyan$$bold$-->$$ $cyan$%s$$\n", buf);
            else
               color_fprintf(stderr, "    $cyan$%s$$\n", buf);
         }
         fclose(f);
      }
   }
}

__attribute__((noinline))
void show_stacktrace(void)
{
   debug_info_t *di = debug_capture();

   const int nframes = debug_count_frames(di);
   for (int n = 1; n < nframes; n++) {
      const debug_frame_t *f = debug_get_frame(di, n);

      for (debug_inline_t *inl = f->inlined; inl != NULL; inl = inl->next)
         trace_one_frame(f->pc, f->module, inl->srcfile, inl->symbol,
                         inl->lineno, inl->colno, f->disp, f->kind);

      trace_one_frame(f->pc, f->module, f->srcfile, f->symbol, f->lineno,
                      f->colno, f->disp, f->kind);

   }

   debug_free(di);

#if defined __linux && !defined HAVE_LIBDW && !defined HAVE_LIBDWARF
   color_fprintf(stderr, "\n$cyan$Hint: you can get better stack traces by "
                 "installing the libdw-dev package and reconfiguring$$\n");
#endif
}

#ifndef __SANITIZE_THREAD__
static bool check_guard_page(uintptr_t addr)
{
   for (guard_t *it = guards; it != NULL; it = it->next) {
      if ((addr >= it->base) && (addr < it->limit)) {
         fatal_trace("accessed %d bytes beyond $cyan$%s$$ region",
                     (int)(addr - it->base), it->tag);
      }
   }

   return false;
}
#endif

#ifdef __MINGW32__

static const char *exception_name(DWORD code)
{
   switch (code) {
   case EXCEPTION_ACCESS_VIOLATION:
      return "EXCEPTION_ACCESS_VIOLATION";
   case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
      return "EXCEPTION_ARRAY_BOUNDS_EXCEEDED";
   case EXCEPTION_BREAKPOINT:
      return "EXCEPTION_BREAKPOINT";
   case EXCEPTION_DATATYPE_MISALIGNMENT:
      return "EXCEPTION_DATATYPE_MISALIGNMENT";
   case EXCEPTION_ILLEGAL_INSTRUCTION:
      return "EXCEPTION_ILLEGAL_INSTRUCTION";
   case EXCEPTION_IN_PAGE_ERROR:
      return "EXCEPTION_IN_PAGE_ERROR";
   case EXCEPTION_INT_DIVIDE_BY_ZERO:
      return "EXCEPTION_INT_DIVIDE_BY_ZERO";
   case EXCEPTION_INT_OVERFLOW:
      return "EXCEPTION_INT_OVERFLOW";
   case EXCEPTION_PRIV_INSTRUCTION:
      return "EXCEPTION_PRIV_INSTRUCTION";
   case EXCEPTION_STACK_OVERFLOW:
      return "EXCEPTION_STACK_OVERFLOW";
   }

   return "???";
}

WINAPI
static LONG win32_exception_handler(EXCEPTION_POINTERS *ExceptionInfo)
{
   DWORD code = ExceptionInfo->ExceptionRecord->ExceptionCode;
   PVOID addr = ExceptionInfo->ExceptionRecord->ExceptionAddress;

#ifdef __WIN64
   DWORD64 ip = ExceptionInfo->ContextRecord->Rip;
#else
   DWORD ip = ExceptionInfo->ContextRecord->Eip;
#endif

   if (code == EXCEPTION_ACCESS_VIOLATION) {
      addr = (PVOID)ExceptionInfo->ExceptionRecord->ExceptionInformation[1];
      check_guard_page((uintptr_t)addr);
   }

   color_fprintf(stderr, "\n$red$$bold$*** Caught exception %x (%s)",
                 (int)code, exception_name(code));

   switch (code) {
   case EXCEPTION_ACCESS_VIOLATION:
   case EXCEPTION_ILLEGAL_INSTRUCTION:
      fprintf(stderr, " [address=%p, ip=%p]", (void *)addr, (void*)ip);
      break;
   }

   color_fprintf(stderr, " ***$$\n\n");
   fflush(stderr);

#ifdef __WIN64
   if (code != EXCEPTION_STACK_OVERFLOW)
      show_stacktrace();
#endif

  return EXCEPTION_EXECUTE_HANDLER;
}

#elif !defined __SANITIZE_THREAD__

static const char *signame(int sig)
{
   switch (sig) {
   case SIGSEGV: return "SIGSEGV";
   case SIGABRT: return "SIGABRT";
   case SIGILL: return "SIGILL";
   case SIGFPE: return "SIGFPE";
   case SIGUSR1: return "SIGUSR1";
   case SIGBUS: return "SIGBUS";
   default: return "???";
   }
}

static void signal_handler(int sig, siginfo_t *info, void *context)
{
#if defined HAVE_UCONTEXT_H && defined PC_FROM_UCONTEXT
   ucontext_t *uc = (ucontext_t*)context;
   uintptr_t ip = uc->PC_FROM_UCONTEXT;
#else
   uintptr_t ip = 0;
#endif

   if (sig != SIGUSR1)
      crashing = 1;

   extern void check_frozen_object_fault(void *addr);

   if (sig == SIGSEGV) {
      signal(SIGSEGV, SIG_DFL);
      check_guard_page((uintptr_t)info->si_addr);
      check_frozen_object_fault(info->si_addr);
   }

   color_fprintf(stderr, "\n$red$$bold$*** Caught signal %d (%s)",
                 sig, signame(sig));

   switch (sig) {
   case SIGSEGV:
   case SIGILL:
   case SIGFPE:
   case SIGBUS:
      fprintf(stderr, " [address=%p, ip=%p]", info->si_addr, (void*)ip);
      break;
   }

   color_fprintf(stderr, " ***$$\n\n");
   fflush(stderr);

   show_stacktrace();

   if (sig != SIGUSR1)
      _exit(2);
}

#if defined __linux__
static bool scan_file_for_token(const char *file, const char *token)
{
   bool found = false;
   FILE *f = fopen(file, "r");
   if (f != NULL) {
      char buf[1024];
      while (!found && fgets(buf, sizeof(buf), f)) {
         if (strstr(buf, token))
            found = true;
      }
      fclose(f);
   }

   return found;
}
#endif  // __linux__
#endif  // !__SANITIZE_THREAD__

bool is_debugger_running(void)
{
   static int cached = -1;
   if (cached != -1)
      return cached;

#if defined  __SANITIZE_THREAD__
   return false;
#elif defined __APPLE__

   struct kinfo_proc info;
   info.kp_proc.p_flag = 0;

   int mib[4];
   mib[0] = CTL_KERN;
   mib[1] = KERN_PROC;
   mib[2] = KERN_PROC_PID;
   mib[3] = getpid();

   size_t size = sizeof(info);
   int rc = sysctl(mib, sizeof(mib) / sizeof(*mib), &info, &size, NULL, 0);
   if (rc != 0)
      fatal_errno("sysctl");

   return (cached = ((info.kp_proc.p_flag & P_TRACED) != 0));

#elif defined __linux

   // Hack to detect if Valgrind is running
   if (scan_file_for_token("/proc/self/maps", "vgpreload"))
      return (cached = true);

   // Ptrace technique below doesn't work on WSL
   if (scan_file_for_token("/proc/version", "Microsoft"))
      return (cached = false);

#ifdef PR_SET_PTRACER
   // For Linux 3.4 and later allow tracing from any proccess
   // Failure is harmless as this may not be implemented even in a >3.4 kernel
   (void)prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY, 0, 0, 0);
#endif  // PR_SET_PTRACER

   pid_t pid = fork();

   if (pid == -1)
      fatal_errno("fork");
   else if (pid == 0) {
      int ppid = getppid();

      // Try to trace the parent: if we can then GDB is not running
      if (ptrace(PTRACE_ATTACH, ppid, NULL, NULL) == 0) {
         // Wait for the parent to stop and continue it
         waitpid(ppid, NULL, 0);
         ptrace(PTRACE_CONT, NULL, NULL);

         // Detach
         ptrace(PTRACE_DETACH, ppid, NULL, NULL);

         // Able to trace so debugger not present
         exit(0);
      }
      else {
         // Trace failed so debugger is present
         exit(1);
      }
   }
   else {
      int status;
      waitpid(pid, &status, 0);
      return (cached = WEXITSTATUS(status));
   }

#else

   // Not able to detect debugger on this platform
   return (cached = false);

#endif
}

void register_signal_handlers(void)
{
#if defined __MINGW32__
   SetUnhandledExceptionFilter(win32_exception_handler);
#elif !defined __SANITIZE_THREAD__
   (void)is_debugger_running();    // Caches the result

   struct sigaction sa;
   sa.sa_sigaction = signal_handler;
   sigemptyset(&sa.sa_mask);
   sa.sa_flags = SA_RESTART | SA_SIGINFO;

   sigaction(SIGSEGV, &sa, NULL);
   sigaction(SIGUSR1, &sa, NULL);
   sigaction(SIGFPE, &sa, NULL);
   sigaction(SIGBUS, &sa, NULL);
   sigaction(SIGILL, &sa, NULL);
   sigaction(SIGABRT, &sa, NULL);
#endif  // !__SANITIZE_THREAD__
}

void term_init(void)
{
   const char *nvc_colors = getenv("NVC_COLORS");
   const char *term = getenv("TERM") ?: "";

   static const char *term_blacklist[] = {
      "dumb"
   };

   bool is_tty = isatty(STDERR_FILENO) && isatty(STDOUT_FILENO);

#ifdef __MINGW32__
   if (!is_tty) {
      // Handle running under MinTty
      HANDLE hStdOut = GetStdHandle(STD_OUTPUT_HANDLE);
      const size_t size = sizeof(FILE_NAME_INFO) + sizeof(WCHAR) * MAX_PATH;
      FILE_NAME_INFO *nameinfo = malloc(size);
      if (!GetFileInformationByHandleEx(hStdOut, FileNameInfo, nameinfo, size))
         fatal_errno("GetFileInformationByHandle");

      if ((wcsncmp(nameinfo->FileName, L"\\msys-", 6) == 0
           || wcsncmp(nameinfo->FileName, L"\\cygwin-", 8) == 0)
          && wcsstr(nameinfo->FileName, L"pty") != NULL)
         is_tty = true;

      free(nameinfo);
   }
#endif

   if (nvc_colors && strcmp(nvc_colors, "always") == 0)
      want_color = true;
   else if (nvc_colors && strcmp(nvc_colors, "never") == 0)
      want_color = false;
   else {
      want_color = is_tty;

      if (want_color && (term != NULL)) {
         for (size_t i = 0; i < ARRAY_LEN(term_blacklist); i++) {
            if (strcmp(term, term_blacklist[i]) == 0) {
               want_color = false;
               break;
            }
         }
      }
   }

#ifdef __MINGW32__
   HANDLE hConsole = GetStdHandle(STD_ERROR_HANDLE);
   DWORD mode;
   if (GetConsoleMode(hConsole, &mode)) {
      mode |= 0x04; // ENABLE_VIRTUAL_TERMINAL_PROCESSING
      if (!SetConsoleMode(hConsole, mode))
         want_color = false;
   }
#endif
}

static void opt_set_generic(const char *name, option_kind_t kind,
                            optval_t value)
{
   ident_t name_i = ident_new(name);
   struct option *it;
   for (it = options; (it != NULL) && (it->key != name_i); it = it->next)
      ;

   if (it != NULL) {
      if (it->kind == OPTION_STRING)
         free(it->value.s);
      it->value = value;
   }
   else {
      it = xmalloc(sizeof(struct option));
      it->key   = ident_new(name);
      it->value = value;
      it->next  = options;
      it->kind  = kind;

      options = it;
   }
}

static optval_t opt_get_generic(const char *name, option_kind_t kind)
{
   ident_t name_i = ident_new(name);
   struct option *it;
   for (it = options; (it != NULL) && (it->key != name_i); it = it->next)
      ;

   if (it != NULL) {
      if (it->kind == kind)
         return it->value;
      else
         fatal_trace("wrong option kind for %s", name);
   }
   else
      fatal_trace("invalid option %s", name);
}

void opt_set_int(const char *name, int val)
{
   opt_set_generic(name, OPTION_INT, (optval_t)val);
}

int opt_get_int(const char *name)
{
   return opt_get_generic(name, OPTION_INT).i;
}

void opt_set_str(const char *name, const char *val)
{
   opt_set_generic(name, OPTION_STRING, (optval_t)(val ? strdup(val) : NULL));
}

const char *opt_get_str(const char *name)
{
   return opt_get_generic(name, OPTION_STRING).s;
}

char *get_fmt_buf(size_t len)
{
   // This is a bit of a kludge but keeping a sufficient number
   // of static buffers allows us to use format functions multiple
   // times in printf
   static char   *buf_set[MAX_FMT_BUFS];
   static size_t  buflen[MAX_FMT_BUFS];
   static int     next_buf = 0;

   char **bufp = &buf_set[next_buf];
   size_t *blenp = &buflen[next_buf];
   next_buf = (next_buf + 1) % MAX_FMT_BUFS;

   if (*bufp == NULL) {
      *bufp = xmalloc(len);
      *blenp = len;
   }

   while (len > *blenp) {
      *blenp *= 2;
      *bufp = xrealloc(*bufp, *blenp);
   }

   return *bufp;
}

int next_power_of_2(int n)
{
   n--;
   n |= n >> 1;
   n |= n >> 2;
   n |= n >> 4;
   n |= n >> 8;
   n |= n >> 16;
   n++;
   return n;
}

int ilog2(int64_t n)
{
   if (n <= 1)
      return 1;
   else {
      int r = 0;
      int64_t c = 1;
      while (c < n) {
         r += 1;
         c *= 2;
      }
      return r;
   }
}

int64_t ipow(int64_t x, int64_t y)
{
   assert(y >= 0);
   int64_t r = 1;
   while (y) {
      if (y & 1)
         r *= x;
      y >>= 1;
      x *= x;
   }
   return r;
}

static long nvc_page_size(void)
{
#ifndef __MINGW32__
   return sysconf(_SC_PAGESIZE);
#else
   return 4096;   // TODO: how to get page size on Windows?
#endif
}

static void *nvc_mmap(size_t sz)
{
   sz = ALIGN_UP(sz, nvc_page_size());

#if __SANITIZE_ADDRESS__
   void *ptr;
   if (posix_memalign(&ptr, nvc_page_size(), sz) != 0)
      fatal_errno("posix_memalign");

   return ptr;
#elif !defined __MINGW32__
   void *ptr = mmap(NULL, sz, PROT_READ | PROT_WRITE,
                    MAP_SHARED | MAP_ANON, -1, 0);
   if (ptr == MAP_FAILED)
      fatal_errno("mmap");
#else
   void *ptr = VirtualAlloc(NULL, sz, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
   if (ptr == NULL)
      fatal_errno("VirtualAlloc");
#endif

   return ptr;
}

void nvc_munmap(void *ptr, size_t length)
{
#if __SANITIZE_ADDRESS__
   // Ignore it
#elif !defined __MINGW32__
   if (munmap(ptr, length) != 0)
      fatal_errno("munmap");
#else
   if (!VirtualFree(ptr, length, MEM_DECOMMIT))
      fatal_errno("VirtualFree");
#endif
}

void *nvc_memalign(size_t align, size_t sz)
{
#if __SANITIZE_ADDRESS__
   void *ptr;
   if (posix_memalign(&ptr, align, sz) != 0)
      fatal_errno("posix_memalign");

   return ptr;
#else
   assert((align & (align - 1)) == 0);
   const size_t mapsz = ALIGN_UP(sz + align - 1, align);
   void *ptr = nvc_mmap(mapsz);

   void *aligned = ALIGN_UP(ptr, align);
   void *limit = aligned + sz;

   const size_t low_waste = aligned - ptr;
   const size_t high_waste = ptr + mapsz - limit;
   assert(low_waste + high_waste == align);

   if (low_waste > 0) nvc_munmap(ptr, low_waste);
   if (high_waste > 0) nvc_munmap(limit, high_waste);

   return aligned;
#endif
}

void nvc_memprotect(void *ptr, size_t length, mem_access_t prot)
{
#if __SANITIZE_ADDRESS__
   // Ignore it
#elif !defined __MINGW32__
   static const int map[] = {
      PROT_NONE, PROT_READ, PROT_READ | PROT_WRITE
   };
   if (mprotect(ptr, length, map[prot]) < 0)
      fatal_errno("mprotect");
#else
   static const int map[] = {
      PAGE_NOACCESS, PAGE_READONLY, PAGE_READWRITE
   };
   DWORD old_prot;
   if (!VirtualProtect(ptr, length, map[prot], &old_prot))
      fatal_errno("VirtualProtect");
#endif
}

void *mmap_guarded(size_t sz, const char *tag)
{
   const long pagesz = nvc_page_size();
   sz = ALIGN_UP(sz, pagesz);

   void *ptr = nvc_mmap(sz + pagesz);

   uint8_t *guard_ptr = (uint8_t *)ptr + sz;
   nvc_memprotect(guard_ptr, pagesz, MEM_NONE);

   guard_t *guard = xmalloc(sizeof(guard_t));
   guard->next  = guards;
   guard->tag   = tag;
   guard->base  = (uintptr_t)guard_ptr;
   guard->limit = guard->base + pagesz;

   guards = guard;

   return ptr;
}

int checked_sprintf(char *buf, int len, const char *fmt, ...)
{
   assert(len > 0);

   va_list ap;
   va_start(ap, fmt);

   const int nbytes = vsnprintf(buf, len, fmt, ap);
   if (nbytes >= len)
      fatal_trace("checked_sprintf requires %d bytes but have %d",
                  nbytes + 1, len);

   va_end(ap);

   return nbytes;
}

text_buf_t *tb_new(void)
{
   text_buf_t *tb = xmalloc(sizeof(text_buf_t));
   tb->alloc = 256;
   tb->len   = 0;
   tb->buf   = xmalloc(tb->alloc);

   tb->buf[0] = '\0';

   return tb;
}

void tb_free(text_buf_t *tb)
{
   free(tb->buf);
   free(tb);
}

void _tb_cleanup(text_buf_t **tb)
{
   if (*tb != NULL)
      tb_free(*tb);
}

void tb_printf(text_buf_t *tb, const char *fmt, ...)
{
   int nchars, avail;
   for (;;) {
      va_list ap;
      va_start(ap, fmt);

      avail  = tb->alloc - tb->len;
      nchars = vsnprintf(tb->buf + tb->len, avail, fmt, ap);

      va_end(ap);

      if (nchars + 1 < avail)
         break;

      tb->alloc *= 2;
      tb->buf = xrealloc(tb->buf, tb->alloc);
   }

   tb->len += nchars;
}

void tb_append(text_buf_t *tb, char ch)
{
   if (tb->len + 2 >= tb->alloc) {
      tb->alloc *= 2;
      tb->buf = xrealloc(tb->buf, tb->alloc);
   }

   tb->buf[(tb->len)++] = ch;
   tb->buf[tb->len] = '\0';
}

void tb_catn(text_buf_t *tb, const char *str, size_t nchars)
{
   if (tb->len + nchars + 1 >= tb->alloc) {
      tb->alloc = next_power_of_2(tb->alloc + nchars);
      tb->buf = xrealloc(tb->buf, tb->alloc);
   }

   memcpy(tb->buf + tb->len, str, nchars + 1);
   tb->len += nchars;
}

void tb_cat(text_buf_t *tb, const char *str)
{
   tb_catn(tb, str, strlen(str));
}

void tb_repeat(text_buf_t *tb, char ch, size_t count)
{
   if (tb->len + count + 1 >= tb->alloc) {
      tb->alloc = next_power_of_2(tb->alloc + count + 1);
      tb->buf = xrealloc(tb->buf, tb->alloc);
   }

   memset(tb->buf + tb->len, ch, count);
   tb->len += count;
   tb->buf[tb->len] = '\0';
}

char *tb_reserve(text_buf_t *tb, size_t size)
{
   if (tb->len + size + 1 >= tb->alloc) {
      tb->alloc = next_power_of_2(tb->alloc + size + 1);
      tb->buf = xrealloc(tb->buf, tb->alloc);
   }

   char *start = tb->buf + tb->len;

   tb->len += size;
   tb->buf[tb->len] = '\0';

   return start;
}

size_t tb_len(text_buf_t *tb)
{
   return tb->len;
}

char *tb_claim(text_buf_t *tb)
{
   char *buf = tb->buf;
   tb->buf = NULL;
   return buf;
}

const char *tb_get(text_buf_t *tb)
{
   return tb->buf;
}

void tb_rewind(text_buf_t *tb)
{
   tb->len = 0;
   tb->buf[0] = '\0';
}

void tb_backup(text_buf_t *tb, unsigned n)
{
   tb->len = n > tb->len ? 0 : tb->len - n;
   tb->buf[tb->len] = '\0';
}

void _local_free(void *ptr)
{
   free(*(void **)ptr);
}

void set_message_style(message_style_t style)
{
   message_style = style;

   if (style == MESSAGE_COMPACT)
      want_color = false;
}

message_style_t get_message_style(void)
{
   return message_style;
}

#ifndef __MINGW32__
static unsigned tv2ms(struct timeval *tv)
{
   return (tv->tv_sec * 1000) + (tv->tv_usec / 1000);
}
#endif

void nvc_rusage(nvc_rusage_t *ru)
{
#ifndef __MINGW32__
   static struct rusage last;

   struct rusage sys;
   if (getrusage(RUSAGE_SELF, &sys) < 0)
      fatal_errno("getrusage");

   const unsigned utime = tv2ms(&(sys.ru_utime)) - tv2ms(&(last.ru_utime));
   const unsigned stime = tv2ms(&(sys.ru_stime)) - tv2ms(&(last.ru_stime));

   ru->ms = utime + stime;

#ifdef __APPLE__
   const int rss_units = 1024;
#else
   const int rss_units = 1;
#endif

   ru->rss = sys.ru_maxrss / rss_units;

   last = sys;
#else
   static long long last;
   ULARGE_INTEGER lv_Tkernel, lv_Tuser;
   HANDLE hProcess = GetCurrentProcess();

   FILETIME ftCreation, ftExit, ftKernel, ftUser;
   if (!GetProcessTimes(hProcess, &ftCreation, &ftExit, &ftKernel, &ftUser))
      fatal_errno("GetProcessTimes");

   lv_Tkernel.LowPart = ftKernel.dwLowDateTime;
   lv_Tkernel.HighPart = ftKernel.dwHighDateTime;
   lv_Tuser.LowPart = ftUser.dwLowDateTime;
   lv_Tuser.HighPart = ftUser.dwHighDateTime;

   ru->ms = (lv_Tkernel.QuadPart + lv_Tuser.QuadPart) / 10000 - last;
   last = ru->ms;

   PROCESS_MEMORY_COUNTERS counters;
   if (!GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters)))
      fatal_errno("GetProcessMemoryInfo");

   ru->rss = counters.PeakWorkingSetSize / 1024;
#endif
}

void run_program(const char *const *args, size_t n_args)
{
#if defined __CYGWIN__ || defined __MINGW32__
   int status = spawnv(_P_WAIT, args[0], (char *const *)args);
#else  // __CYGWIN__
   pid_t pid = fork();
   int status = 0;
   if (pid == 0) {
      execv(args[0], (char *const *)args);
      fatal_errno("execv");
   }
   else if (pid > 0) {
      if (waitpid(pid, &status, 0) != pid)
         fatal_errno("waitpid");

      status = WEXITSTATUS(status);
   }
   else
      fatal_errno("fork");
#endif  // __CYGWIN__

   if (status != 0) {
      for (size_t i = 0; i < n_args && args[i]; i++)
         fprintf(stderr, "%s%s", i > 0 ? " " : "", args[i]);
      fprintf(stderr, "\n");
      fflush(stderr);
      fatal("%s failed with status %d", args[0], status);
   }
}

void file_read_lock(int fd)
{
#ifdef __MINGW32__
   HANDLE hf = (HANDLE)_get_osfhandle(fd);

   LARGE_INTEGER li;
   li.QuadPart = _filelengthi64(fd);

   OVERLAPPED ovlp;
   memset(&ovlp, 0, sizeof ovlp);

   if (!LockFileEx(hf, 0, 0, li.LowPart, li.HighPart, &ovlp))
      fatal_errno("LockFileEx");
#else
   if (flock(fd, LOCK_SH) < 0)
      fatal_errno("flock");
#endif
}

void file_write_lock(int fd)
{
#ifdef __MINGW32__
   HANDLE hf = (HANDLE)_get_osfhandle(fd);

   LARGE_INTEGER li;
   li.QuadPart = _filelengthi64(fd);

   OVERLAPPED ovlp;
   memset(&ovlp, 0, sizeof ovlp);

   if (!LockFileEx(hf, LOCKFILE_EXCLUSIVE_LOCK, 0,
                   li.LowPart, li.HighPart, &ovlp))
      fatal_errno("LockFileEx");
#else
   if (flock(fd, LOCK_EX) < 0)
      fatal_errno("flock");
#endif
}

void file_unlock(int fd)
{
#ifdef __MINGW32__
   HANDLE hf = (HANDLE)_get_osfhandle(fd);

   LARGE_INTEGER li;
   li.QuadPart = _filelengthi64 (fd);

   UnlockFile(hf, 0, 0, li.LowPart, li.HighPart);
#else
   if (flock(fd, LOCK_UN) < 0)
      fatal_errno("flock");
#endif
}

void *map_file(int fd, size_t size)
{
#ifdef __MINGW32__
   HANDLE handle = CreateFileMapping((HANDLE) _get_osfhandle(fd), NULL,
                                     PAGE_READONLY, 0, size, NULL);
   if (!handle)
      fatal_errno("CreateFileMapping");

   void *ptr = MapViewOfFileEx(handle, FILE_MAP_COPY, 0,
                               0, (SIZE_T) size, (LPVOID) NULL);
   CloseHandle(handle);
   if (ptr == NULL)
      fatal_errno("MapViewOfFileEx");
#else
   void *ptr = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
   if (ptr == MAP_FAILED)
      fatal_errno("mmap");
#endif
   return ptr;
}

void unmap_file(void *ptr, size_t size)
{
#ifdef __MINGW32__
   if (!UnmapViewOfFile((LPCVOID) ptr))
      fatal_errno("UnmapViewOfFile");
#else
   munmap(ptr, size);
#endif
}

void make_dir(const char *path)
{
#ifdef __MINGW32__
   if (!CreateDirectory(path, NULL) && (GetLastError() != ERROR_ALREADY_EXISTS))
      fatal_errno("mkdir: %s", path);
#else
   if (mkdir(path, 0777) != 0 && errno != EEXIST)
      fatal_errno("mkdir: %s", path);
#endif
}

uint64_t get_timestamp_us()
{
#if defined __MINGW32__
   return 0;  // TODO
#else
   struct timespec ts;
   if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
      fatal_errno("clock_gettime");
   return (ts.tv_nsec / 1000) + (ts.tv_sec * 1000 * 1000);
#endif
}

#if defined _WIN32 || defined __CYGWIN__
static struct {
   char illegal;
   const char *rep;
} symbol_replacements[] = {
   { '(', "_lp_"   },
   { ')', "_rp_"   },
   { '"', "_q_"    },
   { '[', "_ls_"   },
   { ']', "_rs_"   },
   { '*', "_mult_" },
   { '+', "_plus_" },
   { '=', "_eq_"   }
};

static text_buf_t *safe_symbol_win32(const char *text)
{
   text_buf_t *tb = tb_new();

   for (const char *p = text; *p != '\0' && p - text < 240; p++) {
      bool replaced = false;
      for (size_t j = 0; j < ARRAY_LEN(symbol_replacements); j++) {
         if (*p == symbol_replacements[j].illegal) {
            tb_cat(tb, symbol_replacements[j].rep);
            replaced = true;
            break;
         }
      }

      if (!replaced)
         tb_append(tb, *p);
   }

   return tb;
}

#endif

text_buf_t *safe_symbol(ident_t id)
{
   // Return a string that is safe to use as a symbol name on this platform

   text_buf_t *tb = tb_new();
   ident_str(id, tb);

#if defined _WIN32 || defined __CYGWIN__
   if (strpbrk(tb_get(tb), "()\"[]*+=") == NULL)
      return tb;
   else {
      text_buf_t *new = safe_symbol_win32(tb_get(tb));
      tb_free(tb);
      return new;
   }
#else
   return tb;
#endif
}

text_buf_t *safe_symbol_str(const char *text)
{
#if defined _WIN32 || defined __CYGWIN__
   if (strpbrk(text, "()\"[]*+=") != NULL)
      return safe_symbol_win32(text);
#endif

   text_buf_t *tb = tb_new();
   tb_cat(tb, text);
   return tb;
}

text_buf_t *unsafe_symbol(const char *text)
{
   // Restore original symbol from safe_symbol

   text_buf_t *tb = tb_new();

#if defined _WIN32 || defined __CYGWIN__
   const char *p = text;
   while (*p) {
      bool replaced = false;
      for (size_t j = 0; j < ARRAY_LEN(symbol_replacements); j++) {
         size_t len = strlen(symbol_replacements[j].rep);
         if (strncmp(p, symbol_replacements[j].rep, len) == 0) {
            tb_append(tb, symbol_replacements[j].illegal);
            p += len;
            replaced = true;
            break;
         }
      }

      if (!replaced)
         tb_append(tb, *p++);
   }

   return tb;
#else
   tb_cat(tb, text);
#endif

   return tb;
}

void __cleanup_array(void *ptr)
{
   A(void *) *a = ptr;
   ACLEAR(*a);
}

void __array_resize_slow(void **ptr, uint32_t *limit, uint32_t count,
                         size_t size)
{
   if (count == 0) {
      free(*ptr);
      *ptr = NULL;
      *limit = 0;
   }
   else {
      if (*limit == 0)
         *limit = count;  // Setting the initial size of the array
      else
         *limit = next_power_of_2(count);
      *ptr = xrealloc_array(*ptr, *limit, size);
   }
}

unsigned error_count(void)
{
   return n_errors;
}

void reset_error_count(void)
{
   n_errors = 0;
}

char *search_path(const char *name)
{
   const char *path = getenv("PATH");
   if (path == NULL)
      return xstrdup(name);

   char LOCAL *tmp = xstrdup(path);
   for (char *p = strtok(tmp, ":"); p; p = strtok(NULL, ":")) {
      char *full = xasprintf("%s"DIR_SEP"%s", p, name);

      struct stat sb;
      if (stat(full, &sb) == 0)
         return full;

      free(full);
   }

   return xstrdup(name);
}

void progress(const char *fmt, ...)
{
   if (opt_get_int("verbose")) {
      va_list ap;
      va_start(ap, fmt);
      char *msg LOCAL = xvasprintf(fmt, ap);
      va_end(ap);

      static nvc_rusage_t last_ru;

      nvc_rusage_t ru;
      nvc_rusage(&ru);
      notef("%s [%ums %+dkB]", msg, ru.ms, ru.rss - last_ru.rss);

      last_ru = ru;
   }
}

nvc_thread_t *thread_create(void *(*fn)(void *), void *arg,
                            const char *fmt, ...)
{
   nvc_thread_t *thread = xcalloc(sizeof(nvc_thread_t));

   va_list ap;
   va_start(ap, fmt);
   thread->name = xvasprintf(fmt, ap);
   va_end(ap);

#if defined HAVE_PTHREAD
   if (pthread_create(&(thread->handle), NULL, fn, arg) != 0)
      fatal_errno("pthread_create");
#elif defined _WIN32
   if ((thread->handle = CreateThread(NULL, 0, fn, arg, 0, NULL)) == NULL)
      fatal_errno("CreateThread");
#else
   fatal_trace("threads are not supported on this platform");
#endif

   return thread;
}

void *thread_join(nvc_thread_t *thread)
{
   void *retval = NULL;

#if defined HAVE_PTHREAD
   if (pthread_join(thread->handle, &retval) != 0)
      fatal_errno("pthread_join");
#elif defined _WIN32
   if (WaitForSingleObject(thread->handle, INFINITE) == WAIT_FAILED)
      fatal_errno("WaitForSingleObject");
#endif

   free(thread->name);
   free(thread);

   return retval;
}

nvc_mutex_t *mutex_create(void)
{
   nvc_mutex_t *mtx = xcalloc(sizeof(nvc_mutex_t));

#if defined HAVE_PTHREAD
   if (pthread_mutex_init(&(mtx->mutex), NULL) != 0)
      fatal_errno("pthread_mutex_init");
#else
   fatal_trace("mutexes are not supported on this platform");
#endif

   return mtx;
}

void mutex_lock(nvc_mutex_t *mtx)
{
#ifdef HAVE_PTHREAD
   if (unlikely(pthread_mutex_lock(&(mtx->mutex)) != 0))
      fatal_errno("pthread_mutex_lock");
#endif
}

void mutex_unlock(nvc_mutex_t *mtx)
{
#ifdef HAVE_PTHREAD
   if (unlikely(pthread_mutex_unlock(&(mtx->mutex)) != 0))
      fatal_errno("pthread_mutex_unlock");
#endif
}

void mutex_destroy(nvc_mutex_t *mtx)
{
#ifdef HAVE_PTHREAD
   if (pthread_mutex_destroy(&(mtx->mutex)) != 0)
      fatal_errno("pthread_mutex_destroy");
#endif

   free(mtx);
}

void __scoped_unlock(nvc_mutex_t **pmtx)
{
   mutex_unlock(*pmtx);
}

unsigned nvc_nprocs(void)
{
#if defined _WIN32
   SYSTEM_INFO sysinfo;
   GetSystemInfo(&sysinfo);

   return sysinfo.dwNumberOfProcessors;
#elif defined _SC_NPROCESSORS_ONLN
   long count = sysconf(_SC_NPROCESSORS_ONLN);
   if (count == -1)
      fatal_errno("sysconf(_SC_NPROCESSORS_ONLN)");

   return count;
#else
#warning Cannot detect number of processors on this platform
   return 1;
#endif
}
