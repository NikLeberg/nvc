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

#include "util.h"
#include "alloc.h"
#include "array.h"
#include "common.h"
#include "cover.h"
#include "debug.h"
#include "diag.h"
#include "ffi.h"
#include "hash.h"
#include "heap.h"
#include "lib.h"
#include "opt.h"
#include "rt/rt.h"
#include "rt/mspace.h"
#include "tree.h"
#include "type.h"

#include <assert.h>
#include <stdint.h>
#include <stdarg.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <errno.h>
#include <signal.h>
#include <sys/time.h>
#include <float.h>
#include <ctype.h>
#include <time.h>
#include <setjmp.h>

#ifdef HAVE_ALLOCA_H
#include <alloca.h>
#endif

#ifdef __MINGW32__
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#undef SEVERITY_ERROR
#endif

#define TRACE_DELTAQ  1
#define TRACE_PENDING 0
#define TRACE_SIGNALS 1
#define RT_DEBUG      0

typedef struct event         event_t;
typedef struct waveform      waveform_t;
typedef struct sens_list     sens_list_t;
typedef struct callback      callback_t;
typedef struct rt_nexus_s    rt_nexus_t;
typedef struct rt_source_s   rt_source_t;
typedef struct rt_implicit_s rt_implicit_t;
typedef struct rt_proc_s     rt_proc_t;
typedef struct rt_alias_s    rt_alias_t;

typedef void *(*proc_fn_t)(void *, rt_scope_t *);
typedef void *(*value_fn_t)(rt_nexus_t *);

typedef enum {
   W_PROC, W_WATCH, W_IMPLICIT
} wakeable_kind_t;

typedef struct {
   uint32_t        wakeup_gen;
   wakeable_kind_t kind : 8;
   bool            pending;
   bool            postponed;
} rt_wakeable_t;

typedef struct rt_proc_s {
   rt_wakeable_t  wakeable;
   tree_t         where;
   ident_t        name;
   proc_fn_t      proc_fn;
   tlab_t         tlab;
   rt_scope_t    *scope;
   rt_proc_t     *chain;
   mptr_t         privdata;
} rt_proc_t;

typedef enum {
   EVENT_TIMEOUT,
   EVENT_DRIVER,
   EVENT_PROCESS,
   EVENT_EFFECTIVE,
} event_kind_t;

typedef struct {
   timeout_fn_t  fn;
   void         *user;
} event_timeout_t;

typedef struct {
   rt_nexus_t   *nexus;
   rt_source_t  *source;
} event_driver_t;

typedef struct {
   rt_proc_t *proc;
   uint32_t   wakeup_gen;
} event_proc_t;

struct event {
   uint64_t            when;
   event_kind_t        kind;
   event_t            *delta_chain;
   union {
      event_timeout_t  timeout;
      event_driver_t   driver;
      event_proc_t     proc;
      rt_nexus_t      *effective;
   };
};

typedef union {
   uint8_t   bytes[8];
   uint64_t  qword;
   void     *ext;
} rt_value_t;

struct waveform {
   uint64_t    when : 63;
   unsigned    null : 1;
   waveform_t *next;
   rt_value_t  value;
};

struct sens_list {
   rt_wakeable_t *wake;
   sens_list_t   *next;
   sens_list_t  **reenq;
   uint32_t       wakeup_gen;
};

typedef struct {
   sens_list_t *pending;
   uint64_t     last_event;
   uint64_t     last_active;
   int32_t      event_delta;
   int32_t      active_delta;
   uint32_t     net_id;
   uint32_t     refcnt;
} rt_net_t;

typedef enum {
   SOURCE_DRIVER,
   SOURCE_PORT,
} source_kind_t;

typedef struct {
   rt_proc_t  *proc;
   waveform_t  waveforms;
} rt_driver_t;

typedef struct {
   ffi_closure_t closure;
   unsigned      refcnt;
   size_t        bufsz;
   uint8_t       buffer[0];
} rt_conv_func_t;

typedef struct {
   rt_nexus_t     *input;
   rt_nexus_t     *output;
   rt_conv_func_t *conv_func;
} rt_port_t;

typedef struct rt_source_s {
   rt_source_t    *chain_input;
   rt_source_t    *chain_output;
   source_kind_t   tag;
   union {
      rt_port_t    port;
      rt_driver_t  driver;
      void        *initial;
   } u;
} rt_source_t;

// The code generator knows the layout of this struct
typedef struct {
   ffi_closure_t    closure;
   uint32_t         flags;
   int32_t          ileft;
   int32_t          nlits;
} rt_resolution_t;

typedef struct {
   ffi_closure_t closure;
   res_flags_t   flags;
   int32_t       ileft;
   int8_t        tab2[16][16];
   int8_t        tab1[16];
} res_memo_t;

typedef struct rt_nexus_s {
   rt_nexus_t   *chain;
   void         *free_value;
   rt_net_t     *net;
   rt_value_t    forcing;
   uint32_t      width;
   net_flags_t   flags : 8;
   uint8_t       size;
   uint8_t       n_sources;
   uint8_t       __pad;
   rt_source_t   sources;
   rt_signal_t  *signal;
   rt_source_t  *outputs;
   void         *resolved;
} rt_nexus_t;

// The code generator knows the layout of this struct
typedef struct {
   uint32_t size;
   uint32_t offset;
   uint8_t  data[0];
} sig_shared_t;

typedef struct rt_signal_s {
   tree_t          where;
   rt_signal_t    *chain;
   rt_scope_t     *parent;
   ihash_t        *index;
   res_memo_t     *resolution;
   net_flags_t     flags;
   uint32_t        n_nexus;
   rt_nexus_t      nexus;
   sig_shared_t    shared;
} rt_signal_t;

typedef struct rt_implicit_s {
   rt_wakeable_t  wakeable;
   ffi_closure_t *closure;
   rt_signal_t    signal;   // Has a flexible member
} rt_implicit_t;

typedef struct rt_alias_s {
   rt_alias_t  *chain;
   tree_t       where;
   rt_signal_t *signal;
} rt_alias_t;

typedef enum {
   SCOPE_ROOT,
   SCOPE_INSTANCE,
   SCOPE_PACKAGE,
   SCOPE_SIGNAL,
} rt_scope_kind_t;

typedef enum {
   SCOPE_F_RESOLVED = (1 << 0)
} rt_scope_flags_t;

typedef struct rt_scope_s {
   rt_signal_t     *signals;
   rt_proc_t       *procs;
   rt_alias_t      *aliases;
   rt_scope_kind_t  kind;
   rt_scope_flags_t flags;
   unsigned         size;   // For signal scopes
   unsigned         offset;
   ident_t          name;
   tree_t           where;
   mptr_t           privdata;
   rt_scope_t      *parent;
   rt_scope_t      *child;
   rt_scope_t      *chain;
} rt_scope_t;

typedef struct {
   event_t **queue;
   size_t    wr, rd;
   size_t    alloc;
} rt_run_queue_t;

typedef struct rt_watch_s {
   rt_wakeable_t   wakeable;
   rt_signal_t    *signal;
   sig_event_fn_t  fn;
   rt_watch_t     *chain_all;
   void           *user_data;
} rt_watch_t;

typedef enum {
   SIDE_EFFECT_ALLOW,
   SIDE_EFFECT_DISALLOW,
   SIDE_EFFECT_OCCURRED
} side_effect_t;

struct callback {
   rt_event_fn_t  fn;
   void          *user;
   callback_t    *next;
};

typedef struct {
   uint32_t n_signals;
   uint32_t n_contig;
   uint32_t n_procs;
   uint32_t runq_min;
   uint32_t runq_max;
   uint32_t n_simple;
   uint32_t nmap_direct;
   uint32_t nmap_search;
   uint32_t nmap_divide;
   double   runq_mean;
   uint64_t deltas;
} rt_profile_t;

static rt_proc_t       *active_proc = NULL;
static rt_scope_t      *active_scope = NULL;
static rt_scope_t      *root = NULL;
static hash_t          *scopes = NULL;
static rt_run_queue_t   timeoutq;
static rt_run_queue_t   driverq;
static rt_run_queue_t   procq;
static rt_run_queue_t   effq;
static heap_t          *eventq_heap = NULL;
static uint64_t         now = 0;
static int              iteration = -1;
static bool             trace_on = false;
static nvc_rusage_t     ready_rusage;
static sens_list_t     *resume = NULL;
static sens_list_t     *postponed = NULL;
static sens_list_t     *resume_watch = NULL;
static sens_list_t     *postponed_watch = NULL;
static sens_list_t     *implicit = NULL;
static rt_watch_t      *watches = NULL;
static event_t         *delta_proc = NULL;
static event_t         *delta_driver = NULL;
static hash_t          *res_memo_hash = NULL;
static side_effect_t    init_side_effect = SIDE_EFFECT_ALLOW;
static bool             force_stop;
static bool             can_create_delta;
static callback_t      *global_cbs[RT_LAST_EVENT];
static bool             profiling = false;
static rt_profile_t     profile;
static rt_nexus_t      *nexuses = NULL;
static rt_nexus_t     **nexus_tail = NULL;
static cover_tagging_t *cover = NULL;
static rt_signal_t    **signals_tail = NULL;
static jmp_buf          abort_env;
static mspace_t        *mspace = NULL;

static __thread tlab_t spare_tlab = {};

static rt_alloc_stack_t event_stack = NULL;
static rt_alloc_stack_t waveform_stack = NULL;
static rt_alloc_stack_t sens_list_stack = NULL;
static rt_alloc_stack_t watch_stack = NULL;
static rt_alloc_stack_t callback_stack = NULL;

static void deltaq_insert_proc(uint64_t delta, rt_proc_t *wake);
static void deltaq_insert_driver(uint64_t delta, rt_nexus_t *nexus,
                                 rt_source_t *source);
static void rt_sched_driver(rt_nexus_t *nexus, uint64_t after,
                            uint64_t reject, rt_value_t value, bool null);
static void rt_sched_event(sens_list_t **list, rt_wakeable_t *proc, bool recur);
static void *rt_tlab_alloc(size_t size);
static rt_value_t rt_alloc_value(rt_nexus_t *n);
static void rt_free_value(rt_nexus_t *n, rt_value_t v);
static void rt_copy_value_ptr(rt_nexus_t *n, rt_value_t *v, const void *p);
static inline uint8_t *rt_value_ptr(rt_nexus_t *n, rt_value_t *v);
static rt_nexus_t *rt_clone_nexus(rt_nexus_t *old, int offset, rt_net_t *net);
static res_memo_t *rt_memo_resolution_fn(rt_signal_t *signal,
                                         rt_resolution_t *resolution);
static void *rt_source_value(rt_nexus_t *nexus, rt_source_t *src);
static void *rt_driving_value(rt_nexus_t *nexus);
static void rt_push_run_queue(rt_run_queue_t *q, event_t *e);
static void _tracef(const char *fmt, ...);

#define FMT_VALUES_SZ   128
#define NEXUS_INDEX_MIN 32
#define MAX_NEXUS_WIDTH 4096
#define CACHELINE       64

#if RT_DEBUG && !defined NDEBUG
#define RT_ASSERT(x) assert((x))
#else
#define RT_ASSERT(x)
#endif

// Helper macro for passing debug loci from LLVM
#define DEBUG_LOCUS(name) \
   const char *name##_unit, uint32_t name##_offset

#define TRACE(...) do {                                 \
      if (unlikely(trace_on)) _tracef(__VA_ARGS__);     \
   } while (0)

#define FOR_ALL_SIZES(size, macro) do {                 \
      switch (size) {                                   \
      case 1:                                           \
         macro(uint8_t); break;                         \
      case 2:                                           \
         macro(uint16_t); break;                        \
      case 4:                                           \
         macro(uint32_t); break;                        \
      case 8:                                           \
         macro(uint64_t); break;                        \
      }                                                 \
   } while (0)

////////////////////////////////////////////////////////////////////////////////
// Utilities

static char *fmt_values_r(const void *values, size_t len, char *buf, size_t max)
{
   char *p = buf;
   const uint8_t *vptr = values;

   for (unsigned i = 0; i < len; i++) {
      if (buf + max - p <= 5)
         return p + checked_sprintf(p, buf + max - p, "...");
      else
         p += checked_sprintf(p, buf + max - p, "%02x", *vptr++);
   }

   return buf;
}

static const char *fmt_nexus(rt_nexus_t *n, const void *values)
{
   static char buf[FMT_VALUES_SZ*2 + 2];
   return fmt_values_r(values, n->size * n->width, buf, sizeof(buf));
}

static const char *fmt_values(const void *values, uint32_t len)
{
   static char buf[FMT_VALUES_SZ*2 + 2];
   return fmt_values_r(values, len, buf, sizeof(buf));
}

static void rt_abort_sim(int code)
{
   assert(code >= 0);
#ifdef __MINGW32__
   fatal_exit(code);
#else
   longjmp(abort_env, code + 1);
#endif
}

static void rt_emit_trace(diag_t *d, const loc_t *loc, tree_t enclosing,
                          const char *symbol)
{
   switch (tree_kind(enclosing)) {
   case T_PROCESS:
      diag_trace(d, loc, "Process$$ %s", istr(active_proc->name));
      break;
   case T_FUNC_BODY:
   case T_FUNC_DECL:
      diag_trace(d, loc, "Function$$ %s", type_pp(tree_type(enclosing)));
      break;
   case T_PROC_BODY:
   case T_PROC_DECL:
      diag_trace(d, loc, "Procedure$$ %s",
                 type_pp(tree_type(enclosing)));
      break;
   case T_TYPE_DECL:
      if (strstr(symbol, "$value"))
         diag_trace(d, loc, "Attribute$$ %s'VALUE",
                    istr(tree_ident(enclosing)));
      else
         diag_trace(d, loc, "Type$$ %s", istr(tree_ident(enclosing)));
      break;
   case T_BLOCK:
      diag_trace(d, loc, "Process$$ (init)");
      break;
   default:
      diag_trace(d, loc, "$$%s", istr(tree_ident(enclosing)));
      break;
   }
}

static void rt_diag_trace(diag_t *d)
{
   debug_info_t *di = debug_capture();

   const int nframes = debug_count_frames(di);
   for (int i = 0; i < nframes; i++) {
      const debug_frame_t *f = debug_get_frame(di, i);
      if (f->kind != FRAME_VHDL || f->vhdl_unit == NULL || f->symbol == NULL)
         continue;

      for (debug_inline_t *inl = f->inlined; inl != NULL; inl = inl->next) {
         tree_t enclosing = find_enclosing_decl(inl->vhdl_unit, inl->symbol);
         if (enclosing == NULL)
            continue;

         // Processes should never be inlined
         assert(tree_kind(enclosing) != T_PROCESS);

         loc_file_ref_t file_ref = loc_file_ref(inl->srcfile, NULL);
         loc_t loc = get_loc(inl->lineno, inl->colno, inl->lineno,
                             inl->colno, file_ref);

         rt_emit_trace(d, &loc, enclosing, inl->symbol);
      }

      tree_t enclosing = find_enclosing_decl(f->vhdl_unit, f->symbol);
      if (enclosing == NULL)
         continue;

      loc_t loc;
      if (f->lineno == 0) {
         // Exact DWARF debug info not available
         loc = *tree_loc(enclosing);
      }
      else {
         loc_file_ref_t file_ref = loc_file_ref(f->srcfile, NULL);
         loc = get_loc(f->lineno, f->colno, f->lineno, f->colno, file_ref);
      }

      rt_emit_trace(d, &loc, enclosing, f->symbol);
   }

   debug_free(di);
}

__attribute__((format(printf, 3, 4)))
static void rt_msg(const loc_t *where, diag_level_t level, const char *fmt, ...)
{
   diag_t *d = diag_new(level, where);

   va_list ap;
   va_start(ap, fmt);
   diag_vprintf(d, fmt, ap);
   va_end(ap);

   rt_diag_trace(d);
   diag_emit(d);

   if (level == DIAG_FATAL)
      rt_abort_sim(EXIT_FAILURE);
}

static void rt_mspace_oom_cb(mspace_t *m, size_t size)
{
   diag_t *d = diag_new(DIAG_FATAL, NULL);
   diag_printf(d, "out of memory attempting to allocate %zu byte object", size);
   rt_diag_trace(d);

   const int heapsize = opt_get_int(OPT_HEAP_SIZE);
   diag_hint(d, NULL, "the current heap size is %u bytes which you can "
             "increase with the $bold$-H$$ option, for example $bold$-H %um$$",
             heapsize, MAX(1, (heapsize * 2) / 1024 / 1024));

   diag_emit(d);
   rt_abort_sim(EXIT_FAILURE);
}

static size_t uarray_len(const ffi_uarray_t *u)
{
   return abs(u->dims[0].length);
}

static ffi_uarray_t wrap_str(char *buf, size_t len)
{
   ffi_uarray_t u = {
      .ptr = buf,
      .dims = { [0] = { .left = 1, .length = len } }
   };
   return u;
}

static ffi_uarray_t bit_vec_to_string(const ffi_uarray_t *vec, int log_base)
{
   const size_t vec_len = uarray_len(vec);
   const size_t result_len = (vec_len + log_base - 1) / log_base;
   const int left_pad = (log_base - (vec_len % log_base)) % log_base;
   char *buf = rt_tlab_alloc(result_len);

   for (int i = 0; i < result_len; i++) {
      unsigned nibble = 0;
      for (int j = 0; j < log_base; j++) {
         if (i > 0 || j >= left_pad) {
            nibble <<= 1;
            nibble |= !!(((uint8_t *)vec->ptr)[i*log_base + j - left_pad]);
         }
      }

      static const char map[16] = "0123456789ABCDEF";
      buf[i] = map[nibble];
   }

   return wrap_str(buf, result_len);
}

static int rt_fmt_now(char *buf, size_t len)
{
   if (iteration < 0)
      return checked_sprintf(buf, len, "(init)");
   else {
      char *p = buf;
      p += fmt_time_r(p, buf + len - p, now);
      p += checked_sprintf(p, buf + len - p, "+%d", iteration);
      return p - buf;
   }
}

static inline void rt_check_postponed(int64_t after)
{
   if (unlikely(active_proc->wakeable.postponed && (after == 0)))
      fatal("postponed process %s cannot cause a delta cycle",
            istr(active_proc->name));
}

static inline tree_t rt_locus_to_tree(const char *unit, unsigned offset)
{
   if (unit == NULL)
      return NULL;
   else
      return tree_from_locus(ident_new(unit), offset, lib_get_qualified);
}

static void rt_dump_one_signal(rt_scope_t *scope, rt_signal_t *s, tree_t alias)
{
   rt_nexus_t *n = &(s->nexus);

   LOCAL_TEXT_BUF tb = tb_new();
   if (scope->kind == SCOPE_SIGNAL)
      tb_printf(tb, "%s.", istr(scope->name));
   tb_cat(tb, istr(tree_ident(alias ?: s->where)));
   if (alias != NULL)
      tb_append(tb, '*');

   for (int nth = 0; nth < s->n_nexus; nth++, n = n->chain) {
      int n_outputs = 0;
      for (rt_source_t *s = n->outputs; s != NULL; s = s->chain_output)
         n_outputs++;

      void *driving = NULL;
      if (n->flags & NET_F_EFFECTIVE)
         driving = n->resolved + 2 * n->signal->shared.size;

      fprintf(stderr, "%-20s %-5d %-4d %-7d %-7d %-4d ",
              nth == 0 ? tb_get(tb) : "+",
              n->width, n->size, n->n_sources, n_outputs,
              n->net != NULL ? n->net->net_id : 0);

      if (n->net != NULL) {
         void *last_value = n->resolved + n->signal->shared.size;
         if (n->net->event_delta == iteration && n->net->last_event == now)
            fprintf(stderr, "%s -> ", fmt_nexus(n, last_value));
      }

      fputs(fmt_nexus(n, n->resolved), stderr);

      if (driving != NULL)
         fprintf(stderr, " (%s)", fmt_nexus(n, driving));

      fputs("\n", stderr);
   }
}

static void rt_dump_signals(rt_scope_t *scope)
{
   if (scope->signals == NULL && scope->child == NULL)
      return;

   if (scope->kind != SCOPE_SIGNAL && scope->kind != SCOPE_ROOT) {
      const char *sname = istr(scope->name);
      fprintf(stderr, "== %s ", sname);
      for (int pad = 74 - strlen(sname); pad > 0; pad--)
         fputc('=', stderr);
      fputc('\n', stderr);

      fprintf(stderr, "%-20s %5s %4s %7s %7s %-4s %s\n",
              "Signal", "Width", "Size", "Sources", "Outputs",
              "Net", "Value");
   }

   for (rt_signal_t *s = scope->signals; s != NULL; s = s->chain)
      rt_dump_one_signal(scope, s, NULL);

   for (rt_alias_t *a = scope->aliases; a != NULL; a = a->chain)
      rt_dump_one_signal(scope, a->signal, a->where);

   for (rt_scope_t *c = scope->child; c != NULL; c = c->chain)
      rt_dump_signals(c);
}

static rt_source_t *rt_add_source(rt_nexus_t *n, source_kind_t kind)
{
   rt_source_t *src = NULL;
   if (n->n_sources == 0)
      src = &(n->sources);
   else if (n->signal->resolution == NULL
            && (n->sources.tag == SOURCE_DRIVER
                || n->sources.u.port.conv_func == NULL))
      rt_msg(tree_loc(n->signal->where), DIAG_FATAL,
             "unresolved signal %s has multiple sources",
             istr(tree_ident(n->signal->where)));
   else {
      rt_source_t **p;
      for (p = &(n->sources.chain_input); *p; p = &((*p)->chain_input))
         ;
      *p = src = xmalloc(sizeof(rt_source_t));
   }

   // The only interesting values of n_sources are 0, 1, and 2
   if (n->n_sources < UINT8_MAX)
      n->n_sources++;

   src->chain_input  = NULL;
   src->chain_output = NULL;
   src->tag          = kind;

   switch (kind) {
   case SOURCE_DRIVER:
      {
         src->u.driver.proc = NULL;

         waveform_t *w0 = &(src->u.driver.waveforms);
         w0->when  = 0;
         w0->null  = 0;
         w0->next  = NULL;
         w0->value = rt_alloc_value(n);
      }
      break;

   case SOURCE_PORT:
      src->u.port.conv_func = NULL;
      src->u.port.input     = NULL;
      src->u.port.output    = n;
      break;
   }

   return src;
}

static rt_net_t *rt_get_net(rt_nexus_t *nexus)
{
   if (likely(nexus->net != NULL))
      return nexus->net;
   else {
      rt_net_t *net = xmalloc(sizeof(rt_net_t));
      net->pending      = NULL;
      net->last_active  = TIME_HIGH;
      net->last_event   = TIME_HIGH;
      net->active_delta = -1;
      net->event_delta  = -1;
      net->refcnt       = 1;

      static uint32_t next_net_id = 1;
      net->net_id = next_net_id++;

      return (nexus->net = net);
   }
}

static void rt_build_index(rt_signal_t *signal)
{
   const unsigned nexus_w = signal->nexus.width;
   const unsigned signal_w = signal->shared.size / signal->nexus.size;

   TRACE("create index for signal %s", istr(tree_ident(signal->where)));

   signal->index = ihash_new(MIN(MAX((signal_w / nexus_w) * 2, 16), 1024));

   rt_nexus_t *n = &(signal->nexus);
   for (int i = 0, offset = 0; i < signal->n_nexus;
        i++, offset += n->width, n = n->chain)
      ihash_put(signal->index, offset, n);
}

static void rt_clone_waveform(rt_nexus_t *nexus, waveform_t *w_new,
                              waveform_t *w_old, int offset)
{
   w_new->when = w_old->when;
   w_new->null = w_old->null;
   w_new->next = NULL;

   const int split = offset * nexus->size;
   const int oldsz = (offset + nexus->width) * nexus->size;
   const int newsz = nexus->width * nexus->size;

   const void *sp = oldsz <= sizeof(rt_value_t)
      ? w_old->value.bytes : w_old->value.ext;

   void *dp = newsz <= sizeof(rt_value_t)
      ? w_new->value.bytes : w_new->value.ext;

   memcpy(dp, sp + split, newsz);
}

static void rt_clone_source(rt_nexus_t *nexus, rt_source_t *old, int offset,
                            rt_net_t *net)
{
   rt_source_t *new = rt_add_source(nexus, old->tag);

   switch (old->tag) {
   case SOURCE_PORT:
      {
         new->u.port.input = old->u.port.input;

         if (old->u.port.conv_func != NULL) {
            new->u.port.conv_func = old->u.port.conv_func;
            new->u.port.conv_func->refcnt++;
         }
         else {
            if (old->u.port.input->width == offset)
               new->u.port.input = old->u.port.input->chain;  // Cycle breaking
            else {
               rt_nexus_t *n = rt_clone_nexus(old->u.port.input, offset, net);
               new->u.port.input = n;
            }
            RT_ASSERT(new->u.port.input->width == nexus->width);
         }
      }
      break;

   case SOURCE_DRIVER:
      {
         new->u.driver.proc = old->u.driver.proc;

         // Current transaction
         waveform_t *w_new = &(new->u.driver.waveforms);
         waveform_t *w_old = &(old->u.driver.waveforms);
         w_new->when = w_old->when;
         w_new->null = w_old->null;

         rt_clone_waveform(nexus, w_new, w_old, offset);

         // Future transactions
         for (w_old = w_old->next; w_old; w_old = w_old->next) {
            w_new = (w_new->next = rt_alloc(waveform_stack));
            w_new->value = rt_alloc_value(nexus);

            rt_clone_waveform(nexus, w_new, w_old, offset);

            RT_ASSERT(w_old->when >= now);
            deltaq_insert_driver(w_new->when - now, nexus, new);
         }
      }
      break;
   }
}

static rt_nexus_t *rt_clone_nexus(rt_nexus_t *old, int offset, rt_net_t *net)
{
   RT_ASSERT(offset < old->width);

   rt_signal_t *signal = old->signal;
   signal->n_nexus++;

   const size_t oldsz = old->width * old->size;

   rt_nexus_t *new = xcalloc(sizeof(rt_nexus_t));
   new->width        = old->width - offset;
   new->size         = old->size;
   new->signal       = signal;
   new->resolved     = (uint8_t *)old->resolved + offset * old->size;
   new->chain        = old->chain;
   new->flags        = old->flags;

   old->chain = new;
   old->width = offset;

   // Old nexus may be holding large amounts of memory
   free(old->free_value);
   old->free_value = NULL;

   if (old->net != NULL) {
      if (net == NULL) {
         rt_net_t *new_net = rt_get_net(new);
         rt_net_t *old_net = rt_get_net(old);

         new_net->last_active  = old_net->last_active;
         new_net->last_event   = old_net->last_event;
         new_net->active_delta = old_net->active_delta;
         new_net->event_delta  = old_net->event_delta;

         for (sens_list_t *l = old_net->pending; l; l = l->next) {
            sens_list_t *lnew = rt_alloc(sens_list_stack);
            lnew->wake       = l->wake;
            lnew->wakeup_gen = l->wakeup_gen;
            lnew->next       = new_net->pending;
            lnew->reenq      = l->reenq ? &(new_net->pending) : NULL;

            new_net->pending = lnew;
         }

         new->net = net = new_net;
      }
      else {
         new->net = net;
         new->net->refcnt++;
      }
   }

   if (new->chain == NULL)
      nexus_tail = &(new->chain);

   if (old->n_sources > 0) {
      for (rt_source_t *it = &(old->sources); it; it = it->chain_input) {
         rt_clone_source(new, it, offset, net);

         // Resize waveforms in old driver
         if (it->tag == SOURCE_DRIVER && oldsz > sizeof(rt_value_t)) {
            for (waveform_t *w_old = &(it->u.driver.waveforms);
                 w_old; w_old = w_old->next) {
               rt_value_t v_old = rt_alloc_value(old);
               rt_copy_value_ptr(old, &v_old, w_old->value.ext);
               free(w_old->value.ext);
               w_old->value = v_old;
            }
         }
      }
   }

   int nth = 0;
   for (rt_source_t *old_o = old->outputs; old_o;
        old_o = old_o->chain_output, nth++) {

      RT_ASSERT(old_o->tag != SOURCE_DRIVER);

      if (old_o->u.port.conv_func != NULL)
         new->outputs = old_o;
      else {
         rt_nexus_t *out_n;
         if (old_o->u.port.output->width == offset)
            out_n = old_o->u.port.output->chain;   // Cycle breaking
         else
            out_n = rt_clone_nexus(old_o->u.port.output, offset, net);

         for (rt_source_t *s = &(out_n->sources); s; s = s->chain_input) {
            if (s->tag == SOURCE_DRIVER)
               continue;
            else if (s->u.port.input == new || s->u.port.input == old) {
               s->u.port.input = new;
               s->chain_output = new->outputs;
               new->outputs = s;
               break;
            }
         }
      }
   }

   if (signal->index == NULL && signal->n_nexus >= NEXUS_INDEX_MIN)
      rt_build_index(signal);
   else if (signal->index != NULL) {
      const unsigned key =
         (new->resolved - (void *)signal->shared.data) / new->size;
      ihash_put(signal->index, key, new);
   }

   return new;
}

static rt_nexus_t *rt_split_nexus(rt_signal_t *s, int offset, int count)
{
   rt_nexus_t *n0 = &(s->nexus);
   if (likely(offset == 0 && n0->width == count))
      return n0;

   rt_nexus_t *map = NULL;
   if (s->index != NULL && offset > 0) {
      if ((map = ihash_get(s->index, offset))) {
         if (likely(map->width == count))
            return map;
         offset = 0;
      }
      else {
         // Try to find the nexus preceding this offset in the index
         uint64_t key = offset;
         if ((map = ihash_less(s->index, &key))) {
            assert(key < offset);
            offset -= key;
         }
      }
   }

   rt_nexus_t *result = NULL;
   for (rt_nexus_t *it = map ?: &(s->nexus); count > 0; it = it->chain) {
      if (offset >= it->width) {
         offset -= it->width;
         continue;
      }
      else if (offset > 0) {
         rt_clone_nexus(it, offset, NULL);
         offset = 0;
         continue;
      }
      else {
         if (it->width > count)
            rt_clone_nexus(it, count, NULL);

         count -= it->width;

         if (result == NULL)
            result = it;
      }
   }

   return result;
}

static void rt_setup_signal(rt_signal_t *s, tree_t where, unsigned count,
                            unsigned size, net_flags_t flags, unsigned offset)
{
   s->where         = where;
   s->n_nexus       = 1;
   s->shared.size   = count * size;
   s->shared.offset = offset;
   s->flags         = flags | NET_F_LAST_VALUE;
   s->parent        = active_scope;

   *signals_tail = s;
   signals_tail = &(s->chain);

   s->nexus.width      = count;
   s->nexus.size       = size;
   s->nexus.n_sources  = 0;
   s->nexus.resolved   = s->shared.data;
   s->nexus.flags      = flags | NET_F_LAST_VALUE;
   s->nexus.signal     = s;
   s->nexus.net        = NULL;

   *nexus_tail = &(s->nexus);
   nexus_tail = &(s->nexus.chain);

   if (s->nexus.width > MAX_NEXUS_WIDTH) {
      // Chunk up large signals to avoid excessive memory allocation

      const int stride = MAX_NEXUS_WIDTH;

      s->nexus.width = stride;

      for (int p = stride; p < count; p += stride) {
         rt_nexus_t *n = xcalloc(sizeof(rt_nexus_t));
         n->width    = MIN(stride, count - p);
         n->size     = size;
         n->resolved = s->shared.data + p * size;
         n->flags    = s->nexus.flags;
         n->signal   = s;

         s->n_nexus++;

         *nexus_tail = n;
         nexus_tail = &(n->chain);
      }

      if (s->n_nexus >= NEXUS_INDEX_MIN)
         rt_build_index(s);
   }

   profile.n_signals++;
}

static void rt_copy_sub_signals(rt_scope_t *scope, void *buf, value_fn_t fn)
{
   assert(scope->kind == SCOPE_SIGNAL);

   for (rt_signal_t *s = scope->signals; s != NULL; s = s->chain) {
      rt_nexus_t *n = &(s->nexus);
      for (unsigned i = 0; i < s->n_nexus; i++, n = n->chain) {
         ptrdiff_t o = (uint8_t *)n->resolved - s->shared.data;
         memcpy(buf + s->shared.offset + o, (*fn)(n), n->size * n->width);
      }
   }

   for (rt_scope_t *s = scope->child; s != NULL; s = s->chain)
      rt_copy_sub_signals(s, buf, fn);
}

static void rt_copy_sub_signal_sources(rt_scope_t *scope, void *buf, int stride)
{
   assert(scope->kind == SCOPE_SIGNAL);

   for (rt_signal_t *s = scope->signals; s != NULL; s = s->chain) {
      rt_nexus_t *n = &(s->nexus);
      for (unsigned i = 0; i < s->n_nexus; i++) {
         unsigned o = 0;
         for (rt_source_t *src = &(n->sources); src; src = src->chain_input) {
            const void *data = rt_source_value(n, src);
            if (data == NULL)
               continue;

            memcpy(buf + s->shared.offset + (o++ * stride),
                   data, n->size * n->width);
         }
      }
   }

   for (rt_scope_t *s = scope->child; s != NULL; s = s->chain)
      rt_copy_sub_signal_sources(s, buf, stride);
}

static void *rt_composite_signal(rt_signal_t *signal, size_t *psz, value_fn_t fn)
{
   assert(signal->parent->kind == SCOPE_SIGNAL);

   rt_scope_t *root = signal->parent;
   while (root->parent->kind == SCOPE_SIGNAL)
      root = root->parent;

   *psz = root->size;

   char *buf = xmalloc(root->size);
   rt_copy_sub_signals(root, buf, fn);
   return buf;
}

////////////////////////////////////////////////////////////////////////////////
// Runtime support functions

DLLEXPORT tlab_t __nvc_tlab = {};   // TODO: this should be thread-local

DLLEXPORT
void _sched_process(int64_t delay)
{
   TRACE("_sched_process delay=%s", fmt_time(delay));
   deltaq_insert_proc(delay, active_proc);
}

DLLEXPORT
void _sched_waveform_s(sig_shared_t *ss, uint32_t offset, uint64_t scalar,
                       int64_t after, int64_t reject)
{
   rt_signal_t *s = container_of(ss, rt_signal_t, shared);

   TRACE("_sched_waveform_s %s+%d value=%"PRIi64" after=%s reject=%s",
         istr(tree_ident(s->where)), offset, scalar, fmt_time(after),
         fmt_time(reject));

   rt_check_postponed(after);

   rt_nexus_t *n = rt_split_nexus(s, offset, 1);

   rt_value_t value = rt_alloc_value(n);
   value.qword = scalar;

   rt_sched_driver(n, after, reject, value, false);
}

DLLEXPORT
void _sched_waveform(sig_shared_t *ss, uint32_t offset, void *values,
                     int32_t count, int64_t after, int64_t reject)
{
   rt_signal_t *s = container_of(ss, rt_signal_t, shared);

   TRACE("_sched_waveform %s+%d value=%s count=%d after=%s reject=%s",
         istr(tree_ident(s->where)), offset, fmt_values(values, count),
         count, fmt_time(after), fmt_time(reject));

   rt_check_postponed(after);

   rt_nexus_t *n = rt_split_nexus(s, offset, count);
   char *vptr = values;
   for (; count > 0; n = n->chain) {
      count -= n->width;
      RT_ASSERT(count >= 0);

      const size_t valuesz = n->width * n->size;
      rt_value_t value = rt_alloc_value(n);
      rt_copy_value_ptr(n, &value, vptr);
      vptr += valuesz;

      rt_sched_driver(n, after, reject, value, false);
   }
}

DLLEXPORT
void _disconnect(sig_shared_t *ss, uint32_t offset, int32_t count,
                 int64_t after, int64_t reject)
{
   rt_signal_t *s = container_of(ss, rt_signal_t, shared);

   TRACE("_disconnect %s+%d len=%d after=%s reject=%s",
         istr(tree_ident(s->where)), offset, count, fmt_time(after),
         fmt_time(reject));

   rt_check_postponed(after);

   rt_nexus_t *n = rt_split_nexus(s, offset, count);
   for (; count > 0; n = n->chain) {
      count -= n->width;
      RT_ASSERT(count >= 0);

      rt_value_t null = {};
      rt_sched_driver(n, after, reject, null, true);
   }
}

DLLEXPORT
void __nvc_force(sig_shared_t *ss, uint32_t offset, int32_t count, void *values)
{
   rt_signal_t *s = container_of(ss, rt_signal_t, shared);

   TRACE("force signal %s+%d value=%s count=%d", istr(tree_ident(s->where)),
         offset, fmt_values(values, count), count);

   rt_check_postponed(0);

   rt_nexus_t *n = rt_split_nexus(s, offset, count);
   char *vptr = values;
   for (; count > 0; n = n->chain) {
      count -= n->width;
      RT_ASSERT(count >= 0);

      if (!(n->flags & NET_F_FORCED)) {
         n->flags |= NET_F_FORCED;
         n->forcing = rt_alloc_value(n);
      }

      rt_copy_value_ptr(n, &(n->forcing), vptr);
      vptr += n->width * n->size;

      deltaq_insert_driver(0, n, NULL);
   }
}

DLLEXPORT
void __nvc_release(sig_shared_t *ss, uint32_t offset, int32_t count)
{
   rt_signal_t *s = container_of(ss, rt_signal_t, shared);

   TRACE("release signal %s+%d count=%d", istr(tree_ident(s->where)),
         offset, count);

   rt_check_postponed(0);

   rt_nexus_t *n = rt_split_nexus(s, offset, count);
   for (; count > 0; n = n->chain) {
      count -= n->width;
      RT_ASSERT(count >= 0);

      if (n->flags & NET_F_FORCED) {
         n->flags &= ~NET_F_FORCED;
         rt_free_value(n, n->forcing);
         n->forcing.qword = 0;
      }

      deltaq_insert_driver(0, n, NULL);
   }
}

DLLEXPORT
void _sched_event(sig_shared_t *ss, uint32_t offset, int32_t count, bool recur,
                  sig_shared_t *wake_ss)
{
   rt_signal_t *s = container_of(ss, rt_signal_t, shared);

   TRACE("_sched_event %s+%d count=%d recur=%d proc %s",
         istr(tree_ident(s->where)), offset, count, recur,
         wake_ss ? "(implicit)" : istr(active_proc->name));

   rt_wakeable_t *wake;
   if (wake_ss != NULL)
      wake = &(container_of(wake_ss, rt_implicit_t, signal.shared)->wakeable);
   else
      wake = &(active_proc->wakeable);

   rt_nexus_t *n = rt_split_nexus(s, offset, count);
   for (; count > 0; n = n->chain) {
      rt_sched_event(&(rt_get_net(n)->pending), wake, recur);

      count -= n->width;
      RT_ASSERT(count >= 0);
   }
}

DLLEXPORT
void __nvc_claim_tlab(void)
{
   TRACE("claiming TLAB for private use (used %zu/%d)",
         __nvc_tlab.alloc - __nvc_tlab.base, TLAB_SIZE);

   assert(tlab_valid(__nvc_tlab));
   assert(__nvc_tlab.alloc > __nvc_tlab.base);

   tlab_move(__nvc_tlab, active_proc->tlab);
}

DLLEXPORT
void __nvc_drive_signal(sig_shared_t *ss, uint32_t offset, int32_t count)
{
   rt_signal_t *s = container_of(ss, rt_signal_t, shared);

   TRACE("drive signal %s+%d count=%d", istr(tree_ident(s->where)),
         offset, count);

   rt_nexus_t *n = rt_split_nexus(s, offset, count);
   for (; count > 0; n = n->chain) {
      rt_source_t *s;
      for (s = &(n->sources); s; s = s->chain_input) {
         if (s->tag == SOURCE_DRIVER && s->u.driver.proc == active_proc)
            break;
      }

      if (s == NULL) {
         s = rt_add_source(n, SOURCE_DRIVER);
         s->u.driver.proc = active_proc;
      }

      count -= n->width;
      RT_ASSERT(count >= 0);
   }
}

DLLEXPORT
void __nvc_alias_signal(sig_shared_t *ss, DEBUG_LOCUS(locus))
{
   rt_signal_t *s = container_of(ss, rt_signal_t, shared);
   tree_t where = rt_locus_to_tree(locus_unit, locus_offset);

   TRACE("alias signal %s to %s", istr(tree_ident(s->where)),
         istr(tree_ident(where)));

   rt_alias_t *a = xcalloc(sizeof(rt_alias_t));
   a->where  = where;
   a->signal = s;
   a->chain  = active_scope->aliases;

   active_scope->aliases = a;
}

DLLEXPORT
void __nvc_resolve_signal(sig_shared_t *ss, rt_resolution_t *resolution)
{
   rt_signal_t *s = container_of(ss, rt_signal_t, shared);

   TRACE("resolve signal %s", istr(tree_ident(s->where)));

   s->resolution = rt_memo_resolution_fn(s, resolution);

   // Copy R_IDENT into the nexus flags to avoid rt_resolve_nexus_fast
   // having to dereference the resolution pointer in the common case
   if (s->resolution->flags & R_IDENT) {
      s->flags |= NET_F_R_IDENT;

      rt_nexus_t *n = &(s->nexus);
      for (int i = 0; i < s->n_nexus; i++, n = n->chain)
         n->flags |= NET_F_R_IDENT;
   }
}

DLLEXPORT
void __nvc_push_scope(DEBUG_LOCUS(locus), int32_t size)
{
   tree_t where = rt_locus_to_tree(locus_unit, locus_offset);

   TRACE("push scope %s size=%d", istr(tree_ident(where)), size);

   ident_t name = tree_ident(where);
   if (active_scope->kind == SCOPE_SIGNAL)
      name = ident_prefix(active_scope->name, name, '.');

   rt_scope_t *s = xcalloc(sizeof(rt_scope_t));
   s->where    = where;
   s->name     = name;
   s->kind     = SCOPE_SIGNAL;
   s->parent   = active_scope;
   s->chain    = active_scope->child;
   s->size     = size;
   s->privdata = mptr_new(mspace, "push scope privdata");

   type_t type = tree_type(where);
   if (type_kind(type) == T_SUBTYPE && type_has_resolution(type))
      s->flags |= SCOPE_F_RESOLVED;

   active_scope->child = s;
   active_scope = s;

   signals_tail = &(s->signals);
}

DLLEXPORT
void __nvc_pop_scope(void)
{
   TRACE("pop scope %s", istr(tree_ident(active_scope->where)));

   if (unlikely(active_scope->kind != SCOPE_SIGNAL))
      fatal_trace("cannot pop non-signal scope");

   int offset = INT_MAX;
   for (rt_scope_t *s = active_scope->child; s; s = s->chain)
      offset = MIN(offset, s->offset);
   for (rt_signal_t *s = active_scope->signals; s; s = s->chain)
      offset = MIN(offset, s->shared.offset);
   active_scope->offset = offset;

   active_scope = active_scope->parent;

   for (signals_tail = &(active_scope->signals);
        *signals_tail != NULL;
        signals_tail = &((*signals_tail)->chain))
      ;
}

DLLEXPORT
sig_shared_t *_init_signal(uint32_t count, uint32_t size, const uint8_t *values,
                           int32_t flags, DEBUG_LOCUS(locus), int32_t offset)
{
   tree_t where = rt_locus_to_tree(locus_unit, locus_offset);

   TRACE("init signal %s count=%d size=%d values=%s flags=%x offset=%d",
         istr(tree_ident(where)), count, size,
         fmt_values(values, size * count), flags, offset);

   const size_t datasz = MAX(3 * count * size, 8);
   rt_signal_t *s = xcalloc_flex(sizeof(rt_signal_t), 1, datasz);
   rt_setup_signal(s, where, count, size, flags, offset);

   memcpy(s->shared.data, values, s->shared.size);

   // The driving value area is also used to save the default value
   void *driving = s->shared.data + 2*s->shared.size;
   memcpy(driving, values, s->shared.size);

   return &(s->shared);
}

DLLEXPORT
sig_shared_t *_implicit_signal(uint32_t count, uint32_t size,
                               DEBUG_LOCUS(locus), uint32_t kind,
                               ffi_closure_t *closure)
{
   tree_t where = rt_locus_to_tree(locus_unit, locus_offset);

   TRACE("_implicit_signal %s count=%d size=%d kind=%d",
         istr(tree_ident(where)), count, size, kind);

   const size_t datasz = MAX(2 * count * size, 8);
   rt_implicit_t *imp = xcalloc_flex(sizeof(rt_implicit_t), 1, datasz);
   rt_setup_signal(&(imp->signal), where, count, size, NET_F_IMPLICIT, 0);

   ffi_closure_t *copy = xmalloc(sizeof(ffi_closure_t));
   *copy = *closure;
   copy->refcnt = 1;

   imp->closure = copy;
   imp->wakeable.kind = W_IMPLICIT;

   int8_t r;
   ffi_call(imp->closure, NULL, 0, &r, sizeof(r));

   RT_ASSERT(size * count == 1);
   memcpy(imp->signal.shared.data, &r, imp->signal.shared.size);

   return &(imp->signal.shared);
}

DLLEXPORT
void __nvc_map_signal(sig_shared_t *src_ss, uint32_t src_offset,
                      sig_shared_t *dst_ss, uint32_t dst_offset,
                      uint32_t src_count, uint32_t dst_count,
                      ffi_closure_t *closure)
{
   rt_signal_t *src_s = container_of(src_ss, rt_signal_t, shared);
   rt_signal_t *dst_s = container_of(dst_ss, rt_signal_t, shared);

   TRACE("map signal %s+%d to %s+%d count %d/%d%s",
         istr(tree_ident(src_s->where)), src_offset,
         istr(tree_ident(dst_s->where)), dst_offset,
         src_count, dst_count, closure ? " converted" : "");

   assert(src_count == dst_count || closure != NULL);

   rt_conv_func_t *conv_func = NULL;
   if (closure != NULL) {
      size_t bufsz = dst_s->shared.size;
      if (dst_s->parent->kind == SCOPE_SIGNAL) {
         rt_scope_t *root = dst_s->parent;
         while (root->parent->kind == SCOPE_SIGNAL)
            root = root->parent;
         bufsz = root->size;
      }

      TRACE("need %zu bytes for conversion function buffer", bufsz);

      conv_func = xmalloc_flex(sizeof(rt_conv_func_t), 1, bufsz);
      conv_func->closure = *closure;
      conv_func->refcnt  = 0;
      conv_func->bufsz   = bufsz;
   }

   rt_nexus_t *src_n = rt_split_nexus(src_s, src_offset, src_count);
   rt_nexus_t *dst_n = rt_split_nexus(dst_s, dst_offset, dst_count);

   while (src_count > 0 && dst_count > 0) {
      if (src_n->width > dst_n->width && closure == NULL)
         rt_clone_nexus(src_n, dst_n->width, NULL);
      else if (src_n->width < dst_n->width && closure == NULL)
         rt_clone_nexus(dst_n, src_n->width, NULL);

      assert(src_n->width == dst_n->width || closure != NULL);
      assert(src_n->size == dst_n->size || closure != NULL);

      // For inout ports and ports with conversion functions the driving
      // value and the effective value may be different so 'EVENT and
      // 'ACTIVE are not necessarily equal for all signals attached to
      // the same net
      if (((dst_n->flags | src_n->flags) & NET_F_EFFECTIVE) == 0) {
         if (src_n->net == NULL) {
            src_n->net = rt_get_net(dst_n);
            src_n->net->refcnt++;
         }
         else {
            assert(dst_n->net == NULL);
            dst_n->net = rt_get_net(src_n);
            dst_n->net->refcnt++;
         }
      }
      else {
         src_n->flags |= NET_F_EFFECTIVE;
         dst_n->flags |= NET_F_EFFECTIVE;
      }

      rt_source_t *port = rt_add_source(dst_n, SOURCE_PORT);
      port->u.port.input = src_n;

      if (conv_func != NULL) {
         port->u.port.conv_func = conv_func;
         conv_func->refcnt++;
         src_n->flags |= NET_F_EFFECTIVE;
         dst_n->flags |= NET_F_EFFECTIVE;
      }

      port->chain_output = src_n->outputs;
      src_n->outputs = port;

      src_count -= src_n->width;
      dst_count -= dst_n->width;
      RT_ASSERT(src_count >= 0);
      RT_ASSERT(dst_count >= 0);

      src_n = src_n->chain;
      dst_n = dst_n->chain;
   }
}

DLLEXPORT
void __nvc_map_const(sig_shared_t *ss, uint32_t offset,
                     const uint8_t *values, uint32_t count)
{
   rt_signal_t *s = container_of(ss, rt_signal_t, shared);

   TRACE("map const %s to %s+%d count %d", fmt_values(values, count),
         istr(tree_ident(s->where)), offset, count);

   rt_nexus_t *n = rt_split_nexus(s, offset, count);
   for (; count > 0; n = n->chain) {
      void *driving = n->resolved + 2*n->signal->shared.size;
      memcpy(driving, values, n->width * n->size);

      memcpy(n->resolved, values, n->width * n->size);
      values += n->width * n->size;

      count -= n->width;
      RT_ASSERT(count >= 0);
   }
}

DLLEXPORT
void __nvc_assert_fail(const uint8_t *msg, int32_t msg_len, int8_t severity,
                       int64_t hint_left, int64_t hint_right, int8_t hint_valid,
                       DEBUG_LOCUS(locus))
{
   // LRM 93 section 8.2
   // The error message consists of at least
   // a) An indication that this message is from an assertion
   // b) The value of the severity level
   // c) The value of the message string
   // d) The name of the design unit containing the assertion

   RT_ASSERT(severity <= SEVERITY_FAILURE);

   static const char *levels[] = {
      "Note", "Warning", "Error", "Failure"
   };

   if (init_side_effect != SIDE_EFFECT_ALLOW) {
      init_side_effect = SIDE_EFFECT_OCCURRED;
      return;
   }

   tree_t where = rt_locus_to_tree(locus_unit, locus_offset);

   char tmbuf[64];
   rt_fmt_now(tmbuf, sizeof(tmbuf));

   const diag_level_t level = diag_severity(severity);

   diag_t *d = diag_new(level, tree_loc(where));
   if (msg == NULL)
      diag_printf(d, "%s: Assertion %s: Assertion violation.",
                  tmbuf, levels[severity]);
   else {
      diag_printf(d, "%s: Assertion %s: %.*s",
                  tmbuf, levels[severity], msg_len, msg);

      // Assume we don't want to dump the source code if the user
      // provided their own message
      diag_show_source(d, false);
   }

   if (hint_valid) {
      assert(tree_kind(where) == T_FCALL);
      type_t p0_type = tree_type(tree_value(tree_param(where, 0)));
      type_t p1_type = tree_type(tree_value(tree_param(where, 1)));

      LOCAL_TEXT_BUF tb = tb_new();
      to_string(tb, p0_type, hint_left);
      switch (tree_subkind(tree_ref(where))) {
      case S_SCALAR_EQ:  tb_cat(tb, " = "); break;
      case S_SCALAR_NEQ: tb_cat(tb, " /= "); break;
      case S_SCALAR_LT:  tb_cat(tb, " < "); break;
      case S_SCALAR_GT:  tb_cat(tb, " > "); break;
      case S_SCALAR_LE:  tb_cat(tb, " <= "); break;
      case S_SCALAR_GE:  tb_cat(tb, " >= "); break;
      default: tb_cat(tb, " <?> "); break;
      }
      to_string(tb, p1_type, hint_right);
      tb_cat(tb, " is false");

      diag_hint(d, tree_loc(where), "%s", tb_get(tb));
   }

   rt_diag_trace(d);
   diag_emit(d);

   if (level == DIAG_FATAL)
      rt_abort_sim(EXIT_FAILURE);
}

DLLEXPORT
void __nvc_report(const uint8_t *msg, int32_t msg_len, int8_t severity,
                  DEBUG_LOCUS(locus))
{
   RT_ASSERT(severity <= SEVERITY_FAILURE);

   static const char *levels[] = {
      "Note", "Warning", "Error", "Failure"
   };

   if (init_side_effect != SIDE_EFFECT_ALLOW) {
      init_side_effect = SIDE_EFFECT_OCCURRED;
      return;
   }

   char tmbuf[64];
   rt_fmt_now(tmbuf, sizeof(tmbuf));

   tree_t where = rt_locus_to_tree(locus_unit, locus_offset);

   const diag_level_t level = diag_severity(severity);

   diag_t *d = diag_new(level, tree_loc(where));
   diag_printf(d, "%s: Report %s: %.*s", tmbuf, levels[severity], msg_len, msg);
   diag_show_source(d, false);
   rt_diag_trace(d);
   diag_emit(d);

   if (level == DIAG_FATAL)
      rt_abort_sim(EXIT_FAILURE);
}

DLLEXPORT
void __nvc_index_fail(int32_t value, int32_t left, int32_t right, int8_t dir,
                      DEBUG_LOCUS(locus), DEBUG_LOCUS(hint))
{
   tree_t where = rt_locus_to_tree(locus_unit, locus_offset);
   tree_t hint = rt_locus_to_tree(hint_unit, hint_offset);

   type_t type = tree_type(hint);

   LOCAL_TEXT_BUF tb = tb_new();
   tb_cat(tb, "index ");
   to_string(tb, type, value);
   tb_printf(tb, " outside of %s range ", type_pp(type));
   to_string(tb, type, left);
   tb_cat(tb, dir == RANGE_TO ? " to " : " downto ");
   to_string(tb, type, right);

   rt_msg(tree_loc(where), DIAG_FATAL, "%s", tb_get(tb));
}

DLLEXPORT
void __nvc_range_fail(int64_t value, int64_t left, int64_t right, int8_t dir,
                      DEBUG_LOCUS(locus), DEBUG_LOCUS(hint))
{
   tree_t where = rt_locus_to_tree(locus_unit, locus_offset);
   tree_t hint = rt_locus_to_tree(hint_unit, hint_offset);

   type_t type = tree_type(hint);

   LOCAL_TEXT_BUF tb = tb_new();
   tb_cat(tb, "value ");
   to_string(tb, type, value);
   tb_printf(tb, " outside of %s range ", type_pp(type));
   to_string(tb, type, left);
   tb_cat(tb, dir == RANGE_TO ? " to " : " downto ");
   to_string(tb, type, right);

   switch (tree_kind(hint)) {
   case T_SIGNAL_DECL:
   case T_CONST_DECL:
   case T_VAR_DECL:
   case T_REF:
      tb_printf(tb, " for %s %s", class_str(class_of(hint)),
                istr(tree_ident(hint)));
      break;
   case T_PORT_DECL:
      tb_printf(tb, " for port %s", istr(tree_ident(hint)));
      break;
   case T_PARAM_DECL:
      tb_printf(tb, " for parameter %s", istr(tree_ident(hint)));
      break;
   case T_GENERIC_DECL:
      tb_printf(tb, " for generic %s", istr(tree_ident(hint)));
      break;
   case T_ATTR_REF:
      tb_printf(tb, " for attribute '%s", istr(tree_ident(hint)));
      break;
   default:
      break;
   }

   rt_msg(tree_loc(where), DIAG_FATAL, "%s", tb_get(tb));
}

DLLEXPORT
void __nvc_length_fail(int32_t left, int32_t right, int32_t dim,
                       DEBUG_LOCUS(locus))
{
   tree_t where = rt_locus_to_tree(locus_unit, locus_offset);
   const tree_kind_t kind = tree_kind(where);

   LOCAL_TEXT_BUF tb = tb_new();
   if (kind == T_PORT_DECL || kind == T_GENERIC_DECL || kind == T_PARAM_DECL)
      tb_cat(tb, "actual");
   else if (kind == T_CASE || kind == T_MATCH_CASE)
      tb_cat(tb, "expression");
   else if (kind == T_ASSOC)
      tb_cat(tb, "choice");
   else
      tb_cat(tb, "value");
   tb_printf(tb, " length %d", right);
   if (dim > 0)
      tb_printf(tb, " for dimension %d", dim);
   tb_cat(tb, " does not match ");

   switch (kind) {
   case T_PORT_DECL:
      tb_printf(tb, "port %s", istr(tree_ident(where)));
      break;
   case T_PARAM_DECL:
      tb_printf(tb, "parameter %s", istr(tree_ident(where)));
      break;
   case T_GENERIC_DECL:
      tb_printf(tb, "generic %s", istr(tree_ident(where)));
      break;
   case T_VAR_DECL:
      tb_printf(tb, "variable %s", istr(tree_ident(where)));
      break;
   case T_SIGNAL_DECL:
      tb_printf(tb, "signal %s", istr(tree_ident(where)));
      break;
   case T_REF:
      tb_printf(tb, "%s %s", class_str(class_of(where)),
                istr(tree_ident(where)));
      break;
   case T_FIELD_DECL:
      tb_printf(tb, "field %s", istr(tree_ident(where)));
      break;
   case T_ALIAS:
      tb_printf(tb, "alias %s", istr(tree_ident(where)));
      break;
   case T_CASE:
   case T_MATCH_CASE:
      tb_cat(tb, "case choice");
      break;
   case T_ASSOC:
      tb_cat(tb, "expected");
      break;
   default:
      tb_cat(tb, "target");
      break;
   }

   tb_printf(tb, " length %d", left);

   rt_msg(tree_loc(where), DIAG_FATAL, "%s", tb_get(tb));
}

DLLEXPORT
void __nvc_exponent_fail(int32_t value, DEBUG_LOCUS(locus))
{
   tree_t where = rt_locus_to_tree(locus_unit, locus_offset);

   rt_msg(tree_loc(where), DIAG_FATAL, "negative exponent %d only "
          "allowed for floating-point types", value);
}

DLLEXPORT
void __nvc_elab_order_fail(DEBUG_LOCUS(locus))
{
   tree_t where = rt_locus_to_tree(locus_unit, locus_offset);
   assert(tree_kind(where) == T_EXTERNAL_NAME);

   rt_msg(tree_loc(where), DIAG_FATAL, "%s %s has not yet been elaborated",
          class_str(tree_class(where)), istr(tree_ident(tree_ref(where))));
}

DLLEXPORT
void __nvc_unreachable(DEBUG_LOCUS(locus))
{
   tree_t where = rt_locus_to_tree(locus_unit, locus_offset);
   if (where != NULL && tree_kind(where) == T_FUNC_BODY)
      rt_msg(tree_loc(where), DIAG_FATAL, "function %s did not return a value",
             istr(tree_ident(where)));
   else
      rt_msg(NULL, DIAG_FATAL, "executed unreachable instruction");
}

DLLEXPORT
void *__nvc_mspace_alloc(uint32_t size, uint32_t nelems)
{
   uint32_t total;
   if (unlikely(__builtin_mul_overflow(nelems, size, &total))) {
      rt_msg(NULL, DIAG_FATAL, "attempting to allocate %"PRIu64" byte object "
             "which is larger than the maximum supported %u bytes",
             (uint64_t)size * (uint64_t)nelems, UINT32_MAX);
      __builtin_unreachable();
   }
   else
      return mspace_alloc(mspace, total);
}

DLLEXPORT
void _canon_value(const uint8_t *raw_str, int32_t str_len, ffi_uarray_t *u)
{
   char *buf = rt_tlab_alloc(str_len), *p = buf;
   int pos = 0;

   for (; pos < str_len && isspace((int)raw_str[pos]); pos++)
      ;

   bool upcase = true;
   for (; pos < str_len && !isspace((int)raw_str[pos]); pos++) {
      if (raw_str[pos] == '\'')
         upcase = !upcase;

      *p++ = upcase ? toupper((int)raw_str[pos]) : raw_str[pos];
   }

   for (; pos < str_len; pos++) {
      if (!isspace((int)raw_str[pos])) {
         rt_msg(NULL, DIAG_FATAL, "found invalid characters \"%.*s\" after "
                "value \"%.*s\"", (int)(str_len - pos), raw_str + pos, str_len,
                (const char *)raw_str);
      }
   }

   *u = wrap_str(buf, p - buf);
}

DLLEXPORT
void _int_to_string(int64_t value, ffi_uarray_t *u)
{
   char *buf = rt_tlab_alloc(20);
   size_t len = checked_sprintf(buf, 20, "%"PRIi64, value);

   *u = wrap_str(buf, len);
}

DLLEXPORT
void _real_to_string(double value, ffi_uarray_t *u)
{
   char *buf = rt_tlab_alloc(32);
   size_t len = checked_sprintf(buf, 32, "%.*g", DBL_DIG, value);

   *u = wrap_str(buf, len);
}

DLLEXPORT
int64_t _string_to_int(const uint8_t *raw_str, int32_t str_len, int32_t *used)
{
   const char *p = (const char *)raw_str;
   const char *endp = p + str_len;

   for (; p < endp && isspace((int)*p); p++)
      ;

   const bool is_negative = p < endp && *p == '-';
   if (is_negative) p++;

   int64_t value = INT64_MIN;
   int num_digits = 0;
   while (p < endp && (isdigit((int)*p) || *p == '_')) {
      if (*p != '_') {
         value *= 10;
         value += (*p - '0');
         num_digits++;
      }
      ++p;
   }

   if (is_negative) value = -value;

   if (num_digits == 0)
      rt_msg(NULL, DIAG_FATAL, "invalid integer value "
             "\"%.*s\"", str_len, (const char *)raw_str);

   if (used != NULL)
      *used = p - (const char *)raw_str;
   else {
      for (; p < endp && *p != '\0'; p++) {
         if (!isspace((int)*p)) {
            rt_msg(NULL, DIAG_FATAL, "found invalid characters \"%.*s\" after "
                   "value \"%.*s\"", (int)(endp - p), p, str_len,
                   (const char *)raw_str);
         }
      }
   }

   return value;
}

DLLEXPORT
double _string_to_real(const uint8_t *raw_str, int32_t str_len, uint8_t **tail)
{
   char *null LOCAL = xmalloc(str_len + 1);
   memcpy(null, raw_str, str_len);
   null[str_len] = '\0';

   char *p = null;
   for (; p < p + str_len && isspace((int)*p); p++)
      ;

   double value = strtod(p, &p);

   if (*p != '\0' && !isspace((int)*p))
      rt_msg(NULL, DIAG_FATAL, "invalid real value "
             "\"%.*s\"", str_len, (const char *)raw_str);

   if (tail != NULL)
      *tail = (uint8_t *)p;
   else {
      for (; p < null + str_len && *p != '\0'; p++) {
         if (!isspace((int)*p)) {
            rt_msg(NULL, DIAG_FATAL, "found invalid characters \"%.*s\" after "
                   "value \"%.*s\"", (int)(null + str_len - p), p, str_len,
                   (const char *)raw_str);
         }
      }
   }

   return value;
}

DLLEXPORT
void __nvc_div_zero(DEBUG_LOCUS(locus))
{
   tree_t where = rt_locus_to_tree(locus_unit, locus_offset);
   rt_msg(tree_loc(where), DIAG_FATAL, "division by zero");
}

DLLEXPORT
void __nvc_null_deref(DEBUG_LOCUS(locus))
{
   tree_t where = rt_locus_to_tree(locus_unit, locus_offset);
   rt_msg(tree_loc(where), DIAG_FATAL, "null access dereference");
}

DLLEXPORT
void __nvc_overflow(int64_t lhs, int64_t rhs, DEBUG_LOCUS(locus))
{
   tree_t where = rt_locus_to_tree(locus_unit, locus_offset);

   const char *op = "??";
   if (tree_kind(where) == T_FCALL) {
      switch (tree_subkind(tree_ref(where))) {
      case S_ADD: op = "+"; break;
      case S_MUL: op = "*"; break;
      case S_SUB: op = "-"; break;
      }
   }

   rt_msg(tree_loc(where), DIAG_FATAL, "result of %"PRIi64" %s %"PRIi64
          " cannot be represented as %s", lhs, op, rhs,
          type_pp(tree_type(where)));
}

DLLEXPORT
bool _nvc_ieee_warnings(void)
{
   return opt_get_int(OPT_IEEE_WARNINGS);
}

DLLEXPORT
int _nvc_current_delta(void)
{
   return iteration;
}

DLLEXPORT
int64_t _std_standard_now(void)
{
   return now;
}

DLLEXPORT
void _std_to_string_time(int64_t value, int64_t unit, ffi_uarray_t *u)
{
   const char *unit_str = "";
   switch (unit) {
   case 1ll: unit_str = "fs"; break;
   case 1000ll: unit_str = "ps"; break;
   case 1000000ll: unit_str = "ns"; break;
   case 1000000000ll: unit_str = "us"; break;
   case 1000000000000ll: unit_str = "ms"; break;
   case 1000000000000000ll: unit_str = "sec"; break;
   case 60000000000000000ll: unit_str = "min"; break;
   case 3600000000000000000ll: unit_str = "hr"; break;
   default:
      rt_msg(NULL, DIAG_FATAL, "invalid UNIT argument %"PRIi64" in TO_STRING",
             unit);
   }

   size_t max_len = 16 + strlen(unit_str) + 1;
   char *buf = rt_tlab_alloc(max_len);

   size_t len;
   if (value % unit == 0)
      len = checked_sprintf(buf, max_len, "%"PRIi64" %s",
                            value / unit, unit_str);
   else
      len = checked_sprintf(buf, max_len, "%g %s",
                            (double)value / (double)unit, unit_str);

   *u = wrap_str(buf, len);
}

DLLEXPORT
void _std_to_string_real_digits(double value, int32_t digits, ffi_uarray_t *u)
{
   size_t max_len = 32;
   char *buf = rt_tlab_alloc(max_len);

   size_t len;
   if (digits == 0)
      len = checked_sprintf(buf, max_len, "%.17g", value);
   else
      len = checked_sprintf(buf, max_len, "%.*f", digits, value);

   *u = wrap_str(buf, len);
}

DLLEXPORT
void _std_to_string_real_format(double value, EXPLODED_UARRAY(fmt),
                                ffi_uarray_t *u)
{
   char *LOCAL fmt_cstr = xmalloc(fmt_length + 1);
   memcpy(fmt_cstr, fmt_ptr, fmt_length);
   fmt_cstr[fmt_length] = '\0';

   if (fmt_cstr[0] != '%')
      rt_msg(NULL, DIAG_FATAL, "conversion specification must start with '%%'");

   for (const char *p = fmt_cstr + 1; *p; p++) {
      switch (*p) {
      case 'e': case 'E': case 'f': case 'F': case 'g': case 'G':
      case 'a': case 'A':
         continue;
      case '0'...'9':
         continue;
      case '.': case '-':
         continue;
      default:
         rt_msg(NULL, DIAG_FATAL, "illegal character '%c' in format \"%s\"",
                *p, fmt_cstr + 1);
      }
   }

   size_t max_len = 64;
   char *buf = rt_tlab_alloc(max_len);
   size_t len = checked_sprintf(buf, max_len, fmt_cstr, value);
   *u = wrap_str(buf, len);
}

DLLEXPORT
void _std_to_hstring_bit_vec(EXPLODED_UARRAY(vec), ffi_uarray_t *u)
{
   const ffi_uarray_t vec = { vec_ptr, { { vec_left, vec_length } } };
   *u = bit_vec_to_string(&vec, 4);
}

DLLEXPORT
void _std_to_ostring_bit_vec(EXPLODED_UARRAY(vec), ffi_uarray_t *u)
{
   const ffi_uarray_t vec = { vec_ptr, { { vec_left, vec_length } } };
   *u = bit_vec_to_string(&vec, 3);
}

DLLEXPORT
void _std_env_stop(int32_t finish, int32_t have_status, int32_t status)
{
   if (have_status)
      notef("%s called with status %d", finish ? "FINISH" : "STOP", status);
   else
      notef("%s called", finish ? "FINISH" : "STOP");

   rt_abort_sim(status);
}

DLLEXPORT
void _debug_out(intptr_t val, int32_t reg)
{
   printf("DEBUG: r%d val=%"PRIxPTR"\n", reg, val);
   fflush(stdout);
}

DLLEXPORT
void _debug_dump(const uint8_t *ptr, int32_t len)
{
   printf("---- %p ----\n", ptr);

   if (ptr != NULL) {
      for (int i = 0; i < len; i++)
         printf("%02x%c", ptr[i], (i % 8 == 7) ? '\n' : ' ');
      if (len % 8 != 0)
         printf("\n");
   }

   fflush(stdout);
}

DLLEXPORT
int64_t _last_event(sig_shared_t *ss, uint32_t offset, int32_t count)
{
   rt_signal_t *s = container_of(ss, rt_signal_t, shared);

   TRACE("_last_event %s offset=%d count=%d",
         istr(tree_ident(s->where)), offset, count);

   int64_t last = TIME_HIGH;

   rt_nexus_t *n = rt_split_nexus(s, offset, count);
   for (; count > 0; n = n->chain) {
      rt_net_t *net = rt_get_net(n);
      if (net->last_event <= now)
         last = MIN(last, now - net->last_event);

      count -= n->width;
      RT_ASSERT(count >= 0);
   }

   return last;
}

DLLEXPORT
int64_t _last_active(sig_shared_t *ss, uint32_t offset, int32_t count)
{
   rt_signal_t *s = container_of(ss, rt_signal_t, shared);

   TRACE("_last_active %s offset=%d count=%d",
         istr(tree_ident(s->where)), offset, count);

   int64_t last = TIME_HIGH;

   rt_nexus_t *n = rt_split_nexus(s, offset, count);
   for (; count > 0; n = n->chain) {
      rt_net_t *net = rt_get_net(n);
      if (net->last_active <= now)
         last = MIN(last, now - net->last_active);

      count -= n->width;
      RT_ASSERT(count >= 0);
   }

   return last;
}

DLLEXPORT
bool _driving(sig_shared_t *ss, uint32_t offset, int32_t count)
{
   rt_signal_t *s = container_of(ss, rt_signal_t, shared);

   TRACE("_driving %s offset=%d count=%d",
         istr(tree_ident(s->where)), offset, count);

   int ntotal = 0, ndriving = 0;
   bool found = false;
   rt_nexus_t *n = rt_split_nexus(s, offset, count);
   for (; count > 0; n = n->chain) {
      if (n->n_sources > 0) {
         for (rt_source_t *src = &(n->sources); src; src = src->chain_input) {
            if (src->tag != SOURCE_DRIVER)
               continue;
            else if (src->u.driver.proc == active_proc) {
               if (!src->u.driver.waveforms.null) ndriving++;
               found = true;
               break;
            }
         }
      }

      ntotal++;
      count -= n->width;
      RT_ASSERT(count >= 0);
   }

   if (!found)
      rt_msg(NULL, DIAG_FATAL, "process %s does not contain a driver for %s",
             istr(active_proc->name), istr(tree_ident(s->where)));

   return ntotal == ndriving;
}

DLLEXPORT
void *_driving_value(sig_shared_t *ss, uint32_t offset, int32_t count)
{
   rt_signal_t *s = container_of(ss, rt_signal_t, shared);

   TRACE("_driving_value %s offset=%d count=%d",
         istr(tree_ident(s->where)), offset, count);

   void *result = rt_tlab_alloc(s->shared.size);

   uint8_t *p = result;
   rt_nexus_t *n = rt_split_nexus(s, offset, count);
   for (; count > 0; n = n->chain) {
      rt_source_t *src = NULL;
      if (n->n_sources > 0) {
         for (src = &(n->sources); src; src = src->chain_input) {
            if (src->tag == SOURCE_DRIVER && src->u.driver.proc == active_proc)
               break;
         }
      }

      if (src == NULL)
         rt_msg(NULL, DIAG_FATAL, "process %s does not contain a driver for %s",
                istr(active_proc->name), istr(tree_ident(s->where)));

      memcpy(p, rt_value_ptr(n, &(src->u.driver.waveforms.value)),
             n->width * n->size);
      p += n->width * n->size;

      count -= n->width;
      RT_ASSERT(count >= 0);
   }

   return result;
}

DLLEXPORT
int32_t _test_net_active(sig_shared_t *ss, uint32_t offset, int32_t count)
{
   rt_signal_t *s = container_of(ss, rt_signal_t, shared);

   TRACE("_test_net_active %s offset=%d count=%d",
         istr(tree_ident(s->where)), offset, count);

   rt_nexus_t *n = rt_split_nexus(s, offset, count);
   for (; count > 0; n = n->chain) {
      rt_net_t *net = rt_get_net(n);
      if (net->last_active == now && net->active_delta == iteration)
         return 1;

      count -= n->width;
      RT_ASSERT(count >= 0);
   }

   return 0;
}

DLLEXPORT
int32_t _test_net_event(sig_shared_t *ss, uint32_t offset, int32_t count)
{
   rt_signal_t *s = container_of(ss, rt_signal_t, shared);

   TRACE("_test_net_event %s offset=%d count=%d",
         istr(tree_ident(s->where)), offset, count);

   rt_nexus_t *n = rt_split_nexus(s, offset, count);
   for (; count > 0; n = n->chain) {
      rt_net_t *net = rt_get_net(n);
      if (net->last_event == now && net->event_delta == iteration)
         return 1;

      count -= n->width;
      RT_ASSERT(count >= 0);
   }

   return 0;
}

DLLEXPORT
void _file_open(int8_t *status, void **_fp, uint8_t *name_bytes,
                int32_t name_len, int8_t mode, DEBUG_LOCUS(locus))
{
   tree_t where = rt_locus_to_tree(locus_unit, locus_offset);
   FILE **fp = (FILE **)_fp;

   char *fname LOCAL = xmalloc(name_len + 1);
   memcpy(fname, name_bytes, name_len);
   fname[name_len] = '\0';

   TRACE("_file_open %s fp=%p mode=%d", fname, fp, mode);

   const char *mode_str[] = {
      "rb", "wb", "w+b"
   };
   RT_ASSERT(mode < ARRAY_LEN(mode_str));

   if (status != NULL)
      *status = OPEN_OK;

   if (*fp != NULL) {
      if (status == NULL)
         rt_msg(tree_loc(where), DIAG_FATAL, "file object already associated "
                "with an external file");
      else
         *status = STATUS_ERROR;
   }
   else if (name_len == 0) {
      if (status == NULL)
         rt_msg(tree_loc(where), DIAG_FATAL, "empty file name in FILE_OPEN");
      else
         *status = NAME_ERROR;
   }
   else if (strcmp(fname, "STD_INPUT") == 0)
      *fp = stdin;
   else if (strcmp(fname, "STD_OUTPUT") == 0)
      *fp = stdout;
   else {
#ifdef __MINGW32__
      const bool failed = (fopen_s(fp, fname, mode_str[mode]) != 0);
#else
      const bool failed = ((*fp = fopen(fname, mode_str[mode])) == NULL);
#endif
      if (failed) {
         if (status == NULL)
            rt_msg(tree_loc(where), DIAG_FATAL, "failed to open %s: %s", fname,
                   strerror(errno));
         else {
            switch (errno) {
            case ENOENT: *status = NAME_ERROR; break;
            case EPERM:  *status = MODE_ERROR; break;
            default:     *status = NAME_ERROR; break;
            }
         }
      }
   }
}

DLLEXPORT
void _file_write(void **_fp, uint8_t *data, int32_t len)
{
   FILE **fp = (FILE **)_fp;

   if (*fp == NULL)
      rt_msg(NULL, DIAG_FATAL, "write to closed file");

   fwrite(data, 1, len, *fp);
}

DLLEXPORT
void _file_read(void **_fp, uint8_t *data, int32_t size, int32_t count,
                int32_t *out)
{
   FILE **fp = (FILE **)_fp;

   if (*fp == NULL)
      rt_msg(NULL, DIAG_FATAL, "read from closed file");

   size_t n = fread(data, size, count, *fp);
   if (out != NULL)
      *out = n;
}

DLLEXPORT
void _file_close(void **_fp)
{
   FILE **fp = (FILE **)_fp;

   TRACE("_file_close fp=%p", fp);

   if (*fp != NULL) {
      fclose(*fp);
      *fp = NULL;
   }
}

DLLEXPORT
int8_t _endfile(void *_f)
{
   FILE *f = _f;

   if (f == NULL)
      rt_msg(NULL, DIAG_FATAL, "ENDFILE called on closed file");

   int c = fgetc(f);
   if (c == EOF)
      return 1;
   else {
      ungetc(c, f);
      return 0;
   }
}

DLLEXPORT
void __nvc_flush(FILE *f)
{
   if (f == NULL)
      rt_msg(NULL, DIAG_FATAL, "FLUSH called on closed file");

   fflush(f);
}

////////////////////////////////////////////////////////////////////////////////
// Simulation kernel

__attribute__((format(printf, 1, 2)))
static void _tracef(const char *fmt, ...)
{
   va_list ap;
   va_start(ap, fmt);

   char buf[64];
   rt_fmt_now(buf, sizeof(buf));

   fprintf(stderr, "TRACE %s: ", buf);
   vfprintf(stderr, fmt, ap);
   fprintf(stderr, "\n");
   fflush(stderr);

   va_end(ap);
}

static void deltaq_insert(event_t *e)
{
   if (e->when == now) {
      event_t **chain = (e->kind == EVENT_DRIVER) ? &delta_driver : &delta_proc;
      e->delta_chain = *chain;
      *chain = e;
   }
   else {
      e->delta_chain = NULL;
      heap_insert(eventq_heap, e->when, e);
   }
}

static void deltaq_insert_proc(uint64_t delta, rt_proc_t *wake)
{
   event_t *e = rt_alloc(event_stack);
   e->when            = now + delta;
   e->kind            = EVENT_PROCESS;
   e->proc.wakeup_gen = wake->wakeable.wakeup_gen;
   e->proc.proc       = wake;

   deltaq_insert(e);
}

static void deltaq_insert_driver(uint64_t delta, rt_nexus_t *nexus,
                                 rt_source_t *source)
{
   event_t *e = rt_alloc(event_stack);
   e->when          = now + delta;
   e->kind          = EVENT_DRIVER;
   e->driver.nexus  = nexus;
   e->driver.source = source;

   deltaq_insert(e);
}

#if TRACE_DELTAQ > 0
static void deltaq_walk(uint64_t key, void *user, void *context)
{
   event_t *e = user;

   fprintf(stderr, "%s\t", fmt_time(e->when));
   switch (e->kind) {
   case EVENT_DRIVER:
      fprintf(stderr, "driver\t %s\n",
              istr(tree_ident(e->driver.nexus->signal->where)));
      break;
   case EVENT_PROCESS:
      fprintf(stderr, "process\t %s%s\n", istr(e->proc.proc->name),
              (e->proc.wakeup_gen == e->proc.proc->wakeable.wakeup_gen)
              ? "" : " (stale)");
      break;
   case EVENT_TIMEOUT:
      fprintf(stderr, "timeout\t %p %p\n", e->timeout.fn, e->timeout.user);
      break;
   case EVENT_EFFECTIVE:
      break;
   }
}

static void deltaq_dump(void)
{
   for (event_t *e = delta_driver; e != NULL; e = e->delta_chain)
      fprintf(stderr, "delta\tdriver\t %s\n",
              istr(tree_ident(e->driver.nexus->signal->where)));

   for (event_t *e = delta_proc; e != NULL; e = e->delta_chain)
      fprintf(stderr, "delta\tprocess\t %s%s\n",
              istr(tree_ident(e->proc.proc->where)),
              (e->proc.wakeup_gen == e->proc.proc->wakeable.wakeup_gen)
              ? "" : " (stale)");

   heap_walk(eventq_heap, deltaq_walk, NULL);
}
#endif

static res_memo_t *rt_memo_resolution_fn(rt_signal_t *signal,
                                         rt_resolution_t *resolution)
{
   // Optimise some common resolution functions by memoising them

   res_memo_t *memo = hash_get(res_memo_hash, resolution->closure.fn);
   if (memo != NULL)
      return memo;

   memo = xmalloc(sizeof(res_memo_t));
   memo->closure = resolution->closure;
   memo->flags   = resolution->flags;
   memo->ileft   = resolution->ileft;

   hash_put(res_memo_hash, memo->closure.fn, memo);

   if (resolution->nlits == 0 || resolution->nlits > 16)
      return memo;

   init_side_effect = SIDE_EFFECT_DISALLOW;

   // Memoise the function for all two value cases

   for (int i = 0; i < resolution->nlits; i++) {
      for (int j = 0; j < resolution->nlits; j++) {
         int8_t args[2] = { i, j };
         ffi_uarray_t u = { args, { { memo->ileft, 2 } } };
         ffi_call(&(memo->closure), &u, sizeof(u), &(memo->tab2[i][j]), 1);
         RT_ASSERT(memo->tab2[i][j] < resolution->nlits);
      }
   }

   init_side_effect = SIDE_EFFECT_ALLOW;

   // Memoise the function for all single value cases and determine if the
   // function behaves like the identity function

   bool identity = true;
   for (int i = 0; i < resolution->nlits; i++) {
      int8_t args[1] = { i };
      ffi_uarray_t u = { args, { { memo->ileft, 1 } } };
      ffi_call(&(memo->closure), &u, sizeof(u), &(memo->tab1[i]), 1);
      identity = identity && (memo->tab1[i] == i);
   }

   if (init_side_effect != SIDE_EFFECT_OCCURRED) {
      memo->flags |= R_MEMO;
      if (identity)
         memo->flags |= R_IDENT;
   }

   TRACE("memoised resolution function %p for type %s",
         resolution->closure.fn, type_pp(tree_type(signal->where)));

   return memo;
}

static void rt_global_event(rt_event_t kind)
{
   callback_t *it = global_cbs[kind];
   if (unlikely(it != NULL)) {
      while (it != NULL) {
         callback_t *tmp = it->next;
         (*it->fn)(it->user);
         rt_free(callback_stack, it);
         it = tmp;
      }

      global_cbs[kind] = NULL;
   }
}

static rt_value_t rt_alloc_value(rt_nexus_t *n)
{
   rt_value_t result = {};

   const size_t valuesz = n->size * n->width;
   if (valuesz > sizeof(rt_value_t)) {
      if (n->free_value != NULL) {
         result.ext = n->free_value;
         n->free_value = NULL;
      }
      else
         result.ext = xmalloc(valuesz);
   }

   return result;
}

static inline uint8_t *rt_value_ptr(rt_nexus_t *n, rt_value_t *v)
{
   const size_t valuesz = n->width * n->size;
   if (valuesz <= sizeof(rt_value_t))
      return v->bytes;
   else
      return v->ext;
}

static void rt_copy_value_ptr(rt_nexus_t *n, rt_value_t *v, const void *p)
{
   const size_t valuesz = n->width * n->size;
   if (valuesz <= sizeof(rt_value_t))
      v->qword = *(uint64_t *)p;
   else
      memcpy(v->ext, p, valuesz);
}

static inline bool rt_cmp_values(rt_nexus_t *n, rt_value_t a, rt_value_t b)
{
   const size_t valuesz = n->width * n->size;
   if (valuesz <= sizeof(rt_value_t))
      return a.qword == b.qword;
   else
      return memcmp(a.ext, b.ext, valuesz) == 0;
}

static void rt_free_value(rt_nexus_t *n, rt_value_t v)
{
   const size_t valuesz = n->width * n->size;
   if (valuesz > sizeof(rt_value_t)) {
      if (n->free_value == NULL)
         n->free_value = v.ext;
      else
         free(v.ext);
   }
}

static void *rt_tlab_alloc(size_t size)
{
   if (tlab_valid(__nvc_tlab))
      return tlab_alloc(&__nvc_tlab, size);
   else
      return mspace_alloc(mspace, size);
}

static void rt_sched_event(sens_list_t **list, rt_wakeable_t *obj, bool recur)
{
   // See if there is already a stale entry in the pending list for this
   // object
   sens_list_t *it = *list;
   int count = 0;
   for (; it != NULL; it = it->next, ++count) {
      if ((it->wake == obj) && (it->wakeup_gen != obj->wakeup_gen))
         break;
   }

   if (it == NULL) {
      sens_list_t *node = rt_alloc(sens_list_stack);
      node->wake       = obj;
      node->wakeup_gen = obj->wakeup_gen;
      node->next       = *list;
      node->reenq      = (recur ? list : NULL);

      *list = node;
   }
   else {
      // Reuse the stale entry
      it->wakeup_gen = obj->wakeup_gen;
   }
}

#if TRACE_PENDING
static void rt_dump_pending(void)
{
   for (struct sens_list *it = pending; it != NULL; it = it->next) {
      printf("%d..%d\t%s%s\n", it->first, it->last,
             istr(tree_ident(it->proc->source)),
             (it->wakeup_gen == it->proc->wakeup_gen) ? "" : " (stale)");
   }
}
#endif  // TRACE_PENDING

static void rt_free_delta_events(event_t *e)
{
   while (e != NULL) {
      event_t *tmp = e->delta_chain;
      rt_free(event_stack, e);
      e = tmp;
   }
}

static void rt_scope_deps_cb(ident_t unit_name, void *__ctx)
{
   // TODO: this should be redundant now we have the package init op

   rt_scope_t ***tailp = __ctx;

   tree_t unit = lib_get_qualified(unit_name);
   if (unit == NULL) {
      warnf("missing dependency %s", istr(unit_name));
      return;
   }

   if (hash_get(scopes, unit) != NULL)
      return;

   const tree_kind_t kind = tree_kind(unit);
   if (kind != T_PACKAGE && kind != T_PACK_INST) {
      tree_walk_deps(unit, rt_scope_deps_cb, tailp);
      return;
   }

   rt_scope_t *s = xcalloc(sizeof(rt_scope_t));
   s->where    = unit;
   s->name     = tree_ident(unit);
   s->kind     = SCOPE_PACKAGE;
   s->privdata = mptr_new(mspace, "package privdata");

   hash_put(scopes, unit, s);

   tree_walk_deps(unit, rt_scope_deps_cb, tailp);

   if (kind == T_PACKAGE) {
      tree_t body = body_of(unit);
      if (body != NULL)
         tree_walk_deps(body, rt_scope_deps_cb, tailp);
   }

   **tailp = s;
   *tailp = &(s->chain);
}

static rt_scope_t *rt_scope_for_block(tree_t block, ident_t prefix)
{
   rt_scope_t *s = xcalloc(sizeof(rt_scope_t));
   s->where    = block;
   s->name     = ident_prefix(prefix, tree_ident(block), '.');
   s->kind     = SCOPE_INSTANCE;
   s->privdata = mptr_new(mspace, "block privdata");

   hash_put(scopes, block, s);

   rt_scope_t **childp = &(s->child);
   rt_proc_t **procp = &(s->procs);

   tree_t hier = tree_decl(block, 0);
   assert(tree_kind(hier) == T_HIER);

   ident_t path = tree_ident(hier);

   const int ndecls = tree_decls(block);
   for (int i = 0; i < ndecls; i++) {
      tree_t d = tree_decl(block, i);
      if (tree_kind(d) == T_PACK_INST) {
         rt_scope_t *p = xcalloc(sizeof(rt_scope_t));
         p->where    = d;
         p->name     = ident_prefix(s->name, tree_ident(d), '.');
         p->kind     = SCOPE_PACKAGE;
         p->privdata = mptr_new(mspace, "pack inst privdata");

         hash_put(scopes, d, p);

         *childp = p;
         childp = &(p->chain);
      }
   }

   const int nstmts = tree_stmts(block);
   for (int i = 0; i < nstmts; i++) {
      tree_t t = tree_stmt(block, i);
      switch (tree_kind(t)) {
      case T_BLOCK:
         {
            rt_scope_t *c = rt_scope_for_block(t, s->name);
            c->parent = s;;

            *childp = c;
            childp = &(c->chain);
         }
         break;

      case T_PROCESS:
         {
            ident_t name = tree_ident(t);
            ident_t sym = ident_prefix(s->name, name, '.');

            rt_proc_t *p = xcalloc(sizeof(rt_proc_t));
            p->where     = t;
            p->name      = ident_prefix(path, ident_downcase(name), ':');
            p->proc_fn   = jit_find_symbol(istr(sym), true);
            p->scope     = s;
            p->privdata  = mptr_new(mspace, "process privdata");

            p->wakeable.kind       = W_PROC;
            p->wakeable.wakeup_gen = 0;
            p->wakeable.pending    = false;
            p->wakeable.postponed  = !!(tree_flags(t) & TREE_F_POSTPONED);

            *procp = p;
            procp = &(p->chain);
         }
         break;

      default:
         break;
      }
   }

   return s;
}

static void rt_setup(tree_t top)
{
   now = 0;
   iteration = -1;
   active_proc = NULL;
   active_scope = NULL;
   force_stop = false;
   can_create_delta = true;
   nexus_tail = &nexuses;

   RT_ASSERT(resume == NULL);

   rt_free_delta_events(delta_proc);
   rt_free_delta_events(delta_driver);

   eventq_heap = heap_new(512);

   scopes = hash_new(256);

   root = xcalloc(sizeof(rt_scope_t));
   root->kind     = SCOPE_ROOT;
   root->where    = top;
   root->privdata = mptr_new(mspace, "root privdata");

   rt_scope_t **tailp = &(root->child);
   tree_walk_deps(top, rt_scope_deps_cb, &tailp);

   *tailp = rt_scope_for_block(tree_stmt(top, 0), lib_name(lib_work()));

   res_memo_hash = hash_new(128);
}

static void rt_reset(rt_proc_t *proc)
{
   TRACE("reset process %s", istr(proc->name));

   assert(!tlab_valid(proc->tlab));
   assert(!tlab_valid(__nvc_tlab));   // Not used during reset

   active_proc = proc;
   active_scope = proc->scope;

   void *p = (*proc->proc_fn)(NULL, mptr_get(mspace, proc->scope->privdata));
   mptr_put(mspace, proc->privdata, p);
}

static void rt_run(rt_proc_t *proc)
{
   TRACE("run %sprocess %s", proc->privdata ? "" :  "stateless ",
         istr(proc->name));

   assert(!tlab_valid(spare_tlab));

   if (tlab_valid(proc->tlab)) {
      TRACE("using private TLAB at %p (%zu used)", proc->tlab.base,
            proc->tlab.alloc - proc->tlab.base);
      tlab_move(__nvc_tlab, spare_tlab);
      tlab_move(proc->tlab, __nvc_tlab);
   }
   else if (!tlab_valid(__nvc_tlab))
      tlab_acquire(mspace, &__nvc_tlab);

   active_proc = proc;
   active_scope = proc->scope;

   // Stateless processes have NULL privdata so pass a dummy pointer
   // value in so it can be distinguished from a reset
   void *state = mptr_get(mspace, proc->privdata) ?: (void *)-1;

   void *context = mptr_get(mspace, proc->scope->privdata);
   (*proc->proc_fn)(state, context);

   active_proc = NULL;

   if (tlab_valid(__nvc_tlab)) {
      // The TLAB is still valid which means the process finished
      // instead of suspending at a wait statement and none of the data
      // inside it can be live anymore
      assert(!tlab_valid(proc->tlab));
      tlab_reset(__nvc_tlab);

      if (tlab_valid(spare_tlab))   // Surplus TLAB
         tlab_release(&spare_tlab);
   }
   else {
      // Process must have claimed TLAB or otherwise it would be lost
      assert(tlab_valid(proc->tlab));
      if (tlab_valid(spare_tlab))
         tlab_move(spare_tlab, __nvc_tlab);
   }
}

static void *rt_call_module_reset(ident_t name, void *arg)
{
   char *buf LOCAL = xasprintf("%s_reset", istr(name));

   assert(!tlab_valid(__nvc_tlab));   // Not used during reset

   void *result = NULL;
   void *(*reset_fn)(void *) = jit_find_symbol(buf, false);
   if (reset_fn != NULL)
      result = (*reset_fn)(arg);

   return result;
}

static void *rt_call_conversion(rt_port_t *port, value_fn_t fn)
{
   rt_signal_t *i0 = port->input->signal;
   rt_conv_func_t *cf = port->conv_func;

   bool incopy = false;
   void *indata;
   size_t insz;
   if (i0->parent->kind == SCOPE_SIGNAL) {
      indata = rt_composite_signal(i0, &insz, fn);
      incopy = true;
   }
   else if (i0->n_nexus == 1) {
      insz   = i0->shared.size;
      indata = (void *)(*fn)(&(i0->nexus));
   }
   else {
      insz   = i0->shared.size;
      indata = xmalloc(insz);
      incopy = true;

      rt_nexus_t *n = &(i0->nexus);
      for (unsigned i = 0; i < i0->n_nexus; i++, n = n->chain) {
         ptrdiff_t o = (uint8_t *)n->resolved - i0->shared.data;
         memcpy(indata + i0->shared.offset + o, (*fn)(n), n->size * n->width);
      }
   }

   TRACE("call conversion function %p insz=%zu outsz=%zu",
         cf->closure.fn, insz, cf->bufsz);

   ffi_call(&(cf->closure), indata, insz, cf->buffer, cf->bufsz);

   if (incopy) free(indata);

   return cf->buffer + port->output->signal->shared.offset;
}

static void *rt_source_value(rt_nexus_t *nexus, rt_source_t *src)
{
   switch (src->tag) {
   case SOURCE_DRIVER:
      if (unlikely(src->u.driver.waveforms.null))
         return NULL;
      else
         return rt_value_ptr(nexus, &(src->u.driver.waveforms.value));

   case SOURCE_PORT:
      if (likely(src->u.port.conv_func == NULL))
         return rt_driving_value(src->u.port.input);
      else
         return rt_call_conversion(&(src->u.port), rt_driving_value);
   }

   return NULL;
}

static void *rt_call_resolution(rt_nexus_t *nexus, res_memo_t *r, int nonnull)
{
   // Find the first non-null source
   char *p0;
   rt_source_t *s0 = &(nexus->sources);
   for (; s0 && (p0 = rt_source_value(nexus, s0)) == NULL; s0 = s0->chain_input)
      ;

   if ((nexus->flags & NET_F_R_IDENT) && nonnull == 1) {
      // Resolution function behaves like identity for a single driver
      return p0;
   }
   else if ((r->flags & R_MEMO) && nonnull == 1) {
      // Resolution function has been memoised so do a table lookup

      void *resolved = rt_tlab_alloc(nexus->width * nexus->size);

      for (int j = 0; j < nexus->width; j++) {
         const int index = ((uint8_t *)p0)[j];
         ((int8_t *)resolved)[j] = r->tab1[index];
      }

      return resolved;
   }
   else if ((r->flags & R_MEMO) && nonnull == 2) {
      // Resolution function has been memoised so do a table lookup

      void *resolved = rt_tlab_alloc(nexus->width * nexus->size);

      char *p1 = NULL;
      for (rt_source_t *s1 = s0->chain_input;
           s1 && (p1 = rt_source_value(nexus, s1)) == NULL;
           s1 = s1->chain_input)
         ;

      for (int j = 0; j < nexus->width; j++)
         ((int8_t *)resolved)[j] = r->tab2[(int)p0[j]][(int)p1[j]];

      return resolved;
   }
   else if (r->flags & R_COMPOSITE) {
      // Call resolution function of composite type

      rt_scope_t *scope = nexus->signal->parent, *rscope = scope;
      while (scope->parent->kind == SCOPE_SIGNAL) {
         scope = scope->parent;
         if (scope->flags & SCOPE_F_RESOLVED)
            rscope = scope;
      }

      TRACE("resolved composite signal needs %d bytes", scope->size);

      uint8_t *inputs = rt_tlab_alloc(nonnull * scope->size);
      rt_copy_sub_signal_sources(scope, inputs, scope->size);

      void *resolved;
      ffi_uarray_t u = { inputs, { { r->ileft, nonnull } } };
      ffi_call(&(r->closure), &u, sizeof(u), &resolved, sizeof(resolved));

      const ptrdiff_t noff =
         nexus->resolved - (void *)nexus->signal->shared.data;
      return resolved + nexus->signal->shared.offset + noff - rscope->offset;
   }
   else {
      void *resolved = rt_tlab_alloc(nexus->width * nexus->size);

      for (int j = 0; j < nexus->width; j++) {
#define CALL_RESOLUTION_FN(type) do {                                   \
            type vals[nonnull];                                         \
            unsigned o = 0;                                             \
            for (rt_source_t *s = s0; s; s = s->chain_input) {          \
               const void *data = rt_source_value(nexus, s);            \
               if (data != NULL)                                        \
                  vals[o++] = ((const type *)data)[j];                  \
            }                                                           \
            assert(o == nonnull);                                       \
            type *p = (type *)resolved;                                 \
            ffi_uarray_t u = { vals, { { r->ileft, nonnull } } };       \
            ffi_call(&(r->closure), &u, sizeof(u),                      \
                     &(p[j]), sizeof(p[j]));                            \
         } while (0)

         FOR_ALL_SIZES(nexus->size, CALL_RESOLUTION_FN);
      }

      return resolved;
   }
}

static void *rt_driving_value(rt_nexus_t *nexus)
{
   // Algorithm for driving values is in LRM 08 section 14.7.7.2

   // If S is driving-value forced, the driving value of S is unchanged
   // from its previous value; no further steps are required.
   if (unlikely(nexus->flags & NET_F_FORCED))
      return rt_value_ptr(nexus, &(nexus->forcing));

   // If S has no source, then the driving value of S is given by the
   // default value associated with S
   if (nexus->n_sources == 0)
      return nexus->resolved + 2*nexus->signal->shared.size;

   res_memo_t *r = nexus->signal->resolution;

   if (r == NULL) {
      rt_source_t *s = &(nexus->sources);

      if (s->tag == SOURCE_DRIVER) {
         // If S has one source that is a driver and S is not a resolved
         // signal, then the driving value of S is the current value of
         // that driver.
         assert(!(s->u.driver.waveforms.null));
         return rt_value_ptr(nexus, &(s->u.driver.waveforms.value));
      }
      else {
         // If S has one source that is a port and S is not a resolved
         // signal, then the driving value of S is the driving value of
         // the formal part of the association element that associates S
         // with that port
         if (likely(s->u.port.conv_func == NULL))
            return rt_driving_value(s->u.port.input);
         else
            return rt_call_conversion(&(s->u.port), rt_driving_value);
      }
   }
   else {
      // If S is a resolved signal and has one or more sources, then the
      // driving values of the sources of S are examined.

      int nonnull = 0;
      for (rt_source_t *s = &(nexus->sources); s; s = s->chain_input) {
         if (s->tag != SOURCE_DRIVER || !s->u.driver.waveforms.null)
            nonnull++;
      }

      // If S is of signal kind register and all the sources of S have
      // values determined by the null transaction, then the driving
      // value of S is unchanged from its previous value.
      if (nonnull == 0 && (nexus->flags & NET_F_REGISTER))
         return nexus->resolved;

      // Otherwise, the driving value of S is obtained by executing the
      // resolution function associated with S
      return rt_call_resolution(nexus, r, nonnull);
   }
}

static const void *rt_effective_value(rt_nexus_t *nexus)
{
   // Algorithm for effective values is in LRM 08 section 14.7.7.3

   // If S is a connected port of mode in or inout, then the effective
   // value of S is the same as the effective value of the actual part
   // of the association element that associates an actual with S
   if (nexus->flags & NET_F_INOUT) {
      for (rt_source_t *s = nexus->outputs; s; s = s->chain_output) {
         if (s->tag == SOURCE_PORT) {
            if (likely(s->u.port.conv_func == NULL))
               return rt_effective_value(s->u.port.output);
            else
               return nexus->resolved;
         }
      }
   }

   // If S is a signal declared by a signal declaration, a port of mode
   // out or buffer, or an unconnected port of mode inout, then the
   // effective value of S is the same as the driving value of S.
   //
   // If S is an unconnected port of mode in, the effective value of S
   // is given by the default value associated with S.
   if (nexus->flags & NET_F_EFFECTIVE)
      return nexus->resolved + 2 * nexus->signal->shared.size;
   else
      return nexus->resolved;
}

static void rt_propagate_nexus(rt_nexus_t *nexus, const void *resolved)
{
   const size_t valuesz = nexus->size * nexus->width;

   // LAST_VALUE is the same as the initial value when there have
   // been no events on the signal otherwise only update it when
   // there is an event
   void *last_value = nexus->resolved + nexus->signal->shared.size;
   memcpy(last_value, nexus->resolved, valuesz);

   if (nexus->resolved != resolved)   // Can occur during startup
      memcpy(nexus->resolved, resolved, valuesz);
}

static void rt_reset_scope(rt_scope_t *s)
{
   if (s->kind == SCOPE_INSTANCE || s->kind == SCOPE_PACKAGE) {
      TRACE("reset scope %s", istr(s->name));

      void *context = s->parent ? mptr_get(mspace, s->parent->privdata) : NULL;
      active_scope = s;
      signals_tail = &(s->signals);

      void *p = rt_call_module_reset(s->name, context);
      mptr_put(mspace, s->privdata, p);

      active_scope = NULL;
      signals_tail = NULL;
   }

   for (rt_scope_t *c = s->child; c != NULL; c = c->chain)
      rt_reset_scope(c);

   for (rt_proc_t *p = s->procs; p != NULL; p = p->chain)
      rt_reset(p);
}

static int rt_nexus_rank(rt_nexus_t *nexus)
{
   if (nexus->n_sources > 0) {
      int rank = 0;
      for (rt_source_t *s = &(nexus->sources); s; s = s->chain_input) {
         if (s->tag == SOURCE_PORT)
            rank = MAX(rank, rt_nexus_rank(s->u.port.input) + 1);
      }
      return rank;
   }
   else
      return 0;
}

static void rt_initial(tree_t top)
{
   // Initialisation is described in LRM 93 section 12.6.4

   rt_reset_scope(root);

#if TRACE_SIGNALS > 0
   if (trace_on)
      rt_dump_signals(root);
#endif

   TRACE("calculate initial signal values");

   // The signals in the model are updated as follows in an order such
   // that if a given signal R depends upon the current value of another
   // signal S, then the current value of S is updated prior to the
   // updating of the current value of R.

   heap_t *q = heap_new(MAX(profile.n_signals + 1, 128));

   for (rt_nexus_t *n = nexuses; n != NULL; n = n->chain) {
      // The initial value of each driver is the default value of the signal
      if (n->n_sources > 0) {
         for (rt_source_t *s = &(n->sources); s; s = s->chain_input) {
            if (s->tag == SOURCE_DRIVER)
               rt_copy_value_ptr(n, &(s->u.driver.waveforms.value),
                                 n->resolved);
         }
      }

      heap_insert(q, rt_nexus_rank(n), n);
   }

   SCOPED_A(rt_nexus_t *) effq = AINIT;

   while (heap_size(q) > 0) {
      rt_nexus_t *n = heap_extract_min(q);

      if (n->flags & NET_F_EFFECTIVE) {
         // Driving and effective values must be calculated separately
         void *driving = n->resolved + 2*n->signal->shared.size;
         memcpy(driving, rt_driving_value(n), n->width * n->size);

         APUSH(effq, n);

         TRACE("%s initial driving value %s",
               istr(tree_ident(n->signal->where)), fmt_nexus(n, driving));
      }
      else {
         // Effective value is always the same as the driving value
         const void *initial = n->resolved;
         if (n->n_sources > 0)
            initial = rt_driving_value(n);

         rt_propagate_nexus(n, initial);

         TRACE("%s initial value %s", istr(tree_ident(n->signal->where)),
               fmt_nexus(n, initial));
      }
   }

   heap_free(q);

   // Update effective values after all initial driving values calculated
   for (int i = 0; i < effq.count; i++) {
      rt_nexus_t *n = effq.items[i];

      const void *initial = rt_effective_value(n);
      rt_propagate_nexus(n, initial);

      TRACE("%s initial effective value %s", istr(tree_ident(n->signal->where)),
            fmt_nexus(n, initial));
   }
}

static void rt_trace_wakeup(rt_wakeable_t *obj)
{
   if (unlikely(trace_on)) {
      switch (obj->kind) {
      case W_PROC:
         TRACE("wakeup %sprocess %s", obj->postponed ? "postponed " : "",
               istr(container_of(obj, rt_proc_t, wakeable)->name));
         break;

      case W_WATCH:
         TRACE("wakeup %svalue change callback %p",
               obj->postponed ? "postponed " : "",
               container_of(obj, rt_watch_t, wakeable)->fn);
         break;

      case W_IMPLICIT:
         TRACE("wakeup implicit signal %s",
               istr(tree_ident(container_of(obj, rt_implicit_t, wakeable)
                               ->signal.where)));
      }
   }
}

static void rt_wakeup(sens_list_t *sl)
{
   // To avoid having each process keep a list of the signals it is
   // sensitive to, each process has a "wakeup generation" number which
   // is incremented after each wait statement and stored in the signal
   // sensitivity list. We then ignore any sensitivity list elements
   // where the generation doesn't match the current process wakeup
   // generation: these correspond to stale "wait on" statements that
   // have already resumed.

   if (sl->wakeup_gen == sl->wake->wakeup_gen || sl->reenq != NULL) {
      rt_trace_wakeup(sl->wake);

      sens_list_t **enq = NULL;
      if (sl->wake->postponed) {
         switch (sl->wake->kind) {
         case W_PROC: enq = &postponed; break;
         case W_WATCH: enq = &postponed_watch; break;
         case W_IMPLICIT: assert(false); break;
         }
      }
      else {
         switch (sl->wake->kind) {
         case W_PROC: enq = &resume; break;
         case W_WATCH: enq = &resume_watch; break;
         case W_IMPLICIT: enq = &implicit; break;
         }
      }

      sl->next = *enq;
      *enq = sl;

      ++(sl->wake->wakeup_gen);
      sl->wake->pending = true;
   }
   else
      rt_free(sens_list_stack, sl);
}

static void rt_sched_driver(rt_nexus_t *nexus, uint64_t after,
                            uint64_t reject, rt_value_t value, bool null)
{
   if (unlikely(reject > after))
      fatal("signal %s pulse reject limit %s is greater than "
            "delay %s", istr(tree_ident(nexus->signal->where)),
            fmt_time(reject), fmt_time(after));

   // Try to find this process in the list of existing drivers
   rt_source_t *d;
   for (d = &(nexus->sources); d; d = d->chain_input) {
      if (d->tag == SOURCE_DRIVER && d->u.driver.proc == active_proc)
         break;
   }
   RT_ASSERT(d != NULL);

   waveform_t *w = rt_alloc(waveform_stack);
   w->when  = now + after;
   w->next  = NULL;
   w->value = value;
   w->null  = null;

   waveform_t *last = &(d->u.driver.waveforms);
   waveform_t *it   = last->next;
   while (it != NULL && it->when < w->when) {
      // If the current transaction is within the pulse rejection interval
      // and the value is different to that of the new transaction then
      // delete the current transaction
      RT_ASSERT(it->when >= now);
      if ((it->when >= w->when - reject)
          && !rt_cmp_values(nexus, it->value, w->value)) {
         waveform_t *next = it->next;
         last->next = next;
         rt_free_value(nexus, it->value);
         rt_free(waveform_stack, it);
         it = next;
      }
      else {
         last = it;
         it = it->next;
      }
   }
   w->next = NULL;
   last->next = w;

   // Delete all transactions later than this
   // We could remove this transaction from the deltaq as well but the
   // overhead of doing so is probably higher than the cost of waking
   // up for the empty event
   bool already_scheduled = false;
   while (it != NULL) {
      rt_free_value(nexus, it->value);

      if (it->when == w->when)
         already_scheduled = true;

      waveform_t *next = it->next;
      rt_free(waveform_stack, it);
      it = next;
   }

   if (!already_scheduled)
      deltaq_insert_driver(after, nexus, d);
}

static void rt_notify_event(rt_net_t *net)
{
   net->last_event = net->last_active = now;
   net->event_delta = net->active_delta = iteration;

   // Wake up everything on the pending list
   for (sens_list_t *it = net->pending, *next; it; it = next) {
      next = it->next;
      rt_wakeup(it);
   }
   net->pending = NULL;
}

static void rt_notify_active(rt_net_t *net)
{
   net->last_active = now;
   net->active_delta = iteration;
}

static void rt_update_effective(rt_nexus_t *nexus)
{
   const void *value = rt_effective_value(nexus);

   TRACE("update %s effective value %s", istr(tree_ident(nexus->signal->where)),
         fmt_nexus(nexus, value));

   rt_net_t *net = rt_get_net(nexus);

   if (memcmp(nexus->resolved, value, nexus->size * nexus->width) != 0) {
      rt_propagate_nexus(nexus, value);
      rt_notify_event(net);
   }
   else
      rt_notify_active(net);
}

static void rt_enqueue_effective(rt_nexus_t *nexus)
{
   event_t *e = rt_alloc(event_stack);
   e->when      = now;
   e->kind      = EVENT_EFFECTIVE;
   e->effective = nexus;

   rt_push_run_queue(&effq, e);

   if (nexus->n_sources > 0) {
      for (rt_source_t *s = &(nexus->sources); s; s = s->chain_input) {
         if (s->tag == SOURCE_PORT && (s->u.port.input->flags & NET_F_INOUT))
            rt_enqueue_effective(s->u.port.input);
      }
   }
}

static void rt_update_driving(rt_nexus_t *nexus)
{
   const void *value = rt_driving_value(nexus);
   const size_t valuesz = nexus->size * nexus->width;

   TRACE("update %s driving value %s", istr(tree_ident(nexus->signal->where)),
         fmt_nexus(nexus, value));

   rt_net_t *net = rt_get_net(nexus);
   bool update_outputs = false;

   if (nexus->flags & NET_F_EFFECTIVE) {
      // The active and event flags will be set when we update the
      // effective value later
      update_outputs = true;

      void *driving = nexus->resolved + 2*nexus->signal->shared.size;
      memcpy(driving, value, valuesz);

      rt_enqueue_effective(nexus);
   }
   else if (memcmp(nexus->resolved, value, valuesz) != 0) {
      rt_propagate_nexus(nexus, value);
      rt_notify_event(net);
      update_outputs = true;
   }
   else
      rt_notify_active(net);

   if (update_outputs) {
      for (rt_source_t *o = nexus->outputs; o; o = o->chain_output) {
         assert(o->tag == SOURCE_PORT);
         rt_update_driving(o->u.port.output);
      }
   }
}

static void rt_update_driver(rt_nexus_t *nexus, rt_source_t *source)
{
   // Updating drivers may involve calling resolution functions
   if (!tlab_valid(__nvc_tlab))
      tlab_acquire(mspace, &__nvc_tlab);

   if (likely(source != NULL)) {
      waveform_t *w_now  = &(source->u.driver.waveforms);
      waveform_t *w_next = w_now->next;

      if (likely((w_next != NULL) && (w_next->when == now))) {
         rt_free_value(nexus, w_now->value);
         *w_now = *w_next;
         rt_free(waveform_stack, w_next);
         rt_update_driving(nexus);
      }
      else
         RT_ASSERT(w_now != NULL);
   }
   else  // Update due to force/release
      rt_update_driving(nexus);

   tlab_reset(__nvc_tlab);   // No allocations can be live past here
}

static void rt_update_implicit_signal(rt_implicit_t *imp)
{
   int8_t r;
   ffi_call(imp->closure, NULL, 0, &r, sizeof(r));

   TRACE("implicit signal %s guard expression %d",
         istr(tree_ident(imp->signal.where)), r);

   RT_ASSERT(imp->signal.n_nexus == 1);
   rt_nexus_t *n0 = &(imp->signal.nexus);

   rt_net_t *net = rt_get_net(n0);

   if (*(int8_t *)n0->resolved != r) {
      rt_propagate_nexus(n0, &r);
      rt_notify_event(net);
   }
   else
      rt_notify_active(net);
}

static bool rt_stale_event(event_t *e)
{
   return (e->kind == EVENT_PROCESS)
      && (e->proc.wakeup_gen != e->proc.proc->wakeable.wakeup_gen);
}

static void rt_push_run_queue(rt_run_queue_t *q, event_t *e)
{
   if (unlikely(q->wr == q->alloc)) {
      if (q->alloc == 0) {
         q->alloc = 128;
         q->queue = xmalloc_array(q->alloc, sizeof(event_t *));
      }
      else {
         q->alloc *= 2;
         q->queue = xrealloc_array(q->queue, q->alloc, sizeof(event_t *));
      }
   }

   if (unlikely(rt_stale_event(e)))
      rt_free(event_stack, e);
   else {
      q->queue[(q->wr)++] = e;
      if (e->kind == EVENT_PROCESS)
         ++(e->proc.proc->wakeable.wakeup_gen);
   }
}

static event_t *rt_pop_run_queue(rt_run_queue_t *q)
{
   if (q->wr == q->rd) {
      q->wr = 0;
      q->rd = 0;
      return NULL;
   }
   else
      return q->queue[(q->rd)++];
}

static void rt_iteration_limit(void)
{
   diag_t *d = diag_new(DIAG_FATAL, NULL);

   diag_printf(d, "iteration limit of %d delta cycles reached",
               opt_get_int(OPT_STOP_DELTA));

   for (sens_list_t *it = resume; it != NULL; it = it->next) {
      if (it->wake->kind == W_PROC) {
         rt_proc_t *proc = container_of(it->wake, rt_proc_t, wakeable);
         const loc_t *l = tree_loc(proc->where);
         diag_hint(d, l, "process %s is active", istr(proc->name));
      }
   }

   diag_hint(d, NULL, "You can increase this limit with $bold$--stop-delta$$");

   diag_emit(d);
   fatal_exit(EXIT_FAILURE);
}

static void rt_resume(sens_list_t **list)
{
   for (sens_list_t *it = *list, *tmp; it; it = tmp) {
      rt_wakeable_t *wake = it->wake;
      tmp = *list = it->next;

      // Free the list element now as rt_run may longjmp out of this
      // function
      if (it->reenq == NULL)
         rt_free(sens_list_stack, it);
      else {
         it->next = *(it->reenq);
         *(it->reenq) = it;
      }
      it = NULL;

      if (!wake->pending)
         continue;   // Stale wakeup

      wake->pending = false;

      switch (wake->kind) {
      case W_PROC:
         {
            rt_proc_t *proc = container_of(wake, rt_proc_t, wakeable);
            rt_run(proc);
         }
         break;
      case W_WATCH:
         {
            rt_watch_t *w = container_of(wake, rt_watch_t, wakeable);
            (*w->fn)(now, w->signal, w, w->user_data);
         }
         break;
      case W_IMPLICIT:
         {
            rt_implicit_t *imp = container_of(wake, rt_implicit_t, wakeable);
            rt_update_implicit_signal(imp);
         }
      }
   }
}

static inline bool rt_next_cycle_is_delta(void)
{
   return (delta_driver != NULL) || (delta_proc != NULL);
}

static void rt_cycle(int stop_delta)
{
   // Simulation cycle is described in LRM 93 section 12.6.4

   const bool is_delta_cycle = (delta_driver != NULL) || (delta_proc != NULL);

   if (is_delta_cycle)
      iteration = iteration + 1;
   else {
      event_t *peek = heap_min(eventq_heap);
      while (unlikely(rt_stale_event(peek))) {
         // Discard stale events
         rt_free(event_stack, heap_extract_min(eventq_heap));
         if (heap_size(eventq_heap) == 0)
            return;
         else
            peek = heap_min(eventq_heap);
      }
      now = peek->when;
      iteration = 0;
   }

   TRACE("begin cycle");

#if TRACE_DELTAQ > 0
   if (trace_on)
      deltaq_dump();
#endif
#if TRACE_PENDING > 0
   if (trace_on)
      rt_dump_pending();
#endif

   if (is_delta_cycle) {
      for (event_t *e = delta_driver; e != NULL; e = e->delta_chain)
         rt_push_run_queue(&driverq, e);

      for (event_t *e = delta_proc; e != NULL; e = e->delta_chain)
         rt_push_run_queue(&procq, e);

      delta_driver = NULL;
      delta_proc = NULL;
   }
   else {
      rt_global_event(RT_NEXT_TIME_STEP);

      for (;;) {
         event_t *e = heap_extract_min(eventq_heap);
         switch (e->kind) {
         case EVENT_PROCESS: rt_push_run_queue(&procq, e); break;
         case EVENT_DRIVER:  rt_push_run_queue(&driverq, e); break;
         case EVENT_TIMEOUT: rt_push_run_queue(&timeoutq, e); break;
         case EVENT_EFFECTIVE: break;
         }

         if (heap_size(eventq_heap) == 0)
            break;

         event_t *peek = heap_min(eventq_heap);
         if (peek->when > now)
            break;
      }
   }

   if (profiling) {
      const uint32_t nevents = procq.wr + driverq.wr + timeoutq.wr;

      profile.deltas++;
      profile.runq_min = MIN(profile.runq_min, nevents);
      profile.runq_max = MAX(profile.runq_max, nevents);
      profile.runq_mean += (nevents - profile.runq_mean) / profile.deltas;
   }

   event_t *event;

   while ((event = rt_pop_run_queue(&timeoutq))) {
      (*event->timeout.fn)(now, event->timeout.user);
      rt_free(event_stack, event);
   }

   while ((event = rt_pop_run_queue(&driverq))) {
      rt_update_driver(event->driver.nexus, event->driver.source);
      rt_free(event_stack, event);
   }

   while ((event = rt_pop_run_queue(&effq))) {
      rt_update_effective(event->effective);
      rt_free(event_stack, event);
   }

   rt_resume(&implicit);

#if TRACE_SIGNALS > 0
   if (trace_on)
      rt_dump_signals(root);
#endif

   // Run all non-postponed event callbacks
   rt_resume(&resume_watch);

   while ((event = rt_pop_run_queue(&procq))) {
      // Free the event before running the process to avoid a leak if we
      // longjmp back to rt_run_sim
      rt_proc_t *p = event->proc.proc;
      rt_free(event_stack, event);
      rt_run(p);
   }

   // Run all processes that resumed because of signal events
   rt_resume(&resume);
   rt_global_event(RT_END_OF_PROCESSES);

   if (!rt_next_cycle_is_delta()) {
      can_create_delta = false;
      rt_global_event(RT_LAST_KNOWN_DELTA_CYCLE);

      // Run any postponed processes
      rt_resume(&postponed);

      // Execute all postponed event callbacks
      rt_resume(&postponed_watch);

      can_create_delta = true;
   }
   else if (unlikely((stop_delta > 0) && (iteration == stop_delta)))
      rt_iteration_limit();
}

static void rt_free_sens_list(sens_list_t **l)
{
   for (sens_list_t *it = *l, *tmp; it; it = tmp) {
      tmp = it->next;
      rt_free(sens_list_stack, it);
   }
   *l = NULL;
}

static void rt_cleanup_nexus(rt_nexus_t *n)
{
   rt_free_value(n, n->forcing);

   bool must_free = false;
   for (rt_source_t *s = &(n->sources), *tmp; s; s = tmp, must_free = true) {
      tmp = s->chain_input;

      switch (s->tag) {
      case SOURCE_DRIVER:
         rt_free_value(n, s->u.driver.waveforms.value);

         for (waveform_t *it = s->u.driver.waveforms.next, *next;
              it; it = next) {
            rt_free_value(n, it->value);
            next = it->next;
            rt_free(waveform_stack, it);
         }
         break;

      case SOURCE_PORT:
         if (s->u.port.conv_func != NULL) {
            RT_ASSERT(s->u.port.conv_func->refcnt > 0);
            if (--(s->u.port.conv_func->refcnt) == 0)
               free(s->u.port.conv_func);
         }
         break;
      }

      if (must_free) free(s);
   }

   if (n->net != NULL) {
      RT_ASSERT(n->net->refcnt > 0);
      if (--(n->net->refcnt) == 0) {
         rt_free_sens_list(&(n->net->pending));
         free(n->net);
      }
   }

   free(n->free_value);
}

static void rt_cleanup_signal(rt_signal_t *s)
{
   rt_nexus_t *n = &(s->nexus), *tmp;
   for (int i = 0; i < s->n_nexus; i++, n = tmp) {
      tmp = n->chain;
      rt_cleanup_nexus(n);
      if (i > 0) free(n);
   }

   if (s->index != NULL)
      ihash_free(s->index);

   if (s->flags & NET_F_IMPLICIT) {
      rt_implicit_t *imp = container_of(s, rt_implicit_t, signal);
      ffi_unref_closure(imp->closure);
      free(imp);
   }
   else
      free(s);
}

static void rt_cleanup_scope(rt_scope_t *scope)
{
   for (rt_proc_t *it = scope->procs, *tmp; it; it = tmp) {
      tmp = it->chain;
      mptr_free(mspace, &(it->privdata));
      tlab_release(&(it->tlab));
      free(it);
   }

   for (rt_signal_t *it = scope->signals, *tmp; it; it = tmp) {
      tmp = it->chain;
      rt_cleanup_signal(it);
   }

   for (rt_alias_t *it = scope->aliases, *tmp; it; it = tmp) {
      tmp = it->chain;
      free(it);
   }

   for (rt_scope_t *it = scope->child, *tmp; it; it = tmp) {
      tmp = it->chain;
      rt_cleanup_scope(it);
   }

   mptr_free(mspace, &(scope->privdata));
   free(scope);
}

static void rt_cleanup(void)
{
   while (heap_size(eventq_heap) > 0)
      rt_free(event_stack, heap_extract_min(eventq_heap));

   rt_free_delta_events(delta_proc);
   rt_free_delta_events(delta_driver);

   heap_free(eventq_heap);
   eventq_heap = NULL;

   hash_iter_t it = HASH_BEGIN;
   const void *key;
   void *value;
   while (hash_iter(res_memo_hash, &it, &key, &value))
      free(value);

   hash_free(res_memo_hash);
   res_memo_hash = NULL;

   rt_cleanup_scope(root);
   root = NULL;

   tlab_release(&__nvc_tlab);
   tlab_release(&spare_tlab);

   mspace_destroy(mspace);
   mspace = NULL;

   nexuses = NULL;
   nexus_tail = NULL;

   rt_free_sens_list(&resume);
   rt_free_sens_list(&postponed);
   rt_free_sens_list(&resume_watch);
   rt_free_sens_list(&postponed_watch);
   rt_free_sens_list(&implicit);

   event_t *e;
   while ((e = rt_pop_run_queue(&procq)))
      rt_free(event_stack, e);
   while ((e = rt_pop_run_queue(&effq)))
      rt_free(event_stack, e);
   while ((e = rt_pop_run_queue(&driverq)))
      rt_free(event_stack, e);

   while (watches != NULL) {
      rt_watch_t *next = watches->chain_all;
      rt_free(watch_stack, watches);
      watches = next;
   }

   for (int i = 0; i < RT_LAST_EVENT; i++) {
      while (global_cbs[i] != NULL) {
         callback_t *tmp = global_cbs[i]->next;
         rt_free(callback_stack, global_cbs[i]);
         global_cbs[i] = tmp;
      }
   }

   rt_alloc_stack_destroy(event_stack);
   rt_alloc_stack_destroy(waveform_stack);
   rt_alloc_stack_destroy(sens_list_stack);
   rt_alloc_stack_destroy(watch_stack);
   rt_alloc_stack_destroy(callback_stack);
}

static bool rt_stop_now(uint64_t stop_time)
{
   if ((delta_driver != NULL) || (delta_proc != NULL))
      return false;
   else if (heap_size(eventq_heap) == 0)
      return true;
   else if (force_stop)
      return true;
   else {
      event_t *peek = heap_min(eventq_heap);
      return peek->when > stop_time;
   }
}

static void rt_stats_print(void)
{
   nvc_rusage_t ru;
   nvc_rusage(&ru);

   if (profiling) {
      LOCAL_TEXT_BUF tb = tb_new();
      tb_printf(tb, "Signals: %d  (%.1f%% contiguous)\n", profile.n_signals,
                100.0f * ((float)profile.n_contig / profile.n_signals));
      //tb_printf(tb, "Nexuses: %-5d      Simple signals: %d (1:%.1f)\n",
      //          nnexus, profile.n_simple, (double)profile.n_simple / nnexus);
      tb_printf(tb, "Mapping:  direct:%d search:%d divide:%d\n",
                profile.nmap_direct, profile.nmap_search, profile.nmap_divide);
      //tb_printf(tb, "Processes: %-5d    Scopes: %d\n",
      // profile.n_procs, n_scopes);
      tb_printf(tb, "Cycles: %"PRIu64"\n", profile.deltas);
      tb_printf(tb, "Run queue:   min:%d max:%d avg:%.2f\n",
                profile.runq_min, profile.runq_max, profile.runq_mean);

      notef("Simulation profile data:%s", tb_get(tb));
   }

   notef("setup:%ums run:%ums maxrss:%ukB", ready_rusage.ms, ru.ms, ru.rss);
}

static void rt_reset_coverage(tree_t top)
{
   assert(cover == NULL);

   if ((cover = cover_read_tags(top)) == NULL)
      return;

   int32_t n_stmts, n_conds;
   cover_count_tags(cover, &n_stmts, &n_conds);

   int32_t *cover_stmts = jit_find_symbol("cover_stmts", false);
   if (cover_stmts != NULL)
      memset(cover_stmts, '\0', sizeof(int32_t) * n_stmts);

   int32_t *cover_conds = jit_find_symbol("cover_conds", false);
   if (cover_conds != NULL)
      memset(cover_conds, '\0', sizeof(int32_t) * n_conds);
}

static void rt_emit_coverage(tree_t top)
{
   if (cover != NULL) {
      const int32_t *cover_stmts = jit_find_symbol("cover_stmts", false);
      const int32_t *cover_conds = jit_find_symbol("cover_conds", false);
      if (cover_stmts != NULL)
         cover_report(top, cover, cover_stmts, cover_conds);
   }
}

static void rt_interrupt(void)
{
#ifdef __SANITIZE_THREAD__
   _Exit(1);
#else
   if (active_proc != NULL)
      rt_msg(NULL, DIAG_FATAL,
             "interrupted in process %s at %s+%d",
             istr(active_proc->name), fmt_time(now), iteration);
   else
      fatal("interrupted");
#endif
}

#ifdef __MINGW32__
static BOOL rt_win_ctrl_handler(DWORD fdwCtrlType)
{
   switch (fdwCtrlType) {
   case CTRL_C_EVENT:
      rt_interrupt();
      return TRUE;

   default:
      return FALSE;
   }
}
#endif

void rt_start_of_tool(tree_t top)
{
   jit_init(top);

#if RT_DEBUG && !defined NDEBUG
   warnf("runtime debug checks enabled");
#endif

#ifndef __MINGW32__
   struct sigaction sa;
   sa.sa_sigaction = (void*)rt_interrupt;
   sigemptyset(&sa.sa_mask);
   sa.sa_flags = SA_RESTART | SA_SIGINFO;

   sigaction(SIGINT, &sa, NULL);
#else
   if (!SetConsoleCtrlHandler(rt_win_ctrl_handler, TRUE))
      fatal_trace("SetConsoleCtrlHandler");
#endif

   trace_on = opt_get_int(OPT_RT_TRACE);
   profiling = opt_get_int(OPT_RT_PROFILE);

   if (profiling) {
      memset(&profile, '\0', sizeof(profile));
      profile.runq_min = ~0;
   }

   event_stack     = rt_alloc_stack_new(sizeof(event_t), "event");
   waveform_stack  = rt_alloc_stack_new(sizeof(waveform_t), "waveform");
   sens_list_stack = rt_alloc_stack_new(sizeof(sens_list_t), "sens_list");
   watch_stack     = rt_alloc_stack_new(sizeof(rt_watch_t), "watch");
   callback_stack  = rt_alloc_stack_new(sizeof(callback_t), "callback");

   const int heapsz = opt_get_int(OPT_HEAP_SIZE);
   if (heapsz < 0x100000)
      warnf("recommended heap size is at least 1M");

   mspace = mspace_new(heapsz);
   mspace_set_oom_handler(mspace, rt_mspace_oom_cb);

   rt_reset_coverage(top);

   nvc_rusage(&ready_rusage);
}

void rt_end_of_tool(tree_t top)
{
   rt_cleanup();
   rt_emit_coverage(top);

   jit_shutdown();

   if (opt_get_int(OPT_RT_STATS) || profiling)
      rt_stats_print();
}

int rt_run_sim(tree_t top, uint64_t stop_time)
{
   rt_setup(top);

   int rc = setjmp(abort_env);
   if (rc != 0)
      rc -= 1;   // rt_abort_sim adds 1 to exit code
   else {
      rt_initial(top);
      wave_restart();

      rt_global_event(RT_START_OF_SIMULATION);

      const int stop_delta = opt_get_int(OPT_STOP_DELTA);
      while (!rt_stop_now(stop_time))
         rt_cycle(stop_delta);
   }

   rt_global_event(RT_END_OF_SIMULATION);

   return rc;
}

void rt_set_timeout_cb(uint64_t when, timeout_fn_t fn, void *user)
{
   event_t *e = rt_alloc(event_stack);
   e->when         = now + when;
   e->kind         = EVENT_TIMEOUT;
   e->timeout.fn   = fn;
   e->timeout.user = user;

   deltaq_insert(e);
}

rt_watch_t *rt_set_event_cb(rt_signal_t *s, sig_event_fn_t fn, void *user,
                            bool postponed)
{
   if (fn == NULL) {
      // Find the first entry in the watch list and disable it
      for (rt_watch_t *it = watches; it != NULL; it = it->chain_all) {
         if ((it->signal == s) && (it->user_data == user)) {
            it->wakeable.pending = true;   // TODO: not a good way of doing this
            break;
         }
      }

      return NULL;
   }
   else {
      rt_watch_t *w = rt_alloc(watch_stack);
      RT_ASSERT(w != NULL);
      w->signal        = s;
      w->fn            = fn;
      w->chain_all     = watches;
      w->user_data     = user;

      w->wakeable.kind       = W_WATCH;
      w->wakeable.postponed  = postponed;
      w->wakeable.pending    = false;
      w->wakeable.wakeup_gen = 0;

      watches = w;

      rt_nexus_t *n = &(w->signal->nexus);
      for (int i = 0; i < s->n_nexus; i++, n = n->chain)
         rt_sched_event(&(rt_get_net(n)->pending), &(w->wakeable), true);

      return w;
   }
}

void rt_set_global_cb(rt_event_t event, rt_event_fn_t fn, void *user)
{
   RT_ASSERT(event < RT_LAST_EVENT);

   callback_t *cb = rt_alloc(callback_stack);
   cb->next = global_cbs[event];
   cb->fn   = fn;
   cb->user = user;

   global_cbs[event] = cb;
}

size_t rt_signal_string(rt_signal_t *s, const char *map, char *buf, size_t max)
{
   char *endp = buf + max;
   int offset = 0;
   rt_nexus_t *n = &(s->nexus);
   for (unsigned i = 0; i < s->n_nexus; i++, n = n->chain) {
      const char *vals = n->resolved;
      if (likely(map != NULL)) {
         for (int j = 0; j < n->width; j++) {
            if (buf + 1 < endp)
               *buf++ = map[(int)vals[j]];
         }
      }
      else {
         for (int j = 0; j < n->width; j++) {
            if (buf + 1 < endp)
               *buf++ = vals[j];
         }
      }

      if (buf < endp)
         *buf = '\0';

      offset += n->width;
   }

   return offset + 1;
}

size_t rt_signal_expand(rt_signal_t *s, int offset, uint64_t *buf, size_t max)
{
   rt_nexus_t *n = &(s->nexus);
   for (; offset > 0; n = n->chain)
      offset -= n->width;
   assert(offset == 0);

   for (; n != NULL && offset < max; n = n->chain) {
#define SIGNAL_READ_EXPAND_U64(type) do {                               \
         const type *sp = (type *)n->resolved;                          \
         for (int j = 0; (j < n->width) && (offset + j < max); j++)     \
            buf[offset + j] = sp[j];                                    \
      } while (0)

      FOR_ALL_SIZES(n->size, SIGNAL_READ_EXPAND_U64);

      offset += n->width;
   }

   return offset;
}

const void *rt_signal_value(rt_signal_t *s, int offset)
{
   const uint8_t *ptr = s->shared.data;
   for (rt_nexus_t *n = &(s->nexus); offset > 0; n = n->chain) {
      ptr += n->width * n->size;
      offset -= n->width;
   }
   assert(offset == 0);

   return ptr;
}

rt_signal_t *rt_find_signal(rt_scope_t *scope, tree_t decl)
{
   for (rt_signal_t *s = scope->signals; s; s = s->chain) {
      if (s->where == decl)
         return s;
   }

   for (rt_alias_t *a = scope->aliases; a; a = a->chain) {
      if (a->where == decl)
         return a->signal;
   }

   return NULL;
}

rt_scope_t *rt_find_scope(tree_t container)
{
   if (scopes == NULL)
      return NULL;
   else
      return hash_get(scopes, container);
}

rt_scope_t *rt_child_scope(rt_scope_t *scope, tree_t decl)
{
   for (rt_scope_t *s = scope->child; s != NULL; s = s->chain) {
      if (s->where == decl)
         return s;
   }

   return NULL;
}

bool rt_force_signal(rt_signal_t *s, const uint64_t *buf, size_t count)
{
   TRACE("force signal %s to %"PRIu64"%s",
         istr(tree_ident(s->where)), buf[0], count > 1 ? "..." : "");

   RT_ASSERT(can_create_delta);

   int offset = 0;
   rt_nexus_t *n = rt_split_nexus(s, offset, count);
   for (; count > 0; n = n->chain) {
      if (n->flags & NET_F_FORCED)
         rt_free_value(n, n->forcing);

      n->flags |= NET_F_FORCED;
      n->forcing = rt_alloc_value(n);

#define SIGNAL_FORCE_EXPAND_U64(type) do {                              \
         type *dp = (type *)rt_value_ptr(n, &(n->forcing));             \
         for (int i = 0; (i < n->width) && (offset + i < count); i++)   \
            dp[i] = buf[offset + i];                                    \
      } while (0)

      FOR_ALL_SIZES(n->size, SIGNAL_FORCE_EXPAND_U64);

      deltaq_insert_driver(0, n, NULL);

      offset += n->width;
      count -= n->width;
   }

   return (offset == count);
}

bool rt_can_create_delta(void)
{
   return can_create_delta;
}

uint64_t rt_now(unsigned *deltas)
{
   if (deltas != NULL)
      *deltas = MAX(iteration, 0);
   return now;
}

void rt_stop(void)
{
   force_stop = true;
}
