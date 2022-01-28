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

#include "rt.h"
#include "tree.h"
#include "lib.h"
#include "util.h"
#include "alloc.h"
#include "heap.h"
#include "common.h"
#include "cover.h"
#include "hash.h"
#include "debug.h"
#include "enode.h"
#include "ffi.h"
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
#define RT_DEBUG      0

typedef struct event         event_t;
typedef struct waveform      waveform_t;
typedef struct sens_list     sens_list_t;
typedef struct value         value_t;
typedef struct callback      callback_t;
typedef struct rt_nexus_s    rt_nexus_t;
typedef struct rt_scope_s    rt_scope_t;
typedef struct rt_source_s   rt_source_t;
typedef struct rt_implicit_s rt_implicit_t;

typedef void *(*proc_fn_t)(void *, rt_scope_t *);

typedef enum {
   W_PROC, W_WATCH, W_IMPLICIT
} wakeable_kind_t;

typedef struct {
   uint32_t        wakeup_gen;
   wakeable_kind_t kind : 8;
   bool            pending;
   bool            postponed;
} rt_wakeable_t;

typedef struct {
   rt_wakeable_t  wakeable;
   e_node_t       source;
   proc_fn_t      proc_fn;
   void          *tmp_stack;
   uint32_t       tmp_alloc;
   rt_scope_t    *scope;
   void          *privdata;
} rt_proc_t;

typedef enum {
   EVENT_TIMEOUT,
   EVENT_DRIVER,
   EVENT_PROCESS
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
   };
};

struct waveform {
   uint64_t    when;
   waveform_t *next;
   value_t    *values;
};

struct sens_list {
   rt_wakeable_t *wake;
   sens_list_t   *next;
   sens_list_t  **reenq;
   uint32_t       wakeup_gen;
};

typedef struct rt_source_s {
   rt_proc_t     *proc;
   rt_nexus_t    *input;
   rt_nexus_t    *output;
   waveform_t    *waveforms;
   ffi_closure_t *conv_func;
} rt_source_t;

struct value {
   value_t *next;
   union {
      char     data[0];
      uint64_t qwords[0];
   };
} __attribute__((aligned(8)));

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

typedef enum {
   NET_F_FORCED       = (1 << 0),
   NET_F_OWNS_MEM     = (1 << 1),
   NET_F_LAST_VALUE   = (1 << 2),
   NET_F_PENDING      = (1 << 3),
   NET_F_IMPLICIT     = (1 << 4),
   NET_F_REGISTER     = (1 << 5),
   NET_F_DISCONNECTED = (1 << 6),
} net_flags_t;

typedef struct rt_nexus_s {
   e_node_t      enode;
   uint32_t      width;
   uint32_t      size;
   value_t      *free_values;
   uint64_t      last_event;
   uint64_t      last_active;
   int32_t       event_delta;
   int32_t       active_delta;
   sens_list_t  *pending;
   value_t      *forcing;
   res_memo_t   *resolution;
   net_flags_t   flags;
   unsigned      rank;
   unsigned      n_sources;
   unsigned      n_signals;
   unsigned      n_outputs;
   rt_source_t  *sources;
   rt_signal_t **signals;
   rt_source_t **outputs;
   void         *resolved;
   void         *last_value;
   unsigned     *offsets;
} rt_nexus_t;

// The code generator knows the layout of this struct
typedef struct {
   uint32_t  id;
   uint32_t  __pad;
   void     *resolved;
   void     *last_value;
} sig_shared_t;

STATIC_ASSERT(sizeof(sig_shared_t) == 24);

typedef enum {
   NEXUS_MAP_SEARCH,
   NEXUS_MAP_DIVIDE,
   NEXUS_MAP_DIRECT
} rt_nexus_map_t;

typedef struct rt_signal_s {
   sig_shared_t     shared;
   e_node_t         enode;
   uint32_t         width;
   uint32_t         size;
   rt_nexus_map_t   nmap_kind;
   uint32_t         nmap_param;
   net_flags_t      flags;
   uint32_t         n_nexus;
   rt_nexus_t      *nexus[0];
} rt_signal_t;

typedef struct rt_implicit_s {
   rt_wakeable_t  wakeable;
   ffi_closure_t *closure;
   rt_signal_t    signal;   // Has a flexible member
} rt_implicit_t;

typedef struct rt_scope_s {
   rt_signal_t **signals;
   unsigned      n_signals;
   rt_proc_t    *procs;
   unsigned      n_procs;
   e_node_t      enode;
   void         *privdata;
   rt_scope_t   *parent;
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
static rt_scope_t      *scopes = NULL;
static rt_run_queue_t   timeoutq;
static rt_run_queue_t   driverq;
static rt_run_queue_t   procq;
static heap_t          *eventq_heap = NULL;
static heap_t          *rankn_heap = NULL;
static unsigned         n_scopes = 0;
static unsigned         n_nexuses = 0;
static uint64_t         now = 0;
static int              iteration = -1;
static bool             trace_on = false;
static nvc_rusage_t     ready_rusage;
static bool             aborted = false;
static sens_list_t     *resume = NULL;
static sens_list_t     *postponed = NULL;
static sens_list_t     *resume_watch = NULL;
static sens_list_t     *postponed_watch = NULL;
static sens_list_t     *implicit = NULL;
static rt_watch_t      *watches = NULL;
static event_t         *delta_proc = NULL;
static event_t         *delta_driver = NULL;
static void            *global_tmp_stack = NULL;
static void            *proc_tmp_stack = NULL;
static uint32_t         global_tmp_alloc;
static hash_t          *res_memo_hash = NULL;
static side_effect_t    init_side_effect = SIDE_EFFECT_ALLOW;
static bool             force_stop;
static bool             can_create_delta;
static callback_t      *global_cbs[RT_LAST_EVENT];
static rt_severity_t    exit_severity = SEVERITY_ERROR;
static bool             profiling = false;
static rt_profile_t     profile;
static rt_nexus_t      *nexuses = NULL;
static cover_tagging_t *cover = NULL;
static unsigned         highest_rank;

static rt_alloc_stack_t event_stack = NULL;
static rt_alloc_stack_t waveform_stack = NULL;
static rt_alloc_stack_t sens_list_stack = NULL;
static rt_alloc_stack_t watch_stack = NULL;
static rt_alloc_stack_t callback_stack = NULL;

static void deltaq_insert_proc(uint64_t delta, rt_proc_t *wake);
static void deltaq_insert_driver(uint64_t delta, rt_nexus_t *nexus,
                                 rt_source_t *source);
static void rt_sched_driver(rt_nexus_t *nexus, uint64_t after,
                            uint64_t reject, value_t *values);
static void rt_sched_event(sens_list_t **list, rt_wakeable_t *proc, bool recur);
static void *rt_tmp_alloc(size_t sz);
static value_t *rt_alloc_value(rt_nexus_t *n);
static res_memo_t *rt_memo_resolution_fn(rt_signal_t *signal,
                                         rt_resolution_t *resolution);
static inline unsigned rt_signal_nexus_index(rt_signal_t *s, unsigned offset);
static void _tracef(const char *fmt, ...);

#define GLOBAL_TMP_STACK_SZ (8 * 1024 * 1024)
#define PROC_TMP_STACK_SZ   (64 * 1024)
#define FMT_VALUES_SZ       128

#if RT_DEBUG
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

static char *fmt_nexus_r(rt_nexus_t *n, const void *values,
                         char *buf, size_t max)
{
   char *p = buf;
   const uint8_t *vptr = values;

   for (unsigned i = 0; i < n->size * n->width; i++) {
      if (buf + max - p <= 5)
         return p + checked_sprintf(p, buf + max - p, "...");
      else
         p += checked_sprintf(p, buf + max - p, "%02x", *vptr++);
   }

   return p;
}

static const char *fmt_nexus(rt_nexus_t *n, const void *values)
{
   static char buf[FMT_VALUES_SZ*2 + 2];
   fmt_nexus_r(n, values, buf, sizeof(buf));
   return buf;
}

static const char *fmt_values(rt_signal_t *s, const void *values,
                              unsigned offset, uint32_t len)
{
   static char buf[FMT_VALUES_SZ*2 + 2];

   char *p = buf;
   const uint8_t *vptr = values;
   unsigned index = rt_signal_nexus_index(s, offset);
   while (len > 0 && buf + sizeof(buf) - p > 5)  {
      RT_ASSERT(index < s->n_nexus);
      rt_nexus_t *n = s->nexus[index++];
      len -= n->width;
      RT_ASSERT(len >= 0);

      if (p > buf) p += checked_sprintf(p, buf + sizeof(buf) - p, ",");

      p = fmt_nexus_r(n, vptr, p, buf + sizeof(buf) - p);
      vptr += n->size * n->width;
   }

   return buf;
}

static tree_t rt_find_enclosing_decl(ident_t unit_name, const char *symbol)
{
   tree_t unit = lib_get_qualified(unit_name);
   if (unit == NULL)
      return NULL;

   if (tree_kind(unit) == T_PACKAGE) {
      ident_t body_name = ident_prefix(unit_name, ident_new("body"), '-');
      tree_t body = lib_get_qualified(body_name);
      if (body != NULL)
         unit = body;
   }

   static shash_t *cache = NULL;
   if (cache == NULL)
      cache = shash_new(256);

   tree_t enclosing = shash_get(cache, symbol);
   if (enclosing == NULL) {
      if ((enclosing = find_mangled_decl(unit, ident_new(symbol))))
         shash_put(cache, symbol, enclosing);
   }

   return enclosing;
}

static void rt_fmt_enclosing(text_buf_t *tb, tree_t enclosing,
                             const char *symbol, const char *prefix)
{
   switch (tree_kind(enclosing)) {
   case T_PROCESS:
      tb_printf(tb, "\r\t%sProcess %s", prefix,
                istr(e_path(active_proc->source)));
      break;
   case T_FUNC_BODY:
   case T_FUNC_DECL:
      tb_printf(tb, "\r\t%sFunction %s", prefix, type_pp(tree_type(enclosing)));
      break;
   case T_PROC_BODY:
   case T_PROC_DECL:
      tb_printf(tb, "\r\t%sProcedure %s", prefix,
                type_pp(tree_type(enclosing)));
      break;
   case T_TYPE_DECL:
      if (strstr(symbol, "$value"))
         tb_printf(tb, "\r\t%sAttribute %s'VALUE",
                   prefix, istr(tree_ident(enclosing)));
      else
         tb_printf(tb, "\r\t%sType %s", prefix, istr(tree_ident(enclosing)));
      break;
   case T_BLOCK:
      tb_printf(tb, "\r\t%sProcess (init)", prefix);
      break;
   default:
      tb_printf(tb, "\r\t%s%s", prefix, istr(tree_ident(enclosing)));
      break;
   }
}

static text_buf_t *rt_fmt_trace(const loc_t *fixed)
{
   debug_info_t *di = debug_capture();
   text_buf_t *tb = tb_new();

   bool found_fixed = false;
   const int nframes = debug_count_frames(di);
   for (int i = 0; i < nframes; i++) {
      const debug_frame_t *f = debug_get_frame(di, i);
      if (f->kind != FRAME_VHDL || f->vhdl_unit == NULL || f->symbol == NULL)
         continue;

      for (debug_inline_t *inl = f->inlined; inl != NULL; inl = inl->next) {
         tree_t enclosing = rt_find_enclosing_decl(inl->vhdl_unit, inl->symbol);
         if (enclosing == NULL)
            continue;

         found_fixed = true;  // DWARF data should be most accurate

         // Processes should never be inlined
         assert(tree_kind(enclosing) != T_PROCESS);

         rt_fmt_enclosing(tb, enclosing, inl->symbol, "Inlined ");
         tb_printf(tb, "\r\t    File %s, Line %u", inl->srcfile, inl->lineno);
      }

      tree_t enclosing = rt_find_enclosing_decl(f->vhdl_unit, f->symbol);
      if (enclosing == NULL)
         continue;

      unsigned lineno = f->lineno;
      const char *srcfile = f->srcfile;
      if (fixed != NULL && !found_fixed) {
         lineno = fixed->first_line;
         srcfile = loc_file_str(fixed);
         found_fixed = true;
      }
      else if (f->lineno == 0) {
         // Exact DWARF debug info not available
         const loc_t *loc = tree_loc(enclosing);
         lineno = loc->first_line;
         srcfile = loc_file_str(loc);
      }

      rt_fmt_enclosing(tb, enclosing, f->symbol, "");
      tb_printf(tb, "\r\t    File %s, Line %u", srcfile, lineno);
   }

   if (fixed != NULL && (nframes == 0 || !found_fixed)) {
      const char *pname = active_proc == NULL
         ? "(init)" : istr(e_path(active_proc->source));
      tb_printf(tb, "\r\tProcess %s", pname);
      tb_printf(tb, "\r\t    File %s, Line %u", loc_file_str(fixed),
                fixed->first_line);
   }

   debug_free(di);
   return tb;
}

typedef void (*rt_msg_fn_t)(const char *, ...);

__attribute__((format(printf, 3, 4)))
static void rt_msg(const loc_t *where, rt_msg_fn_t fn, const char *fmt, ...)
{
   va_list ap;
   va_start(ap, fmt);

   char *LOCAL buf = xvasprintf(fmt, ap);
   LOCAL_TEXT_BUF trace = rt_fmt_trace(where);

   va_end(ap);

   (*fn)("%s%s", buf, tb_get(trace));
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
   char *buf = rt_tmp_alloc(result_len);

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

static unsigned rt_signal_nexus_index(rt_signal_t *s, unsigned offset)
{
   unsigned nid = 0;

   switch (s->nmap_kind) {
   case NEXUS_MAP_SEARCH:
      while (offset > 0) {
         RT_ASSERT(nid < s->n_nexus);
         rt_nexus_t *n = s->nexus[nid++];
         offset -= n->width * n->size;
      }
      RT_ASSERT(offset == 0);
      break;

   case NEXUS_MAP_DIVIDE:
      nid = offset / s->nmap_param;
      break;

   case NEXUS_MAP_DIRECT:
      nid = offset;
      break;
   }

   RT_ASSERT(nid < s->n_nexus);
   return nid;
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
            istr(e_path(active_proc->source)));
}

static inline tree_t rt_locus_to_tree(const char *unit, unsigned offset)
{
   return tree_from_locus(ident_new(unit), offset, lib_get_qualified);
}

////////////////////////////////////////////////////////////////////////////////
// Runtime support functions

DLLEXPORT void     *_tmp_stack;
DLLEXPORT uint32_t  _tmp_alloc;

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
         istr(e_path(s->enode)), offset, scalar, fmt_time(after),
         fmt_time(reject));

   rt_check_postponed(after);

   rt_nexus_t *n = s->nexus[rt_signal_nexus_index(s, offset)];

   value_t *values_copy = rt_alloc_value(n);
   values_copy->qwords[0] = scalar;

   rt_sched_driver(n, after, reject, values_copy);
}

DLLEXPORT
void _sched_waveform(sig_shared_t *ss, uint32_t offset, void *values,
                     int32_t len, int64_t after, int64_t reject)
{
   rt_signal_t *s = container_of(ss, rt_signal_t, shared);

   TRACE("_sched_waveform %s+%d value=%s len=%d after=%s reject=%s",
         istr(e_path(s->enode)), offset, fmt_values(s, values, offset, len),
         len, fmt_time(after), fmt_time(reject));

   rt_check_postponed(after);

   char *vptr = values;
   unsigned index = rt_signal_nexus_index(s, offset);
   while (len > 0) {
      RT_ASSERT(index < s->n_nexus);
      rt_nexus_t *n = s->nexus[index++];
      len -= n->width;
      RT_ASSERT(len >= 0);

      const size_t valuesz = n->width * n->size;
      value_t *values_copy = rt_alloc_value(n);
      memcpy(values_copy->data, vptr, valuesz);
      vptr += valuesz;

      rt_sched_driver(n, after, reject, values_copy);
   }
}

DLLEXPORT
void _disconnect(sig_shared_t *ss, uint32_t offset, int32_t count,
                 int64_t after, int64_t reject)
{
   rt_signal_t *s = container_of(ss, rt_signal_t, shared);

   TRACE("_disconnect %s+%d len=%d after=%s reject=%s",
         istr(e_path(s->enode)), offset, count, fmt_time(after),
         fmt_time(reject));

   rt_check_postponed(after);

   unsigned index = rt_signal_nexus_index(s, offset);
   while (count > 0) {
      RT_ASSERT(index < s->n_nexus);
      rt_nexus_t *n = s->nexus[index++];
      count -= n->width;

      rt_sched_driver(n, after, reject, NULL);
      n->flags |= NET_F_DISCONNECTED;
   }
}

DLLEXPORT
void _sched_event(sig_shared_t *ss, uint32_t offset, int32_t count)
{
   rt_signal_t *s = container_of(ss, rt_signal_t, shared);

   TRACE("_sched_event %s+%d count=%d proc %s", istr(e_path(s->enode)),
         offset, count, istr(e_path(active_proc->source)));

   unsigned index = rt_signal_nexus_index(s, offset);
   while (count > 0) {
      RT_ASSERT(index < s->n_nexus);
      rt_nexus_t *n = s->nexus[index++];
      rt_sched_event(&(n->pending), &(active_proc->wakeable), false);

      count -= n->width;
      RT_ASSERT(count >= 0);
   }
}

DLLEXPORT
void _private_stack(void)
{
   TRACE("_private_stack %p %d %d", active_proc->tmp_stack,
         active_proc->tmp_alloc, _tmp_alloc);

   if (active_proc->tmp_stack == NULL && _tmp_alloc > 0) {
      active_proc->tmp_stack = _tmp_stack;

      proc_tmp_stack = mmap_guarded(PROC_TMP_STACK_SZ, "process temp stack");
   }

   active_proc->tmp_alloc = _tmp_alloc;
}

DLLEXPORT
sig_shared_t *_link_signal(const char *name)
{
   ident_t id = ident_new(name);

   for (unsigned i = 0; i < active_scope->n_signals; i++) {
      rt_signal_t *signal = active_scope->signals[i];
      if (e_ident(signal->enode) == id)
         return &(signal->shared);
   }

   fatal("failed to link signal %s in scope %s", name,
         istr(e_instance(active_scope->enode)));
}

DLLEXPORT
void _init_signal(sig_shared_t *ss, uint32_t offset, uint32_t count,
                  uint32_t size, const uint8_t *values,
                  rt_resolution_t *resolution)
{
   rt_signal_t *s = container_of(ss, rt_signal_t, shared);

   TRACE("_init_signal %s+%d values=%s count=%d%s",
         istr(e_path(s->enode)), offset, fmt_values(s, values, offset, count),
         count, resolution ? " resolved" : "");

   res_memo_t *memo = NULL;
   if (resolution != NULL)
      memo = rt_memo_resolution_fn(s, resolution);

   unsigned index = rt_signal_nexus_index(s, offset);
   while (count > 0) {
      RT_ASSERT(index < s->n_nexus);
      rt_nexus_t *n = s->nexus[index++];
      RT_ASSERT(n->size == size);

      if (s == n->signals[0]) {
         RT_ASSERT(n->resolution == NULL || n->resolution == memo);
         n->resolution = memo;

         memcpy(n->resolved, values, n->size * n->width);
         if (n->flags & NET_F_LAST_VALUE)
            memcpy(n->last_value, values, n->size * n->width);
      }

      count -= n->width;
      values += n->width * n->size;
      RT_ASSERT(count >= 0);
   }
}

DLLEXPORT
void _implicit_signal(sig_shared_t *ss, uint32_t kind, ffi_closure_t *closure)
{
   rt_signal_t *s = container_of(ss, rt_signal_t, shared);

   TRACE("_implicit_signal %s kind=%d fn=%p context=%p",
         istr(e_path(s->enode)), kind, closure->fn, closure->context);

   ffi_closure_t *copy = xmalloc(sizeof(ffi_closure_t));
   *copy = *closure;
   copy->refcnt = 1;

   assert(s->flags & NET_F_IMPLICIT);

   rt_implicit_t *imp = container_of(s, rt_implicit_t, signal);
   imp->closure = copy;
}

DLLEXPORT
void _convert_signal(sig_shared_t *ss, uint32_t offset, uint32_t count,
                     ffi_closure_t *closure)
{
   rt_signal_t *s = container_of(ss, rt_signal_t, shared);

   TRACE("_convert_signal %s+%d count=%d fn=%p context=%p",
         istr(e_path(s->enode)), offset, count, closure->fn, closure->context);

   ffi_closure_t *copy = xmalloc(sizeof(ffi_closure_t));
   *copy = *closure;
   copy->refcnt = 1;

   unsigned index = rt_signal_nexus_index(s, offset);
   while (count > 0) {
      RT_ASSERT(index < s->n_nexus);
      rt_nexus_t *n = s->nexus[index++];

      for (unsigned i = 0; i < n->n_sources; i++) {
         if (n->sources[i].proc == NULL) {  // Is a port source
            (copy->refcnt)++;
            n->sources[i].conv_func = copy;
         }
      }

      count -= n->width;
      RT_ASSERT(count >= 0);
   }

   ffi_unref_closure(copy);
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

   static const uint8_t def_str[] = "Assertion violation.";

   if (msg == NULL) {
      msg = def_str;
      msg_len = sizeof(def_str) - 1;
   }

   if (init_side_effect != SIDE_EFFECT_ALLOW) {
      init_side_effect = SIDE_EFFECT_OCCURRED;
      return;
   }

   void (*fn)(const char *fmt, ...) = fatal;

   switch (severity) {
   case SEVERITY_NOTE:    fn = notef; break;
   case SEVERITY_WARNING: fn = warnf; break;
   case SEVERITY_ERROR:
   case SEVERITY_FAILURE: fn = errorf; break;
   }

   if (severity >= exit_severity)
      fn = fatal;

   tree_t where = rt_locus_to_tree(locus_unit, locus_offset);

   if (hint_valid) {
      assert(tree_kind(where) == T_FCALL);
      type_t p0_type = tree_type(tree_value(tree_param(where, 0)));
      type_t p1_type = tree_type(tree_value(tree_param(where, 1)));

      LOCAL_TEXT_BUF tb = tb_new();
      tb_cat(tb, "condition ");
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

      hint_at(tree_loc(where), "%s", tb_get(tb));
   }

   char tmbuf[64];
   rt_fmt_now(tmbuf, sizeof(tmbuf));

   rt_msg(tree_loc(where), fn, "%s: Assertion %s: %.*s",
          tmbuf, levels[severity], msg_len, msg);
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

   void (*fn)(const char *fmt, ...) = fatal;

   switch (severity) {
   case SEVERITY_NOTE:    fn = notef; break;
   case SEVERITY_WARNING: fn = warnf; break;
   case SEVERITY_ERROR:
   case SEVERITY_FAILURE: fn = errorf; break;
   }

   if (severity >= exit_severity)
      fn = fatal;

   char tmbuf[64];
   rt_fmt_now(tmbuf, sizeof(tmbuf));

   tree_t where = rt_locus_to_tree(locus_unit, locus_offset);

   rt_msg(tree_loc(where), fn, "%s: Report %s: %.*s",
          tmbuf, levels[severity], msg_len, msg);
}

DLLEXPORT
void __nvc_index_fail(int32_t value, int32_t left, int32_t right, int8_t dir,
                      DEBUG_LOCUS(locus))
{
   tree_t where = rt_locus_to_tree(locus_unit, locus_offset);
   type_t type = tree_type(where);

   LOCAL_TEXT_BUF tb = tb_new();
   tb_cat(tb, "index ");
   to_string(tb, type, value);
   tb_printf(tb, " outside of %s range ", type_pp(type));
   to_string(tb, type, left);
   tb_cat(tb, dir == RANGE_TO ? " to " : " downto ");
   to_string(tb, type, right);

   rt_msg(tree_loc(where), fatal, "%s", tb_get(tb));
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
      tb_printf(tb, " for parameter %s", istr(tree_ident(hint)));
      break;
   case T_ATTR_REF:
      tb_printf(tb, " for attribute '%s", istr(tree_ident(hint)));
      break;
   default:
      break;
   }

   rt_msg(tree_loc(where), fatal, "%s", tb_get(tb));
}

DLLEXPORT
void __nvc_length_fail(int32_t left, int32_t right, int32_t dim,
                       DEBUG_LOCUS(locus))
{
   tree_t where = rt_locus_to_tree(locus_unit, locus_offset);

   LOCAL_TEXT_BUF tb = tb_new();
   tb_printf(tb, "%s length %d",
             tree_kind(where) == T_PORT_DECL ? "actual" : "value", right);
   if (dim > 0)
      tb_printf(tb, " for dimension %d", dim);
   tb_cat(tb, " does not match ");

   switch (tree_kind(where)) {
   case T_PORT_DECL:
      tb_printf(tb, "formal parameter %s", istr(tree_ident(where)));
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
   default:
      tb_cat(tb, "target");
      break;
   }

   tb_printf(tb, " length %d", left);

   rt_msg(tree_loc(where), fatal, "%s", tb_get(tb));
}

DLLEXPORT
void _canon_value(const uint8_t *raw_str, int32_t str_len, ffi_uarray_t *u)
{
   char *buf = rt_tmp_alloc(str_len), *p = buf;
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
         rt_msg(NULL, fatal, "found invalid characters \"%.*s\" after value "
                "\"%.*s\"", (int)(str_len - pos), raw_str + pos, str_len,
                (const char *)raw_str);
      }
   }

   *u = wrap_str(buf, p - buf);
}

DLLEXPORT
void _int_to_string(int64_t value, ffi_uarray_t *u)
{
   char *buf = rt_tmp_alloc(20);
   size_t len = checked_sprintf(buf, 20, "%"PRIi64, value);

   *u = wrap_str(buf, len);
}

DLLEXPORT
void _real_to_string(double value, ffi_uarray_t *u)
{
   char *buf = rt_tmp_alloc(32);
   size_t len = checked_sprintf(buf, 32, "%.*g", 17, value);

   *u = wrap_str(buf, len);
}

DLLEXPORT
int64_t _string_to_int(const uint8_t *raw_str, int32_t str_len, uint8_t **tail)
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
      rt_msg(NULL, fatal, "invalid integer value "
             "\"%.*s\"", str_len, (const char *)raw_str);

   if (tail != NULL)
      *tail = (uint8_t *)p;
   else {
      for (; p < endp && *p != '\0'; p++) {
         if (!isspace((int)*p)) {
            rt_msg(NULL, fatal, "found invalid characters \"%.*s\" after value "
                   "\"%.*s\"", (int)(endp - p), p, str_len,
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
      rt_msg(NULL, fatal, "invalid real value "
             "\"%.*s\"", str_len, (const char *)raw_str);

   if (tail != NULL)
      *tail = (uint8_t *)p;
   else {
      for (; p < null + str_len && *p != '\0'; p++) {
         if (!isspace((int)*p)) {
            rt_msg(NULL, fatal, "found invalid characters \"%.*s\" after value "
                   "\"%.*s\"", (int)(null + str_len - p), p, str_len,
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
   rt_msg(tree_loc(where), fatal, "division by zero");
}

DLLEXPORT
void __nvc_null_deref(DEBUG_LOCUS(locus))
{
   tree_t where = rt_locus_to_tree(locus_unit, locus_offset);
   rt_msg(tree_loc(where), fatal, "null access dereference");
}

DLLEXPORT
bool _nvc_ieee_warnings(void)
{
   return opt_get_int("ieee-warnings");
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
      rt_msg(NULL, fatal, "invalid UNIT argument %"PRIi64" in TO_STRING", unit);
   }

   size_t max_len = 16 + strlen(unit_str) + 1;
   char *buf = rt_tmp_alloc(max_len);

   size_t len;
   if (value % unit == 0)
      len = checked_sprintf(buf, max_len, "%"PRIi64" %s",
                            value / unit, unit_str);
   else
      len = checked_sprintf(buf, max_len, "%g %s",
                            (double)value / (double)unit, unit_str);

   TRACE("result=%s", buf);
   *u = wrap_str(buf, len);
}

DLLEXPORT
void _std_to_string_real_digits(double value, int32_t digits, ffi_uarray_t *u)
{
   size_t max_len = 32;
   char *buf = rt_tmp_alloc(max_len);

   size_t len;
   if (digits == 0)
      len = checked_sprintf(buf, max_len, "%.17g", value);
   else
      len = checked_sprintf(buf, max_len, "%.*f", digits, value);

   *u = wrap_str(buf, len);
}

DLLEXPORT
void _std_to_string_real_format(double value, EXPLODED_UARRAY(fmt), ffi_uarray_t *u)
{
   char *LOCAL fmt_cstr = xmalloc(fmt_length + 1);
   memcpy(fmt_cstr, fmt_ptr, fmt_length);
   fmt_cstr[fmt_length] = '\0';

   if (fmt_cstr[0] != '%')
      rt_msg(NULL, fatal, "conversion specification must start with '%%'");

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
         rt_msg(NULL, fatal, "illegal character '%c' in format \"%s\"",
                *p, fmt_cstr + 1);
      }
   }

   size_t max_len = 64;
   char *buf = rt_tmp_alloc(max_len);
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

   exit(status);
}

DLLEXPORT
void _debug_out(int32_t val, int32_t reg)
{
   printf("DEBUG: r%d val=%"PRIx32"\n", reg, val);
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
}

DLLEXPORT
int64_t _last_event(sig_shared_t *ss, uint32_t offset, int32_t count)
{
   rt_signal_t *s = container_of(ss, rt_signal_t, shared);

   TRACE("_last_event %s offset=%d count=%d",
         istr(e_path(s->enode)), offset, count);

   int64_t last = INT64_MAX;

   unsigned index = rt_signal_nexus_index(s, offset);
   while (count > 0) {
      RT_ASSERT(index < s->n_nexus);
      rt_nexus_t *n = s->nexus[index++];

      if (n->last_event <= now)
         last = MIN(last, now - n->last_event);

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
         istr(e_path(s->enode)), offset, count);

   int64_t last = INT64_MAX;

   unsigned index = rt_signal_nexus_index(s, offset);
   while (count > 0) {
      RT_ASSERT(index < s->n_nexus);
      rt_nexus_t *n = s->nexus[index++];

      if (n->last_active <= now)
         last = MIN(last, now - n->last_active);

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
         istr(e_path(s->enode)), offset, count);

   int ntotal = 0, ndriving = 0;
   bool found = false;
   unsigned index = rt_signal_nexus_index(s, offset);
   while (count > 0) {
      RT_ASSERT(index < s->n_nexus);
      rt_nexus_t *n = s->nexus[index++];

      int driver;
      for (driver = 0; driver < n->n_sources; driver++) {
         if (likely(n->sources[driver].proc == active_proc)) {
            if (n->sources[driver].waveforms->values != NULL)
               ndriving++;
            found = true;
            break;
         }
      }

      ntotal++;
      count -= n->width;
      RT_ASSERT(count >= 0);
   }

   if (!found)
      rt_msg(NULL, fatal, "process %s does not contain a driver for %s",
             istr(e_path(active_proc->source)), istr(e_ident(s->enode)));

   return ntotal == ndriving;
}

DLLEXPORT
void *_driving_value(sig_shared_t *ss, uint32_t offset, int32_t count)
{
   rt_signal_t *s = container_of(ss, rt_signal_t, shared);

   TRACE("_driving_value %s offset=%d count=%d",
         istr(e_path(s->enode)), offset, count);

   void *result = rt_tmp_alloc(s->size);

   uint8_t *p = result;
   unsigned index = rt_signal_nexus_index(s, offset);
   while (count > 0) {
      RT_ASSERT(index < s->n_nexus);
      rt_nexus_t *n = s->nexus[index++];

      int driver;
      for (driver = 0; driver < n->n_sources; driver++) {
         if (likely(n->sources[driver].proc == active_proc))
            break;
      }

      if (driver == n->n_sources)
         rt_msg(NULL, fatal, "process %s does not contain a driver for %s",
                istr(e_path(active_proc->source)), istr(e_ident(s->enode)));

      memcpy(p, n->sources[driver].waveforms->values->data, n->width * n->size);
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
         istr(e_path(s->enode)), offset, count);

   unsigned index = rt_signal_nexus_index(s, offset);
   while (count > 0) {
      RT_ASSERT(index < s->n_nexus);
      rt_nexus_t *n = s->nexus[index++];

      if (n->last_active == now && n->active_delta == iteration)
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
         istr(e_path(s->enode)), offset, count);

   unsigned index = rt_signal_nexus_index(s, offset);
   while (count > 0) {
      RT_ASSERT(index < s->n_nexus);
      rt_nexus_t *n = s->nexus[index++];

      if (n->last_event == now && n->event_delta == iteration)
         return 1;

      count -= n->width;
      RT_ASSERT(count >= 0);
   }

   return 0;
}

DLLEXPORT
void _file_open(int8_t *status, void **_fp, uint8_t *name_bytes,
                int32_t name_len, int8_t mode)
{
   FILE **fp = (FILE **)_fp;
   if (*fp != NULL) {
      if (status != NULL) {
         *status = 1;   // STATUS_ERROR
         return;
      }
      else {
         // This is to support closing a file implicitly when the
         // design is reset
         fclose(*fp);
      }
   }

   char *fname LOCAL = xmalloc(name_len + 1);
   memcpy(fname, name_bytes, name_len);
   fname[name_len] = '\0';

   TRACE("_file_open %s fp=%p mode=%d", fname, fp, mode);

   const char *mode_str[] = {
      "rb", "wb", "w+b"
   };
   RT_ASSERT(mode < ARRAY_LEN(mode_str));

   if (status != NULL)
      *status = 0;   // OPEN_OK

   if (strcmp(fname, "STD_INPUT") == 0)
      *fp = stdin;
   else if (strcmp(fname, "STD_OUTPUT") == 0)
      *fp = stdout;
   else
      *fp = fopen(fname, mode_str[mode]);

   if (*fp == NULL) {
      if (status == NULL)
         rt_msg(NULL, fatal, "failed to open %s: %s", fname, last_os_error());
      else {
         switch (errno) {
         case ENOENT:
            *status = 2;   // NAME_ERROR
            break;
         case EPERM:
            *status = 3;   // MODE_ERROR
            break;
         default:
            rt_msg(NULL, fatal, "%s: %s", fname, last_os_error());
         }
      }
   }
}

DLLEXPORT
void _file_write(void **_fp, uint8_t *data, int32_t len)
{
   FILE **fp = (FILE **)_fp;

   if (*fp == NULL)
      rt_msg(NULL, fatal, "write to closed file");

   fwrite(data, 1, len, *fp);
}

DLLEXPORT
void _file_read(void **_fp, uint8_t *data, int32_t size, int32_t count,
                int32_t *out)
{
   FILE **fp = (FILE **)_fp;

   if (*fp == NULL)
      rt_msg(NULL, fatal, "read from closed file");

   size_t n = fread(data, size, count, *fp);
   if (out != NULL)
      *out = n;
}

DLLEXPORT
void _file_close(void **_fp)
{
   FILE **fp = (FILE **)_fp;

   TRACE("_file_close fp=%p", fp);

   if (*fp == NULL)
      rt_msg(NULL, fatal, "attempt to close already closed file");

   fclose(*fp);
   *fp = NULL;
}

DLLEXPORT
int8_t _endfile(void *_f)
{
   FILE *f = _f;

   if (f == NULL)
      rt_msg(NULL, fatal, "ENDFILE called on closed file");

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
      rt_msg(NULL, fatal, "FLUSH called on closed file");

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
      fprintf(stderr, "driver\t %s\n", istr(e_ident(e->driver.nexus->enode)));
      break;
   case EVENT_PROCESS:
      fprintf(stderr, "process\t %s%s\n", istr(e_path(e->proc.proc->source)),
              (e->proc.wakeup_gen == e->proc.proc->wakeable.wakeup_gen)
              ? "" : " (stale)");
      break;
   case EVENT_TIMEOUT:
      fprintf(stderr, "timeout\t %p %p\n", e->timeout.fn, e->timeout.user);
      break;
   }
}

static void deltaq_dump(void)
{
   for (event_t *e = delta_driver; e != NULL; e = e->delta_chain)
      fprintf(stderr, "delta\tdriver\t %s\n",
              istr(e_ident(e->driver.nexus->enode)));

   for (event_t *e = delta_proc; e != NULL; e = e->delta_chain)
      fprintf(stderr, "delta\tprocess\t %s%s\n",
              istr(e_path(e->proc.proc->source)),
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
         resolution->closure.fn, type_pp(e_type(signal->enode)));

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

static value_t *rt_alloc_value(rt_nexus_t *n)
{
   if (n->free_values == NULL) {
      const size_t size = MAX(sizeof(uint64_t), n->size * n->width);
      value_t *v = xmalloc(sizeof(struct value) + size);
      v->next = NULL;
      return v;
   }
   else {
      value_t *v = n->free_values;
      n->free_values = v->next;
      v->next = NULL;
      return v;
   }
}

static void rt_free_value(rt_nexus_t *n, value_t *v)
{
   if (v != NULL) {
      RT_ASSERT(v->next == NULL);
      v->next = n->free_values;
      n->free_values = v;
   }
}

static void *rt_tmp_alloc(size_t sz)
{
   // Allocate sz bytes that will be freed by the active process

   uint8_t *ptr = (uint8_t *)_tmp_stack + _tmp_alloc;
   _tmp_alloc += sz;
   return ptr;
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

static unsigned rt_count_scopes(e_node_t e)
{
   unsigned sum = 0;

   if (e_kind(e) == E_SCOPE)
      sum++;

   const int sub_scopes = e_scopes(e);
   for (int i = 0; i < sub_scopes; i++)
      sum += rt_count_scopes(e_scope(e, i));

   return sum;
}

static rt_signal_t *rt_setup_signal(e_node_t e, unsigned *total_mem)
{
   const int nnexus = e_nexuses(e);
   rt_signal_t *s = NULL;

   if (e_kind(e) == E_IMPLICIT) {
      rt_implicit_t *imp = xcalloc_flex(sizeof(rt_implicit_t),
                                        nnexus, sizeof(rt_nexus_t *));
      *total_mem += sizeof(rt_implicit_t) + nnexus * sizeof(rt_nexus_t *);

      imp->wakeable.kind = W_IMPLICIT;

      const int ntriggers = e_triggers(e);
      for (int j = 0; j < ntriggers; j++) {
         rt_nexus_t *n = &(nexuses[e_pos(e_trigger(e, j))]);
         rt_sched_event(&(n->pending), &(imp->wakeable), true);
      }

      s = &(imp->signal);
      s->flags = NET_F_IMPLICIT;
   }
   else {
      s = xcalloc_flex(sizeof(rt_signal_t), nnexus, sizeof(rt_nexus_t *));
      *total_mem += sizeof(rt_signal_t) + nnexus * sizeof(rt_nexus_t *);
   }

   s->enode   = e;
   s->width   = e_width(e);
   s->n_nexus = nnexus;

   const e_flags_t flags = e_flags(e);

   unsigned offset = 0, nmdivide = 0;
   for (int j = 0; j < s->n_nexus; j++) {
      rt_nexus_t *n = &(nexuses[e_pos(e_nexus(e, j))]);
      s->nexus[j] = n;

      unsigned o;
      for (o = 0; o < n->n_signals; o++) {
         if (e_signal(n->enode, o) == e)
            break;
      }

      if (o == n->n_signals)
         fatal_trace("signal %s missing in nexus %s", istr(e_path(e)),
                     istr(e_ident(n->enode)));

      assert(n->signals[o] == NULL);
      n->signals[o] = s;
      n->offsets[o] = offset;

      const unsigned bytes = n->width * n->size;

      if (j == 0)
         nmdivide = bytes;
      else if (nmdivide != bytes)
         nmdivide = 0;

      offset += bytes;

      if (flags & E_F_LAST_VALUE) {
         n->flags |= NET_F_LAST_VALUE;

         if (n->last_value == NULL)
            n->last_value = xcalloc_array(n->width, n->size);
      }

      if (flags & E_F_REGISTER)
         n->flags |= NET_F_REGISTER;
   }

   if (s->n_nexus == 1 || nmdivide == 1) {
      s->nmap_kind = NEXUS_MAP_DIRECT;
      profile.nmap_direct++;
   }
   else if (nmdivide == 0) {
      s->nmap_kind = NEXUS_MAP_SEARCH;
      profile.nmap_search++;
   }
   else {
      s->nmap_kind = NEXUS_MAP_DIVIDE;
      s->nmap_param = nmdivide;
      profile.nmap_divide++;
   }

   s->size = offset;

   profile.n_signals++;

   if (flags & E_F_CONTIGUOUS) {
      s->shared.resolved = s->nexus[0]->resolved;
      profile.n_contig++;
   }
   else {
      s->shared.resolved = xcalloc(s->size);
      s->flags |= NET_F_OWNS_MEM;
   }

   if (flags & E_F_LAST_VALUE) {
      s->shared.last_value = xcalloc(s->size);
      s->flags |= NET_F_LAST_VALUE;
   }

   return s;
}

static void rt_setup_scopes_recur(e_node_t e, rt_scope_t *parent,
                                  unsigned *next_scope,
                                  unsigned *total_mem)
{
   rt_scope_t *scope = NULL;

   if (e_kind(e) == E_SCOPE) {
      const int nsignals = e_signals(e);
      const int nprocs = e_procs(e);

      scope = &(scopes[(*next_scope)++]);
      scope->enode = e;
      scope->parent = parent;

      scope->n_procs = nprocs;
      scope->procs = xcalloc_array(nprocs, sizeof(rt_proc_t));
      *total_mem += nprocs * sizeof(rt_proc_t);

      scope->n_signals = nsignals;
      scope->signals = xcalloc_array(nsignals, sizeof(rt_signal_t *));
      *total_mem += nsignals * sizeof(rt_signal_t *);

      for (int i = 0; i < nprocs; i++) {
         e_node_t p = e_proc(e, i);

         rt_proc_t *r = &(scope->procs[i]);
         r->source     = p;
         r->proc_fn    = jit_find_symbol(istr(e_vcode(p)), true);
         r->tmp_stack  = NULL;
         r->tmp_alloc  = 0;
         r->scope      = scope;

         r->wakeable.kind       = W_PROC;
         r->wakeable.wakeup_gen = 0;
         r->wakeable.pending    = false;
         r->wakeable.postponed  = !!(e_flags(p) & E_F_POSTPONED);

         const int nnexus = e_nexuses(p);
         for (int j = 0; j < nnexus; j++) {
            rt_nexus_t *n = &(nexuses[e_pos(e_nexus(p, j))]);
            for (unsigned k = 0; k < n->n_sources; k++) {
               if (e_source(n->enode, k) == p)
                  n->sources[k].proc = r;
            }
         }

         const int ntriggers = e_triggers(p);
         for (int j = 0; j < ntriggers; j++) {
            rt_nexus_t *n = &(nexuses[e_pos(e_trigger(p, j))]);
            rt_sched_event(&(n->pending), &(r->wakeable), true);
         }

         profile.n_procs++;
      }

      for (int i = 0; i < nsignals; i++)
         scope->signals[i] = rt_setup_signal(e_signal(e, i), total_mem);
   }

   const int nscopes = e_scopes(e);
   for (int i = 0; i < nscopes; i++)
      rt_setup_scopes_recur(e_scope(e, i), scope, next_scope, total_mem);
}

static void rt_setup_scopes(e_node_t e)
{
   n_scopes = rt_count_scopes(e);
   scopes  = xcalloc_array(n_scopes, sizeof(rt_scope_t));

   unsigned total_mem = n_scopes * sizeof(rt_scope_t);

   unsigned next_scope = 0;
   rt_setup_scopes_recur(e, NULL, &next_scope, &total_mem);
   assert(next_scope == n_scopes);

   TRACE("allocated %u bytes for %d scopes", total_mem, n_scopes);
}

static void rt_setup_nexus(e_node_t top)
{
   assert(nexuses == NULL);

   // TODO: how to optimise this for cache locality?

   n_nexuses = e_nexuses(top);
   nexuses = xcalloc_array(n_nexuses, sizeof(rt_nexus_t));
   unsigned total_mem = n_nexuses * sizeof(rt_nexus_t);

   size_t resolved_size = 0;
   for (int i = 0; i < n_nexuses; i++) {
      rt_nexus_t *n = &(nexuses[i]);
      e_node_t e = e_nexus(top, i);
      n->enode     = e;
      n->width     = e_width(e);
      n->size      = e_size(e);
      n->n_sources = e_sources(e);
      n->n_outputs = e_outputs(e);
      n->n_signals = e_signals(e);

      if (n->n_sources > 0) {
         n->sources = xcalloc_array(n->n_sources, sizeof(rt_source_t));
         total_mem += n->n_sources * sizeof(rt_source_t);
      }

      if (n->n_outputs > 0) {
         n->outputs = xcalloc_array(n->n_outputs, sizeof(rt_source_t *));
         total_mem += n->n_sources * sizeof(rt_source_t *);
      }

      for (unsigned i = 0; i < n->n_sources; i++) {
         waveform_t *w = rt_alloc(waveform_stack);
         w->when   = 0;
         w->next   = NULL;
         w->values = rt_alloc_value(n);

         n->sources[i].waveforms = w;
         n->sources[i].output = n;
      }

      if (n->n_signals > 0) {
         n->signals = xcalloc_array(n->n_signals, sizeof(rt_signal_t *));
         n->offsets = xcalloc_array(n->n_signals, sizeof(unsigned));

         total_mem += n->n_signals * (sizeof(rt_signal_t *) + sizeof(unsigned));
      }

      resolved_size += n->width * n->size;
      profile.n_simple += n->width;
   }

   // Allocate memory for all nexuses as one contiguous blob. This is
   // important so that the common case of signals consisting only of
   // contiguous nexuses do not need a private copy of the resolved
   // value.
   uint8_t *resolved_mem = NULL;
   if (resolved_size > 0) resolved_mem = xcalloc(resolved_size);
   total_mem += resolved_size;

   highest_rank = 0;

   uint8_t *nextp = resolved_mem;
   for (int i = 0; i < n_nexuses; i++) {
      rt_nexus_t *n = &(nexuses[i]);
      if (i == 0) n->flags |= NET_F_OWNS_MEM;
      n->resolved = nextp;
      nextp += n->width * n->size;

      // Attach port outputs to sources
      for (int j = 0; j < n->n_outputs; j++) {
         e_node_t p = e_output(n->enode, j);
         assert(e_nexus(p, 0) == n->enode);
         rt_nexus_t *to = &(nexuses[e_pos(e_nexus(p, 1))]);

         int to_src_id = 0;
         for (; to_src_id < to->n_sources; to_src_id++) {
            if (e_source(to->enode, to_src_id) == p)
               break;
         }
         assert(to_src_id != to->n_sources);

         n->outputs[j] = &(to->sources[to_src_id]);
         n->outputs[j]->input = n;

         if (n->outputs[j]->output->rank <= n->rank) {
            n->outputs[j]->output->rank = n->rank + 1;
            highest_rank = MAX(n->rank + 1, highest_rank);
         }
      }
   }

   // Calculate the rank of each nexus so signals can be updated in the
   // correct order
   if (highest_rank > 0) {
      bool made_changes;
      do {
         made_changes = false;
         for (int i = 0; i < n_nexuses; i++) {
            rt_nexus_t *n = &(nexuses[i]);
            for (int j = 0; j < n->n_outputs; j++) {
               if (n->outputs[j]->output->rank <= n->rank) {
                  n->outputs[j]->output->rank = n->rank + 1;
                  highest_rank = MAX(n->rank + 1, highest_rank);
                  made_changes = true;
               }
            }
         }
      } while (made_changes);

      TRACE("highest rank is %u", highest_rank);
   }

   TRACE("allocated %u bytes for %d nexuses", total_mem, n_nexuses);
}

static void rt_setup(e_node_t top)
{
   now = 0;
   iteration = -1;
   active_proc = NULL;
   active_scope = NULL;
   force_stop = false;
   can_create_delta = true;

   RT_ASSERT(resume == NULL);

   rt_free_delta_events(delta_proc);
   rt_free_delta_events(delta_driver);

   eventq_heap = heap_new(512);
   rankn_heap = heap_new(128);

   rt_setup_nexus(top);
   rt_setup_scopes(top);

   res_memo_hash = hash_new(128, true);
}

static void rt_reset(rt_proc_t *proc)
{
  TRACE("reset process %s", istr(e_path(proc->source)));

  assert(proc->tmp_stack == NULL);

  _tmp_stack = global_tmp_stack;
  _tmp_alloc = global_tmp_alloc;

   active_proc = proc;
   active_scope = proc->scope;

   proc->privdata = (*proc->proc_fn)(NULL, proc->scope->privdata);
   global_tmp_alloc = _tmp_alloc;
}

static void rt_run(rt_proc_t *proc)
{
   TRACE("run %sprocess %s", proc->privdata ? "" :  "stateless ",
         istr(e_path(proc->source)));

   if (proc->tmp_stack != NULL) {
      TRACE("using private stack at %p %d", proc->tmp_stack, proc->tmp_alloc);
      _tmp_stack = proc->tmp_stack;
      _tmp_alloc = proc->tmp_alloc;

      // Will be updated by _private_stack if suspending in procedure otherwise
      // clear stack when process suspends
      proc->tmp_alloc = 0;
   }
   else {
      _tmp_stack = proc_tmp_stack;
      _tmp_alloc = 0;
   }

   active_proc = proc;
   active_scope = proc->scope;

   // Stateless processes have NULL privdata so pass a dummy pointer
   // value in so it can be distinguished from a reset
   void *state = proc->privdata ?: (void *)-1;

   (*proc->proc_fn)(state, proc->scope->privdata);
}

static void *rt_call_module_reset(ident_t name, void *arg)
{
   char *buf LOCAL = xasprintf("%s_reset", istr(name));

   _tmp_stack = global_tmp_stack;
   _tmp_alloc = global_tmp_alloc;

   void *result = NULL;
   void *(*reset_fn)(void *) = jit_find_symbol(buf, false);
   if (reset_fn != NULL)
      result = (*reset_fn)(arg);

   global_tmp_alloc = _tmp_alloc;
   return result;
}

static inline void *rt_resolution_buffer(size_t required)
{
   static void *rbuf = NULL;
   static size_t size = 0;

   if (likely(size >= required))
      return rbuf;

   size = MAX(required, 16);
   return (rbuf = xrealloc(rbuf, size));
}

static void *rt_resolve_nexus_slow(rt_nexus_t *nexus)
{
   int nonnull = 0;
   for (unsigned i = 0; i < nexus->n_sources; i++) {
      if (nexus->sources[i].waveforms->values != NULL)
         nonnull++;
   }

   if (nonnull == 0 && (nexus->flags & NET_F_REGISTER)) {
      return nexus->resolved;
   }
   else if (nexus->resolution->flags & R_COMPOSITE) {
      // Call resolution function of composite type

      rt_signal_t *s0 = nexus->signals[0];
      uint8_t *inputs LOCAL = xmalloc(nonnull * s0->size);
      void *resolved = rt_resolution_buffer(s0->size);

      size_t offset = 0, result_offset = 0;
      for (unsigned i = 0; i < s0->n_nexus; i++) {
         rt_nexus_t *n = s0->nexus[i];

         unsigned o = 0;
         for (unsigned j = 0; j < nexus->n_sources; j++) {
            const void *src = NULL;
            if (n->sources[j].waveforms->values == NULL)
               continue;
            else if (n == nexus) {
               result_offset = offset;
               src = n->sources[j].waveforms->values->data;
            }
            else
               src = n->resolved;

            memcpy(inputs + offset + (o * s0->size), src, n->size * n->width);
            o++;
         }
         assert(o == nonnull);

         offset += n->size * n->width;
      }

      const int32_t left = nexus->resolution->ileft;
      ffi_uarray_t u = { inputs, { { left, nonnull } } };
      ffi_call(&(nexus->resolution->closure), &u, sizeof(u),
               resolved, s0->size);

      return resolved + result_offset;
   }
   else {
      void *resolved = rt_resolution_buffer(nexus->width * nexus->size);

      for (int j = 0; j < nexus->width; j++) {
#define CALL_RESOLUTION_FN(type) do {                                   \
            type vals[nonnull];                                         \
            unsigned o = 0;                                             \
            for (int i = 0; i < nexus->n_sources; i++) {                \
               const value_t *v = nexus->sources[i].waveforms->values;  \
               if (v != NULL)                                           \
                  vals[o++] = ((const type *)v->data)[j];               \
            }                                                           \
            type *r = (type *)resolved;                                 \
            const int32_t left = nexus->resolution->ileft;              \
            ffi_uarray_t u = { vals, { { left, nonnull } } };           \
            ffi_call(&(nexus->resolution->closure), &u, sizeof(u),      \
                     &(r[j]), sizeof(r[j]));                            \
         } while (0)

         FOR_ALL_SIZES(nexus->size, CALL_RESOLUTION_FN);
      }

      return resolved;
   }
}

static void *rt_resolve_nexus_fast(rt_nexus_t *nexus)
{
   if (unlikely(nexus->flags & NET_F_FORCED)) {
      return nexus->forcing->data;
   }
   else if (unlikely(nexus->flags & NET_F_DISCONNECTED)) {
      // Some drivers may have null transactions
      return rt_resolve_nexus_slow(nexus);
   }
   else if (nexus->resolution == NULL && nexus->n_sources == 0) {
      // Always maintains initial driver value
      return nexus->resolved;
   }
   else if (nexus->resolution == NULL) {
      return nexus->sources[0].waveforms->values->data;
   }
   else if ((nexus->resolution->flags & R_IDENT) && (nexus->n_sources == 1)) {
      // Resolution function behaves like identity for a single driver
      return nexus->sources[0].waveforms->values->data;
   }
   else if ((nexus->resolution->flags & R_MEMO) && (nexus->n_sources == 1)) {
      // Resolution function has been memoised so do a table lookup

      void *resolved = rt_resolution_buffer(nexus->width * nexus->size);

      for (int j = 0; j < nexus->width; j++) {
         const int index = nexus->sources[0].waveforms->values->data[j];
         const int8_t r = nexus->resolution->tab1[index];
         ((int8_t *)resolved)[j] = r;
      }

      return resolved;
   }
   else if ((nexus->resolution->flags & R_MEMO) && (nexus->n_sources == 2)) {
      // Resolution function has been memoised so do a table lookup

      void *resolved = rt_resolution_buffer(nexus->width * nexus->size);

      const char *p0 = nexus->sources[0].waveforms->values->data;
      const char *p1 = nexus->sources[1].waveforms->values->data;

      for (int j = 0; j < nexus->width; j++) {
         const int driving[2] = { p0[j], p1[j] };
         const int8_t r = nexus->resolution->tab2[driving[0]][driving[1]];
         ((int8_t *)resolved)[j] = r;
      }

      return resolved;
   }
   else {
      // Must actually call resolution function in general case
      return rt_resolve_nexus_slow(nexus);
   }
}

static void rt_propagate_nexus(rt_nexus_t *nexus, const void *resolved)
{
   const size_t valuesz = nexus->size * nexus->width;

   // LAST_VALUE is the same as the initial value when there have
   // been no events on the signal otherwise only update it when
   // there is an event
   if (nexus->flags & NET_F_LAST_VALUE)
      memcpy(nexus->last_value, nexus->resolved, valuesz);
   if (nexus->resolved != resolved)   // Can occur during startup
      memcpy(nexus->resolved, resolved, valuesz);

   for (unsigned i = 0; i < nexus->n_signals; i++) {
      rt_signal_t *s = nexus->signals[i];
      if (s->flags & NET_F_LAST_VALUE)
         memcpy(s->shared.last_value + nexus->offsets[i],
                nexus->last_value, valuesz);
      if (s->flags & NET_F_OWNS_MEM)
         memcpy(s->shared.resolved + nexus->offsets[i],
                nexus->resolved, valuesz);
   }
}

static void rt_update_inputs(rt_nexus_t *nexus)
{
   for (unsigned i = 0; i < nexus->n_sources; i++) {
      rt_source_t *s = &(nexus->sources[i]);
      if (s->proc != NULL)
         continue;
      else if (likely(s->conv_func == NULL)) {
         const size_t valuesz = s->input->size * s->input->width;
         memcpy(s->waveforms->values->data, s->input->resolved, valuesz);
      }
      else {
         rt_signal_t *i0 = s->input->signals[0];
         rt_signal_t *o0 = s->output->signals[0];

         const size_t outsz = s->output->size * s->output->width;

         TRACE("call conversion function %p insz=%d outsz=%zu",
               s->conv_func->fn, i0->size, outsz);

         if (o0->size != outsz) {
            // This corner case occurs with output conversions from
            // aggregate to scalar types

            uint8_t *buf LOCAL = xmalloc(o0->size);
            ffi_call(s->conv_func, i0->shared.resolved, i0->size,
                     buf, o0->size);

            unsigned o = 0;
            for (unsigned i = 0; i < o0->n_nexus; i++) {
               if (o0->nexus[i] == nexus)
                  break;
               else
                  o += o0->nexus[i]->width * o0->nexus[i]->size;
            }
            assert(o + outsz <= o0->size);

            memcpy(s->waveforms->values->data, buf + o, outsz);
         }
         else {
            ffi_call(s->conv_func, i0->shared.resolved, i0->size,
                     s->waveforms->values->data, outsz);
         }
      }
   }
}

static void rt_reset_scopes(e_node_t top)
{
   for (unsigned i = 0; i < n_scopes; i++) {
      rt_scope_t *s = &(scopes[i]);
      TRACE("reset scope %s", istr(e_path(s->enode)));

      void *privdata = s->parent ? s->parent->privdata : NULL;
      active_scope = s;

      s->privdata = rt_call_module_reset(e_vcode(s->enode), privdata);

   }
   active_scope = NULL;
}

static void rt_driver_initial(rt_nexus_t *nexus)
{
   const size_t valuesz = nexus->size * nexus->width;

   // Assign the initial value of the drivers
   for (unsigned i = 0; i < nexus->n_sources; i++) {
      rt_source_t *s = &(nexus->sources[i]);
      if (s->proc != NULL)  // Driver not port source
         memcpy(s->waveforms->values->data, nexus->resolved, valuesz);
   }

   rt_update_inputs(nexus);

   void *resolved;
   if (nexus->n_sources > 0)
      resolved = rt_resolve_nexus_fast(nexus);
   else
      resolved = nexus->resolved;

   nexus->event_delta = nexus->active_delta = -1;
   nexus->last_event = nexus->last_active = INT64_MAX;    // TIME'HIGH

   TRACE("%s initial value %s", istr(e_ident(nexus->enode)),
         fmt_nexus(nexus, resolved));

   rt_propagate_nexus(nexus, resolved);
}

static void rt_initial(e_node_t top)
{
   // Initialisation is described in LRM 93 section 12.6.4

   rt_reset_scopes(top);

   for (unsigned i = 0; i < n_scopes; i++) {
      for (unsigned j = 0; j < scopes[i].n_procs; j++)
         rt_reset(&(scopes[i].procs[j]));
   }

   TRACE("calculate initial driver values");

   init_side_effect = SIDE_EFFECT_ALLOW;

   for (int rank = 0; rank <= highest_rank; rank++) {
      for (unsigned i = 0; i < n_nexuses; i++) {
         if (nexuses[i].rank == rank)
            rt_driver_initial(&(nexuses[i]));
      }
   }

   TRACE("used %d bytes of global temporary stack", global_tmp_alloc);
}

static void rt_trace_wakeup(rt_wakeable_t *obj)
{
   if (unlikely(trace_on)) {
      switch (obj->kind) {
      case W_PROC:
         TRACE("wakeup %sprocess %s", obj->postponed ? "postponed " : "",
               istr(e_path(container_of(obj, rt_proc_t, wakeable)->source)));
         break;

      case W_WATCH:
         TRACE("wakeup %svalue change callback %p",
               obj->postponed ? "postponed " : "",
               container_of(obj, rt_watch_t, wakeable)->fn);
         break;

      case W_IMPLICIT:
         TRACE("wakeup implicit signal %s",
               istr(e_path(container_of(obj, rt_implicit_t, wakeable)
                           ->signal.enode)));
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
                            uint64_t reject, value_t *values)
{
   if (unlikely(reject > after))
      fatal("signal %s pulse reject limit %s is greater than "
            "delay %s", istr(e_path(nexus->signals[0]->enode)),
            fmt_time(reject), fmt_time(after));

   int driver = 0;
   if (unlikely(nexus->n_sources != 1)) {
      // Try to find this process in the list of existing drivers
      for (driver = 0; driver < nexus->n_sources; driver++) {
         if (likely(nexus->sources[driver].proc == active_proc))
            break;
      }

      RT_ASSERT(driver != nexus->n_sources);
   }

   rt_source_t *d = &(nexus->sources[driver]);

   const size_t valuesz = nexus->size * nexus->width;

   waveform_t *w = rt_alloc(waveform_stack);
   w->when   = now + after;
   w->next   = NULL;
   w->values = values;

   waveform_t *last = d->waveforms;
   waveform_t *it   = last->next;
   while ((it != NULL) && (it->when < w->when)) {
      // If the current transaction is within the pulse rejection interval
      // and the value is different to that of the new transaction then
      // delete the current transaction
      if ((it->when >= w->when - reject)
          && (memcmp(it->values->data, w->values->data, valuesz) != 0)) {
         waveform_t *next = it->next;
         last->next = next;
         rt_free_value(nexus, it->values);
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
      rt_free_value(nexus, it->values);

      if (it->when == w->when)
         already_scheduled = true;

      waveform_t *next = it->next;
      rt_free(waveform_stack, it);
      it = next;
   }

   if (!already_scheduled)
      deltaq_insert_driver(after, nexus, d);
}

static void rt_notify_event(rt_nexus_t *nexus)
{
   sens_list_t *it = NULL, *next = NULL;

   nexus->last_event = nexus->last_active = now;
   nexus->event_delta = nexus->active_delta = iteration;

   // First wakeup everything on the nexus specific pending list
   for (it = nexus->pending; it != NULL; it = next) {
      next = it->next;
      rt_wakeup(it);
      nexus->pending = next;
   }

   for (unsigned i = 0; i < nexus->n_sources; i++) {
      rt_source_t *o = &(nexus->sources[i]);
      if (o->proc == NULL)
         rt_notify_event(o->input);
   }
}

static void rt_notify_active(rt_nexus_t *nexus)
{
   nexus->last_active = now;
   nexus->active_delta = iteration;

   for (unsigned i = 0; i < nexus->n_sources; i++) {
      rt_source_t *o = &(nexus->sources[i]);
      if (o->proc == NULL)
         rt_notify_active(o->input);
   }
}

static void rt_update_nexus(rt_nexus_t *nexus)
{
   void *resolved = rt_resolve_nexus_fast(nexus);
   const size_t valuesz = nexus->size * nexus->width;

   nexus->last_active = now;
   nexus->active_delta = iteration;

   RT_ASSERT(nexus->flags & NET_F_PENDING);
   nexus->flags &= ~NET_F_PENDING;

   TRACE("update nexus %s resolved=%s", istr(e_ident(nexus->enode)),
         fmt_nexus(nexus, resolved));

   if (memcmp(nexus->resolved, resolved, valuesz) != 0) {
      rt_propagate_nexus(nexus, resolved);
      rt_notify_event(nexus);
   }
   else
      rt_notify_active(nexus);
}

static void rt_push_active_nexus(rt_nexus_t *nexus)
{
   if (nexus->flags & NET_F_PENDING)
      return;   // Already scheduled

   nexus->flags |= NET_F_PENDING;

   if (nexus->rank == 0 && nexus->n_sources == 1) {
      // This nexus does not depend on the values of any inputs or other
      // drivers so we can eagerly update its value now
      rt_update_nexus(nexus);
   }
   else
      heap_insert(rankn_heap, nexus->rank, nexus);

   for (unsigned i = 0; i < nexus->n_outputs; i++) {
      rt_source_t *o = nexus->outputs[i];
      TRACE("active nexus %s sources nexus %s", istr(e_ident(nexus->enode)),
            istr(e_ident(o->output->enode)));
      RT_ASSERT(nexus->rank < o->output->rank);
      rt_push_active_nexus(o->output);
   }
}

static void rt_update_driver(rt_nexus_t *nexus, rt_source_t *source)
{
   if (likely(source != NULL)) {
      waveform_t *w_now  = source->waveforms;
      waveform_t *w_next = w_now->next;

      if (likely((w_next != NULL) && (w_next->when == now))) {
         source->waveforms = w_next;
         rt_free_value(nexus, w_now->values);
         rt_free(waveform_stack, w_now);
         rt_push_active_nexus(nexus);
      }
      else
         RT_ASSERT(w_now != NULL);
   }
   else if (nexus->flags & NET_F_FORCED)
      rt_push_active_nexus(nexus);
}

static void rt_update_implicit_signal(rt_implicit_t *imp)
{
   int8_t r;
   ffi_call(imp->closure, NULL, 0, &r, sizeof(r));

   TRACE("implicit signal %s guard expression %d",
         istr(e_path(imp->signal.enode)), r);

   RT_ASSERT(imp->signal.n_nexus == 1);
   rt_nexus_t *n0 = imp->signal.nexus[0];

   // Implicit signals have no sources
   RT_ASSERT(!(n0->flags & NET_F_PENDING));

   if (*(int8_t *)n0->resolved != r) {
      rt_propagate_nexus(n0, &r);
      rt_notify_event(n0);
   }
   else
      rt_notify_active(n0);
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
   text_buf_t *buf = tb_new();
   tb_printf(buf, "Iteration limit of %d delta cycles reached. "
             "The following processes are active:\n",
             opt_get_int("stop-delta"));

   for (sens_list_t *it = resume; it != NULL; it = it->next) {
      if (it->wake->kind == W_PROC) {
         rt_proc_t *proc = container_of(it->wake, rt_proc_t, wakeable);
         const loc_t *l = e_loc(proc->source);
         tb_printf(buf, "  %-30s %s line %d\n", istr(e_path(proc->source)),
                   loc_file_str(l), l->first_line);
      }
   }

   tb_printf(buf, "You can increase this limit with --stop-delta");

   fatal("%s", tb_get(buf));
}

static void rt_resume(sens_list_t **list)
{
   sens_list_t *it = *list;
   while (it != NULL) {
      if (it->wake->pending) {
         switch (it->wake->kind) {
         case W_PROC:
            {
               rt_proc_t *proc = container_of(it->wake, rt_proc_t, wakeable);
               rt_run(proc);
            }
            break;
         case W_WATCH:
            {
               rt_watch_t *w = container_of(it->wake, rt_watch_t, wakeable);
               (*w->fn)(now, w->signal, w, w->user_data);
            }
            break;
         case W_IMPLICIT:
            {
               rt_implicit_t *imp =
                  container_of(it->wake, rt_implicit_t, wakeable);
               rt_update_implicit_signal(imp);
            }
         }
         it->wake->pending = false;
      }

      sens_list_t *next = it->next;

      if (it->reenq == NULL)
         rt_free(sens_list_stack, it);
      else {
         it->next = *(it->reenq);
         *(it->reenq) = it;
      }

      it = next;
   }

   *list = NULL;
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

   while (heap_size(rankn_heap) > 0) {
      rt_nexus_t *n = heap_extract_min(rankn_heap);
      rt_update_inputs(n);
      rt_update_nexus(n);
   }

   rt_resume(&implicit);

   while ((event = rt_pop_run_queue(&procq))) {
      rt_run(event->proc.proc);
      rt_free(event_stack, event);
   }

   if (unlikely((stop_delta > 0) && (iteration == stop_delta)))
      rt_iteration_limit();

   // Run all non-postponed event callbacks
   rt_resume(&resume_watch);

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
}

static void rt_cleanup_nexus(rt_nexus_t *n)
{
   if (n->flags & NET_F_OWNS_MEM)
      free(n->resolved);
   if (n->flags & NET_F_LAST_VALUE)
      free(n->last_value);

   free(n->forcing);

   for (int j = 0; j < n->n_sources; j++) {
      while (n->sources[j].waveforms != NULL) {
         waveform_t *next = n->sources[j].waveforms->next;
         if (n->sources[j].waveforms->values)
            rt_free_value(n, n->sources[j].waveforms->values);
         rt_free(waveform_stack, n->sources[j].waveforms);
         n->sources[j].waveforms = next;
      }

      if (n->sources[j].conv_func != NULL)
         ffi_unref_closure(n->sources[j].conv_func);
   }
   free(n->sources);

   free(n->outputs);
   free(n->signals);
   free(n->offsets);

   while (n->free_values != NULL) {
      value_t *next = n->free_values->next;
      free(n->free_values);
      n->free_values = next;
   }

   while (n->pending != NULL) {
      sens_list_t *next = n->pending->next;
      rt_free(sens_list_stack, n->pending);
      n->pending = next;
   }
}

static void rt_cleanup_signal(rt_signal_t *s)
{
   if (s->flags & NET_F_OWNS_MEM)
      free(s->shared.resolved);

   if (s->flags & NET_F_LAST_VALUE)
      free(s->shared.last_value);

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
   for (unsigned i = 0; i < scope->n_procs; i++)
      free(scope->procs[i].privdata);

   for (unsigned i = 0; i < scope->n_signals; i++)
      rt_cleanup_signal(scope->signals[i]);

   free(scope->privdata);
   free(scope->procs);
   free(scope->signals);
}

static void rt_cleanup(e_node_t top)
{
   RT_ASSERT(resume == NULL);

   while (heap_size(eventq_heap) > 0)
      rt_free(event_stack, heap_extract_min(eventq_heap));

   rt_free_delta_events(delta_proc);
   rt_free_delta_events(delta_driver);

   heap_free(eventq_heap);
   eventq_heap = NULL;

   heap_free(rankn_heap);
   rankn_heap = NULL;

   for (unsigned i = 0; i < n_nexuses; i++)
      rt_cleanup_nexus(&(nexuses[i]));

   free(nexuses);
   nexuses = NULL;

   for (unsigned i = 0; i < n_scopes; i++)
      rt_cleanup_scope(&(scopes[i]));

   free(scopes);
   scopes = NULL;

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

   hash_free(res_memo_hash);
}

static bool rt_stop_now(uint64_t stop_time)
{
   if ((delta_driver != NULL) || (delta_proc != NULL))
      return false;
   else if (heap_size(eventq_heap) == 0)
      return true;
   else if (force_stop)
      return true;
   else if (stop_time == UINT64_MAX)
      return false;
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
      tb_printf(tb, "Nexuses: %-5d      Simple signals: %d (1:%.1f)\n",
                n_nexuses, profile.n_simple,
                (double)profile.n_simple / n_nexuses);
      tb_printf(tb, "Mapping:  direct:%d search:%d divide:%d\n",
                profile.nmap_direct, profile.nmap_search, profile.nmap_divide);
      tb_printf(tb, "Processes: %-5d    Scopes: %d\n",
                profile.n_procs, n_scopes);
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
      rt_msg(NULL, fatal,
             "interrupted in process %s at %s+%d",
             istr(e_path(active_proc->source)), fmt_time(now), iteration);
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

void rt_start_of_tool(tree_t top, e_node_t e)
{
   jit_init(top, e);

#if RT_DEBUG
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

   trace_on = opt_get_int("rt_trace_en");
   profiling = opt_get_int("rt_profile");

   if (profiling) {
      memset(&profile, '\0', sizeof(profile));
      profile.runq_min = ~0;
   }

   event_stack     = rt_alloc_stack_new(sizeof(event_t), "event");
   waveform_stack  = rt_alloc_stack_new(sizeof(waveform_t), "waveform");
   sens_list_stack = rt_alloc_stack_new(sizeof(sens_list_t), "sens_list");
   watch_stack     = rt_alloc_stack_new(sizeof(rt_watch_t), "watch");
   callback_stack  = rt_alloc_stack_new(sizeof(callback_t), "callback");

   global_tmp_stack = mmap_guarded(GLOBAL_TMP_STACK_SZ, "global temp stack");
   proc_tmp_stack   = mmap_guarded(PROC_TMP_STACK_SZ, "process temp stack");

   global_tmp_alloc = 0;

   rt_reset_coverage(top);

   nvc_rusage(&ready_rusage);
}

void rt_end_of_tool(tree_t top, e_node_t e)
{
   rt_cleanup(e);
   rt_emit_coverage(top);

   jit_shutdown();

   if (opt_get_int("rt-stats") || profiling)
      rt_stats_print();
}

void rt_run_sim(uint64_t stop_time)
{
   const int stop_delta = opt_get_int("stop-delta");

   wave_restart();

   rt_global_event(RT_START_OF_SIMULATION);
   while (!rt_stop_now(stop_time))
      rt_cycle(stop_delta);
   rt_global_event(RT_END_OF_SIMULATION);
}

void rt_restart(e_node_t top)
{
   rt_setup(top);
   rt_initial(top);
   aborted = false;
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

      for (unsigned i = 0; i < w->signal->n_nexus; i++) {
         rt_nexus_t *n = w->signal->nexus[i];
         rt_sched_event(&(n->pending), &(w->wakeable), true);
      }

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
   for (unsigned i = 0; i < s->n_nexus; i++) {
      rt_nexus_t *n = s->nexus[i];

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
   int index = 0;
   while (offset > 0)
      offset -= s->nexus[index++]->width;
   assert(offset == 0);

   for (; index < s->n_nexus && offset < max; index++) {
      rt_nexus_t *n = s->nexus[index];

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
   int index = 0;
   const uint8_t *ptr = s->shared.resolved;
   while (offset > 0) {
      rt_nexus_t *n = s->nexus[index++];
      ptr += n->width * n->size;
      offset -= n->width;
   }
   assert(offset == 0);

   return ptr;
}

rt_signal_t *rt_find_signal(e_node_t esignal)
{
   assert(e_kind(esignal) == E_SIGNAL);

   for (unsigned i = 0; i < n_scopes; i++) {
      for (unsigned j = 0; j < scopes[i].n_signals; j++) {
         if (scopes[i].signals[j]->enode == esignal)
            return scopes[i].signals[j];
      }
   }

   return NULL;
}

bool rt_force_signal(rt_signal_t *s, const uint64_t *buf, size_t count,
                     bool propagate)
{
   TRACE("force signal %s to %"PRIu64"%s propagate=%d", istr(e_path(s->enode)),
         buf[0], count > 1 ? "..." : "", propagate);

   RT_ASSERT(!propagate || can_create_delta);

   int offset = 0, index = 0;
   while (count > 0) {
      RT_ASSERT(index < s->n_nexus);
      rt_nexus_t *n = s->nexus[index++];

      n->flags |= NET_F_FORCED;

      if (n->forcing == NULL)
         n->forcing = rt_alloc_value(n);

#define SIGNAL_FORCE_EXPAND_U64(type) do {                              \
         type *dp = (type *)n->forcing->data;                           \
         for (int i = 0; (i < n->width) && (offset + i < count); i++)   \
            dp[i] = buf[offset + i];                                    \
      } while (0)

      FOR_ALL_SIZES(n->size, SIGNAL_FORCE_EXPAND_U64);

      if (propagate)   // XXX: this is wrong, sensitive process can run twice
                       //      see vhpi1
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

void rt_set_exit_severity(rt_severity_t severity)
{
   exit_severity = severity;
}
