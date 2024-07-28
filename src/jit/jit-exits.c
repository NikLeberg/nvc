//
//  Copyright (C) 2022-2024  Nick Gasson
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
#include "diag.h"
#include "jit/jit-exits.h"
#include "jit/jit-ffi.h"
#include "jit/jit-priv.h"
#include "jit/jit.h"
#include "lib.h"
#include "object.h"
#include "psl/psl-node.h"
#include "rt/assert.h"
#include "rt/mspace.h"
#include "rt/rt.h"
#include "rt/structs.h"
#include "type.h"

#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <float.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void x_index_fail(int64_t value, int64_t left, int64_t right, int8_t dir,
                  tree_t where, tree_t hint)
{
   type_t type = tree_type(hint);

   LOCAL_TEXT_BUF tb = tb_new();
   tb_cat(tb, "index ");
   to_string(tb, type, value);
   tb_printf(tb, " outside of %s range ", type_pp(type));
   to_string(tb, type, left);
   tb_cat(tb, dir == RANGE_TO ? " to " : " downto ");
   to_string(tb, type, right);

   jit_msg(tree_loc(where), DIAG_FATAL, "%s", tb_get(tb));
}

void x_length_fail(int64_t left, int64_t right, int32_t dim, tree_t where)
{
   const tree_kind_t kind = tree_kind(where);

   LOCAL_TEXT_BUF tb = tb_new();
   switch (kind) {
   case T_PORT_DECL:
   case T_GENERIC_DECL:
   case T_PARAM_DECL:
   case T_PARAM:
      tb_cat(tb, "actual");
      break;
   case T_CASE:
   case T_MATCH_CASE:
      tb_cat(tb, "expression");
      break;
   case T_ASSOC:
      tb_cat(tb, "choice");
      break;
   case T_AGGREGATE:
      tb_cat(tb, "aggregate");
      break;
   case T_EXTERNAL_NAME:
      {
         tree_t last = tree_part(where, tree_parts(where) - 1);
         tb_printf(tb, "object %s", istr(tree_ident(last)));
      }
      break;
   default:
      tb_cat(tb, "value");
      break;
   }

   tb_printf(tb, " length %"PRIi64, right);
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
   case T_CONST_DECL:
      tb_printf(tb, "constant %s", istr(tree_ident(where)));
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
   case T_PARAM:
      tb_cat(tb, "formal");
      break;
   case T_EXTERNAL_NAME:
      tb_cat(tb, "external name subtype indication");
      break;
   case T_TYPE_CONV:
   case T_ATTR_REF:
   case T_AGGREGATE:
      tb_printf(tb, "subtype %s", type_pp(tree_type(where)));
      break;
   default:
      tb_cat(tb, "target");
      break;
   }

   tb_printf(tb, " length %"PRIi64, left);

   jit_msg(tree_loc(where), DIAG_FATAL, "%s", tb_get(tb));
}

void x_range_fail(int64_t value, int64_t left, int64_t right, int8_t dir,
                  tree_t where, tree_t hint)
{
   // Hint tree may be an array type conversion
   type_t type = type_elem_recur(tree_type(hint));

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

   jit_msg(tree_loc(where), DIAG_FATAL, "%s", tb_get(tb));
}

void x_exponent_fail(int64_t value, tree_t where)
{
   jit_msg(tree_loc(where), DIAG_FATAL, "negative exponent %"PRIi64
           " only allowed for floating-point types", value);
}

void x_overflow(int64_t lhs, int64_t rhs, tree_t where)
{
   LOCAL_TEXT_BUF tb = tb_new();
   if (tree_kind(where) == T_FCALL) {
      switch (tree_subkind(tree_ref(where))) {
      case S_ADD:
         tb_printf(tb, "%"PRIi64" + %"PRIi64, lhs, rhs);
         break;
      case S_MUL:
         tb_printf(tb, "%"PRIi64" * %"PRIi64, lhs, rhs);
         break;
      case S_SUB:
         tb_printf(tb, "%"PRIi64" - %"PRIi64, lhs, rhs);
         break;
      case S_NEGATE:
         tb_printf(tb, "-(%"PRIi64")", lhs);
         break;
      case S_EXP:
         tb_printf(tb, "%"PRIi64" ** %"PRIi64, lhs, rhs);
         break;
      }
   }

   jit_msg(tree_loc(where), DIAG_FATAL,
           "result of %s cannot be represented as %s",
           tb_get(tb), type_pp(tree_type(where)));
}

void x_null_deref(tree_t where)
{
   jit_msg(tree_loc(where), DIAG_FATAL, "null access dereference");
}

void x_div_zero(tree_t where)
{
   jit_msg(tree_loc(where), DIAG_FATAL, "division by zero");
}

void x_elab_order_fail(tree_t where)
{
   assert(tree_kind(where) == T_EXTERNAL_NAME);

   jit_msg(tree_loc(where), DIAG_FATAL, "%s %s has not yet been elaborated",
           class_str(tree_class(where)), istr(tree_ident(tree_ref(where))));
}

void x_unreachable(tree_t where)
{
   if (where != NULL && tree_kind(where) == T_FUNC_BODY)
      jit_msg(tree_loc(where), DIAG_FATAL, "function %s did not return a value",
              istr(tree_ident(where)));
   else
      jit_msg(NULL, DIAG_FATAL, "executed unreachable instruction");
}

void x_func_wait(void)
{
   jit_stack_trace_t *trace = jit_stack_trace();
   tree_t inner = tree_from_object(trace->frames[0].object);
   free(trace);

   const char *what;
   switch (tree_kind(inner)) {
   case T_PROC_BODY: what = "call to protected type method"; break;
   case T_PROCESS: what = "process with sensitivity list"; break;
   default: what = "function call";
   }

   jit_msg(NULL, DIAG_FATAL, "cannot wait inside %s", what);
}

void x_instance_name(attr_kind_t kind, text_buf_t *tb)
{
   assert(kind == ATTR_INSTANCE_NAME || kind == ATTR_PATH_NAME);

   jit_stack_trace_t *trace LOCAL = jit_stack_trace();
   for (int i = 0; i < trace->count; i++) {
      tree_t where = tree_from_object(trace->frames[i].object);
      if (where == NULL)
         continue;

      switch (tree_kind(where)) {
      case T_BLOCK:
         {
            tree_t hier = tree_decl(where, 0);
            assert(tree_kind(hier) == T_HIER);

            ident_t inst = tree_ident(hier);

            if (kind == ATTR_PATH_NAME)
               instance_name_to_path(tb, istr(inst));
            else
               tb_istr(tb, inst);
         }
         return;

      case T_PACKAGE:
      case T_PACK_BODY:
      case T_PACK_INST:
         {
            tb_append(tb, ':');
            tb_istr(tb, tree_ident(primary_unit_of(where)));
            tb_replace(tb, '.', ':');
            tb_downcase(tb);
         }
         return;

      default:
         break;
      }
   }

   fatal_trace("cannot get instance name");
}

////////////////////////////////////////////////////////////////////////////////
// Entry point from interpreter or JIT compiled code

DLLEXPORT
void __nvc_sched_waveform(jit_anchor_t *anchor, jit_scalar_t *args,
                          tlab_t *tlab)
{
   jit_thread_local_t *thread = jit_attach_thread(anchor);

   sig_shared_t *shared = args[0].pointer;
   int32_t       offset = args[1].integer;
   int32_t       count  = args[2].integer;
   jit_scalar_t  value  = { .integer = args[3].integer };
   int64_t       after  = args[4].integer;
   int64_t       reject = args[5].integer;
   bool          scalar = args[6].integer;

   if (scalar)
      x_sched_waveform_s(shared, offset, value.integer, after, reject);
   else
      x_sched_waveform(shared, offset, value.pointer, count,
                       after, reject);

   thread->anchor = NULL;
}

DLLEXPORT
void __nvc_test_event(jit_anchor_t *anchor, jit_scalar_t *args, tlab_t *tlab)
{
   jit_thread_local_t *thread = jit_attach_thread(anchor);

   sig_shared_t *shared = args[0].pointer;
   int32_t       offset = args[1].integer;
   int32_t       count  = args[2].integer;

   args[0].integer = x_test_net_event(shared, offset, count);

   thread->anchor = NULL;
}

DLLEXPORT
void __nvc_last_event(jit_anchor_t *anchor, jit_scalar_t *args, tlab_t *tlab)
{
   jit_thread_local_t *thread = jit_attach_thread(anchor);

   sig_shared_t *shared = args[0].pointer;
   uint32_t      offset = args[1].integer;
   uint32_t      count  = args[2].integer;

   args[0].integer = x_last_event(shared, offset, count);

   thread->anchor = NULL;
}

DLLEXPORT
void __nvc_sched_process(jit_anchor_t *anchor, jit_scalar_t *args, tlab_t *tlab)
{
   jit_thread_local_t *thread = jit_attach_thread(anchor);

   int64_t after = args[0].integer;

   x_sched_process(after);

   thread->anchor = NULL;
}

DLLEXPORT
void __nvc_do_exit(jit_exit_t which, jit_anchor_t *anchor, jit_scalar_t *args,
                   tlab_t *tlab)
{
   jit_thread_local_t *thread = jit_attach_thread(anchor);

   switch (which) {
   case JIT_EXIT_ASSERT_FAIL:
      {
         uint8_t  *msg        = args[0].pointer;
         int32_t   len        = args[1].integer;
         int32_t   severity   = args[2].integer;
         int64_t   hint_left  = args[3].integer;
         int64_t   hint_right = args[4].integer;
         int8_t    hint_valid = args[5].integer;
         object_t *where      = args[6].pointer;

         x_assert_fail(msg, len, severity, hint_left, hint_right,
                       hint_valid, where);
      }
      break;

   case JIT_EXIT_REPORT:
      {
         uint8_t  *msg      = args[0].pointer;
         int32_t   len      = args[1].integer;
         int32_t   severity = args[2].integer;
         object_t *where    = args[3].pointer;

         x_report(msg, len, severity, where);
      }
      break;

   case JIT_EXIT_INIT_SIGNAL:
      {
         int64_t      count  = args[0].integer;
         int32_t      size   = args[1].integer;
         jit_scalar_t value  = { .integer = args[2].integer };
         int32_t      flags  = args[3].integer;
         tree_t       where  = args[4].pointer;
         int32_t      offset = args[5].integer;
         bool         scalar = args[6].integer;

         sig_shared_t *ss;
         if (!jit_has_runtime(thread->jit))
            ss = NULL;
         else
            ss = x_init_signal(count, size, value, scalar,
                               flags, where, offset);

         args[0].pointer = ss;
      }
      break;

   case JIT_EXIT_IMPLICIT_SIGNAL:
      {
         int32_t       count   = args[0].integer;
         int32_t       size    = args[1].integer;
         tree_t        where   = args[2].pointer;
         int32_t       kind    = args[3].integer;
         jit_handle_t  handle  = args[4].integer;
         void         *context = args[5].pointer;

         sig_shared_t *ss;
         if (!jit_has_runtime(thread->jit))
            ss = NULL;   // Called during constant folding
         else {
            ffi_closure_t closure = { handle, context };
            ss = x_implicit_signal(count, size, where, kind, &closure);
         }

         args[0].pointer = ss;
      }
      break;

   case JIT_EXIT_RESOLVE_SIGNAL:
      {
         if (!jit_has_runtime(thread->jit))
            return;   // Called during constant folding

         sig_shared_t *shared  = args[0].pointer;
         jit_handle_t  handle  = args[1].integer;
         void         *context = args[2].pointer;
         int32_t       ileft   = args[3].integer;
         int32_t       nlits   = args[4].integer;
         int32_t       flags   = args[5].integer;

         x_resolve_signal(shared, handle, context, ileft, nlits, flags);
      }
      break;

   case JIT_EXIT_DRIVE_SIGNAL:
      {
         if (!jit_has_runtime(thread->jit))
            return;   // Called during constant folding

         sig_shared_t *ss     = args[0].pointer;
         int32_t       offset = args[1].integer;
         int32_t       count  = args[2].integer;

         x_drive_signal(ss, offset, count);
      }
      break;

   case JIT_EXIT_TRANSFER_SIGNAL:
      {
         if (!jit_has_runtime(thread->jit))
            return;   // Called during constant folding

         sig_shared_t *target    = args[0].pointer;
         int32_t       toffset   = args[1].integer;
         sig_shared_t *source    = args[2].pointer;
         int32_t       soffset   = args[3].integer;
         int32_t       count     = args[4].integer;
         int64_t       after     = args[5].integer;
         int64_t       reject    = args[6].integer;

         x_transfer_signal(target, toffset, source, soffset,
                           count, after, reject);
      }
      break;

   case JIT_EXIT_MAP_SIGNAL:
      {
         if (!jit_has_runtime(thread->jit))
            return;   // Called during constant folding

         sig_shared_t  *src_ss     = args[0].pointer;
         uint32_t       src_offset = args[1].integer;
         sig_shared_t  *dst_ss     = args[2].pointer;
         uint32_t       dst_offset = args[3].integer;
         uint32_t       count      = args[4].integer;

         x_map_signal(src_ss, src_offset, dst_ss, dst_offset, count);
      }
      break;

   case JIT_EXIT_MAP_CONST:
      {
         if (!jit_has_runtime(thread->jit))
            return;   // Called during constant folding

         sig_shared_t *dst_ss     = args[0].pointer;
         uint32_t      dst_offset = args[1].integer;
         jit_scalar_t  initval    = { .integer = args[2].integer };
         uint32_t      dst_count  = args[3].integer;
         bool          scalar     = args[4].integer;

         const void *vptr = scalar ? &initval.integer : initval.pointer;

         x_map_const(dst_ss, dst_offset, vptr, dst_count);
      }
      break;

   case JIT_EXIT_MAP_TRANSACTION:
      {
         if (!jit_has_runtime(thread->jit))
            return;   // Called during constant folding

         sig_shared_t  *src_ss     = args[0].pointer;
         uint32_t       src_offset = args[1].integer;
         sig_shared_t  *dst_ss     = args[2].pointer;
         uint32_t       dst_offset = args[3].integer;
         uint32_t       count      = args[4].integer;

         x_map_transaction(src_ss, src_offset, dst_ss, dst_offset, count);
      }
      break;

   case JIT_EXIT_SCHED_PROCESS:
      {
         if (!jit_has_runtime(thread->jit))
            return;   // TODO: this should not be necessary

         __nvc_sched_process(anchor, args, tlab);
      }
      break;

   case JIT_EXIT_SCHED_WAVEFORM:
      __nvc_sched_waveform(anchor, args, tlab);
      break;

   case JIT_EXIT_SCHED_EVENT:
      {
         sig_shared_t *shared  = args[0].pointer;
         int32_t       offset  = args[1].integer;
         int32_t       count   = args[2].integer;

         x_sched_event(shared, offset, count);
      }
      break;

   case JIT_EXIT_IMPLICIT_EVENT:
      {
         if (!jit_has_runtime(thread->jit))
            return;   // Called during constant folding

         sig_shared_t *shared  = args[0].pointer;
         int32_t       offset  = args[1].integer;
         int32_t       count   = args[2].integer;
         sig_shared_t *wake    = args[3].pointer;

         x_implicit_event(shared, offset, count, wake);
      }
      break;

   case JIT_EXIT_ALIAS_SIGNAL:
      {
         if (!jit_has_runtime(thread->jit))
            return;   // Called during constant folding

         sig_shared_t *ss    = args[0].pointer;
         tree_t        where = args[1].pointer;

         x_alias_signal(ss, where);
      }
      break;

   case JIT_EXIT_DISCONNECT:
      {
         sig_shared_t *shared = args[0].pointer;
         int32_t       offset = args[1].integer;
         int32_t       count  = args[2].integer;
         int64_t       reject = args[3].integer;
         int64_t       after  = args[4].integer;

         x_disconnect(shared, offset, count, after, reject);
      }
      break;

   case JIT_EXIT_ELAB_ORDER_FAIL:
      {
         tree_t where = args[0].pointer;
         x_elab_order_fail(where);
      }
      break;

   case JIT_EXIT_UNREACHABLE:
      {
         tree_t where = args[0].pointer;
         x_unreachable(where);
      }
      break;

   case JIT_EXIT_OVERFLOW:
      {
         int32_t lhs   = args[0].integer;
         int32_t rhs   = args[1].integer;
         tree_t  where = args[2].pointer;

         x_overflow(lhs, rhs, where);
      }
      break;

   case JIT_EXIT_INDEX_FAIL:
      {
         int64_t      value = args[0].integer;
         int64_t      left  = args[1].integer;
         int64_t      right = args[2].integer;
         range_kind_t dir   = args[3].integer;
         tree_t       where = args[4].pointer;
         tree_t       hint  = args[5].pointer;

         x_index_fail(value, left, right, dir, where, hint);
      }
      break;

   case JIT_EXIT_RANGE_FAIL:
      {
         int64_t      value = args[0].integer;
         int64_t      left  = args[1].integer;
         int64_t      right = args[2].integer;
         range_kind_t dir   = args[3].integer;
         tree_t       where = args[4].pointer;
         tree_t       hint  = args[5].pointer;

         x_range_fail(value, left, right, dir, where, hint);
      }
      break;

   case JIT_EXIT_FORCE:
      {
         sig_shared_t *shared = args[0].pointer;
         int32_t       offset = args[1].integer;
         int32_t       count  = args[2].integer;
         jit_scalar_t  value  = { .integer = args[3].integer };
         bool          scalar = args[4].integer;

         if (scalar)
            x_force(shared, offset, count, &value.integer);
         else
            x_force(shared, offset, count, value.pointer);
      }
      break;

   case JIT_EXIT_RELEASE:
      {
         sig_shared_t *shared = args[0].pointer;
         int32_t       offset = args[1].integer;
         int32_t       count  = args[2].integer;

         x_release(shared, offset, count);
      }
      break;

   case JIT_EXIT_DEPOSIT_SIGNAL:
      {
         sig_shared_t *shared = args[0].pointer;
         int32_t       offset = args[1].integer;
         int32_t       count  = args[2].integer;
         jit_scalar_t  value  = { .integer = args[3].integer };
         bool          scalar = args[4].integer;

         if (scalar)
            x_deposit_signal(shared, offset, count, &value.integer);
         else
            x_deposit_signal(shared, offset, count, value.pointer);
      }
      break;

   case JIT_EXIT_PUSH_SCOPE:
      {
         if (!jit_has_runtime(thread->jit))
            return;   // Called during constant folding

         tree_t  where = args[0].pointer;
         int32_t size  = args[1].integer;

         x_push_scope(where, size);
      }
      break;

   case JIT_EXIT_POP_SCOPE:
      {
         if (!jit_has_runtime(thread->jit))
            return;   // Called during constant folding

         x_pop_scope();
      }
      break;

   case JIT_EXIT_FUNC_WAIT:
      {
         x_func_wait();
      }
      break;

   case JIT_EXIT_DIV_ZERO:
      {
         tree_t where = args[0].pointer;
         x_div_zero(where);
      }
      break;

   case JIT_EXIT_LENGTH_FAIL:
      {
         int32_t left  = args[0].integer;
         int32_t right = args[1].integer;
         int32_t dim   = args[2].integer;
         tree_t  where = args[3].pointer;

         x_length_fail(left, right, dim, where);
      }
      break;

   case JIT_EXIT_NULL_DEREF:
      {
         tree_t where = args[0].pointer;
         x_null_deref(where);
      }
      break;

   case JIT_EXIT_EXPONENT_FAIL:
      {
         int32_t value = args[0].integer;
         tree_t  where = args[1].pointer;

         x_exponent_fail(value, where);
      }
      break;

   case JIT_EXIT_FILE_OPEN:
      {
         int8_t   *status     = args[0].pointer;
         void    **_fp        = args[1].pointer;
         uint8_t  *name_bytes = args[2].pointer;
         int32_t   name_len   = args[3].integer;
         int32_t   mode       = args[4].integer;

         x_file_open(status, _fp, name_bytes, name_len, mode);
      }
      break;

   case JIT_EXIT_FILE_READ:
      {
         void    **_fp   = args[0].pointer;
         uint8_t  *data  = args[1].pointer;
         int64_t   size  = args[2].integer;
         int64_t   count = args[3].integer;

         args[0].integer = x_file_read(_fp, data, size, count);
      }
      break;

   case JIT_EXIT_FILE_WRITE:
      {
         void         **_fp    = args[0].pointer;
         jit_scalar_t   data   = { .integer = args[1].integer };
         size_t         size   = args[2].integer;
         size_t         count  = args[3].integer;
         bool           scalar = args[4].integer;

         if (scalar)
            x_file_write(_fp, &data.integer, size, count);
         else
            x_file_write(_fp, data.pointer, size, count);
      }
      break;

   case JIT_EXIT_DEBUG_OUT:
      {
         int64_t value = args[0].integer;
         debugf("DEBUG %"PRIi64, value);
      }
      break;

   case JIT_EXIT_LAST_EVENT:
      __nvc_last_event(anchor, args, tlab);
      break;

   case JIT_EXIT_LAST_ACTIVE:
      {
         sig_shared_t *shared = args[0].pointer;
         uint32_t      offset = args[1].integer;
         uint32_t      count  = args[2].integer;

         args[0].integer = x_last_active(shared, offset, count);
      }
      break;

   case JIT_EXIT_TEST_EVENT:
      __nvc_test_event(anchor, args, tlab);
      break;

   case JIT_EXIT_TEST_ACTIVE:
      {
         sig_shared_t *shared = args[0].pointer;
         int32_t       offset = args[1].integer;
         int32_t       count  = args[2].integer;

         args[0].integer = x_test_net_active(shared, offset, count);
      }
      break;

   case JIT_EXIT_DRIVING:
      {
         sig_shared_t *shared = args[0].pointer;
         int32_t       offset = args[1].integer;
         int32_t       count  = args[2].integer;

         args[0].integer = x_driving(shared, offset, count);
      }
      break;

   case JIT_EXIT_DRIVING_VALUE:
      {
         sig_shared_t *shared = args[0].pointer;
         int32_t       offset = args[1].integer;
         int32_t       count  = args[2].integer;

         args[0].pointer = x_driving_value(shared, offset, count);
      }
      break;

   case JIT_EXIT_COVER_TOGGLE:
      {
         if (!jit_has_runtime(thread->jit))
            return;   // Called during constant folding

         sig_shared_t *shared = args[0].pointer;
         int32_t       tag    = args[1].integer;

         x_cover_setup_toggle_cb(shared, tag);
      }
      break;

   case JIT_EXIT_COVER_STATE:
      {
         if (!jit_has_runtime(thread->jit))
            return;   // Called during constant folding

         sig_shared_t *shared = args[0].pointer;
         int64_t      low     = args[1].integer;
         int32_t      tag     = args[2].integer;

         x_cover_setup_state_cb(shared, low, tag);
      }
      break;

   case JIT_EXIT_PROCESS_INIT:
      {
         if (!jit_has_runtime(thread->jit))
            return;   // Called during constant folding

         jit_handle_t handle = args[0].integer;
         tree_t       where  = args[1].pointer;

         x_process_init(handle, where);
      }
      break;

   case JIT_EXIT_CLEAR_EVENT:
      {
         sig_shared_t *shared = args[0].pointer;
         int32_t       offset = args[1].integer;
         int32_t       count  = args[2].integer;

         x_clear_event(shared, offset, count);
      }
      break;

   case JIT_EXIT_ENTER_STATE:
      {
         int32_t state = args[0].integer;

         x_enter_state(state);
      }
      break;

   case JIT_EXIT_REFLECT_VALUE:
      {
         void         *context = args[0].pointer;
         jit_scalar_t  value   = args[1];
         tree_t        where   = args[2].pointer;

         args[0].pointer = x_reflect_value(context, value, where, args + 3);
      }
      break;

   case JIT_EXIT_REFLECT_SUBTYPE:
      {
         void   *context = args[0].pointer;
         tree_t  where   = args[1].pointer;

         args[0].pointer = x_reflect_subtype(context, where, args + 3);
      }
      break;

   case JIT_EXIT_FUNCTION_TRIGGER:
      {
         jit_handle_t handle = args[0].integer;
         unsigned     nargs  = args[1].integer;

         if (jit_has_runtime(thread->jit))
            args[0].pointer = x_function_trigger(handle, nargs, args + 2);
         else
            args[0].pointer = NULL;   // Called during constant folding
      }
      break;

   case JIT_EXIT_OR_TRIGGER:
      {
         void *left  = args[0].pointer;
         void *right = args[1].pointer;

         if (jit_has_runtime(thread->jit))
            args[0].pointer = x_or_trigger(left, right);
         else
            args[0].pointer = NULL;   // Called during constant folding
      }
      break;

   case JIT_EXIT_CMP_TRIGGER:
      {
         sig_shared_t *shared = args[0].pointer;
         int32_t       offset = args[1].integer;
         int64_t       right  = args[2].integer;

         if (jit_has_runtime(thread->jit))
            args[0].pointer = x_cmp_trigger(shared, offset, right);
         else
            args[0].pointer = NULL;   // Called during constant folding
      }
      break;

   case JIT_EXIT_ADD_TRIGGER:
      {
         void *trigger = args[0].pointer;

         if (trigger != NULL)
            x_add_trigger(trigger);
      }
      break;

   case JIT_EXIT_PORT_CONVERSION:
      {
         jit_handle_t  handle1  = args[0].integer;
         void         *context1 = args[1].pointer;
         jit_handle_t  handle2  = args[2].integer;
         void         *context2 = args[3].pointer;

         if (jit_has_runtime(thread->jit)) {
            ffi_closure_t driving = { handle1, context1 };
            ffi_closure_t effective = { handle2, context2 };
            args[0].pointer = x_port_conversion(&driving, &effective);
         }
         else
            args[0].pointer = NULL;   // Called during constant folding
      }
      break;

   case JIT_EXIT_CONVERT_IN:
      {
         void         *conv   = args[0].pointer;
         sig_shared_t *shared = args[1].pointer;
         int32_t       offset = args[2].integer;
         int32_t       count  = args[3].integer;

         if (conv != NULL)
            x_convert_in(conv, shared, offset, count);
      }
      break;

   case JIT_EXIT_CONVERT_OUT:
      {
         void         *conv   = args[0].pointer;
         sig_shared_t *shared = args[1].pointer;
         int32_t       offset = args[2].integer;
         int32_t       count  = args[3].integer;

         if (conv != NULL)
            x_convert_out(conv, shared, offset, count);
      }
      break;

   case JIT_EXIT_BIND_FOREIGN:
      {
         const uint8_t *spec   = args[0].pointer;
         size_t         length = args[1].integer;
         tree_t         where  = args[2].pointer;

         jit_bind_foreign(anchor->func, spec, length, where);
      }
      break;

   case JIT_EXIT_INSTANCE_NAME:
      {
         const attr_kind_t kind = args[0].integer;

         LOCAL_TEXT_BUF tb = tb_new();
         x_instance_name(kind, tb);
         ffi_return_string(tb_get(tb), args, tlab);
      }
      break;

   default:
      fatal_trace("unhandled exit %s", jit_exit_name(which));
   }

   thread->anchor = NULL;
}

////////////////////////////////////////////////////////////////////////////////
// Entry points from AOT compiled code

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
void *__nvc_mspace_alloc(uintptr_t size, jit_anchor_t *anchor)
{
   jit_thread_local_t *thread = jit_attach_thread(anchor);

   if (unlikely(size > UINT32_MAX)) {
      jit_msg(NULL, DIAG_FATAL, "attempting to allocate %zu byte object "
              "which is larger than the maximum supported %u bytes",
              size, UINT32_MAX);
      __builtin_unreachable();
   }
   else if (size == 0)
      size = 1;   // Never return a NULL pointer

   void *ptr = jit_mspace_alloc(size);

   thread->anchor = NULL;
   return ptr;
}

DLLEXPORT
void __nvc_putpriv(jit_handle_t handle, void *data)
{
   jit_t *j = jit_for_thread();
   jit_func_t *f = jit_get_func(j, handle);

   store_release(jit_get_privdata_ptr(j, f), data);
}

DLLEXPORT
object_t *__nvc_get_object(const char *unit, ptrdiff_t offset)
{
   return object_from_locus(ident_new(unit), offset, lib_load_handler);
}
