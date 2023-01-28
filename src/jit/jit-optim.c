//
//  Copyright (C) 2022  Nick Gasson
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
#include "array.h"
#include "jit/jit-priv.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

////////////////////////////////////////////////////////////////////////////////
// Control flow graph construction

static bool cfg_is_terminator(jit_func_t *func, jit_ir_t *ir)
{
   if (ir->op == MACRO_CASE)
      return ir + 1 < func->irbuf + func->nirs && (ir + 1)->op != MACRO_CASE;
   else
      return ir->op == J_JUMP || ir->op == J_RET;
}

static void cfg_add_one_edge(jit_edge_list_t *list, unsigned edge)
{
   if (list->count < 4)
      list->u.edges[list->count++] = edge;
   else if (list->count == 4) {
      unsigned *ptr = xmalloc_array(16, sizeof(unsigned));
      memcpy(ptr, list->u.edges, 4 * sizeof(unsigned));

      list->max = 16;
      list->u.external = ptr;
      list->u.external[list->count++] = edge;
   }
   else if (list->count == list->max) {
      list->max *= 2;
      list->u.external =
         xrealloc_array(list->u.external, list->max, sizeof(unsigned));
      list->u.external[list->count++] = edge;
   }
   else
      list->u.external[list->count++] = edge;
}

static void cfg_add_edge(jit_cfg_t *cfg, jit_block_t *from, jit_block_t *to)
{
   cfg_add_one_edge(&(from->out), to - cfg->blocks);
   cfg_add_one_edge(&(to->in), from - cfg->blocks);
}

static jit_reg_t cfg_get_reg(jit_value_t value)
{
   switch (value.kind) {
   case JIT_VALUE_REG:
   case JIT_ADDR_REG:
      return value.reg;
   default:
      return JIT_REG_INVALID;
   }
}

static inline bool cfg_reads_result(jit_ir_t *ir)
{
   return ir->op == MACRO_COPY || ir->op == MACRO_CASE || ir->op == MACRO_BZERO;
}

static inline bool cfg_writes_result(jit_ir_t *ir)
{
   return ir->result != JIT_REG_INVALID && ir->op != MACRO_CASE;
}

static void cfg_liveness(jit_cfg_t *cfg, jit_func_t *f)
{
   // Algorithm from "Engineering a Compiler" chapter 8.6

   for (int i = 0; i < cfg->nblocks; i++) {
      jit_block_t *b = &(cfg->blocks[i]);
      mask_init(&b->livein, f->nregs);
      mask_init(&b->varkill, f->nregs);
      mask_init(&b->liveout, f->nregs);

      for (int j = b->first; j <= b->last; j++) {
         jit_ir_t *ir = &(f->irbuf[j]);

         jit_reg_t reg1 = cfg_get_reg(ir->arg1);
         if (reg1 != JIT_REG_INVALID && !mask_test(&b->varkill, reg1))
            mask_set(&b->livein, reg1);

         jit_reg_t reg2 = cfg_get_reg(ir->arg2);
         if (reg2 != JIT_REG_INVALID && !mask_test(&b->varkill, reg2))
            mask_set(&b->livein, reg2);

         if (cfg_reads_result(ir))
            mask_set(&b->livein, ir->result);

         if (cfg_writes_result(ir))
            mask_set(&b->varkill, ir->result);
      }
   }

   bit_mask_t new, tmp;
   mask_init(&new, f->nregs);
   mask_init(&tmp, f->nregs);

   bool changed;
   do {
      changed = false;

      for (int i = cfg->nblocks - 1; i >= 0; i--) {
         jit_block_t *b = &(cfg->blocks[i]);
         mask_clearall(&new);

         for (int j = 0; j < b->out.count; j++) {
            jit_block_t *succ = &(cfg->blocks[jit_get_edge(&b->out, j)]);
            mask_copy(&tmp, &succ->liveout);
            mask_subtract(&tmp, &succ->varkill);
            mask_union(&tmp, &succ->livein);
            mask_union(&new, &tmp);
         }

         if (!mask_eq(&new, &b->liveout)) {
            mask_copy(&b->liveout, &new);
            changed = true;
         }
      }
   } while (changed);

   // Replaced "upward exposed variables" set with live-in
   for (int i = 0; i < cfg->nblocks; i++) {
      jit_block_t *b = &(cfg->blocks[i]);
      mask_copy(&tmp, &b->liveout);
      mask_subtract(&tmp, &b->varkill);
      mask_union(&b->livein, &tmp);
   }

   mask_free(&new);
   mask_free(&tmp);
}

jit_cfg_t *jit_get_cfg(jit_func_t *f)
{
   if (f->cfg != NULL)
      return f->cfg;

   int nb = 1;
   for (int i = 0, first = 0; i < f->nirs; i++) {
      jit_ir_t *ir = &(f->irbuf[i]);
      if (ir->target && i > 0 && first != i)
         first = i, nb++;
      if (cfg_is_terminator(f, ir) && i + 1 < f->nirs)
         first = i + 1, nb++;
   }

   jit_cfg_t *cfg = xcalloc_flex(sizeof(jit_cfg_t), nb, sizeof(jit_block_t));
   cfg->nblocks = nb;

   jit_block_t *bb = cfg->blocks;
   for (int i = 0; i < f->nirs; i++) {
      jit_ir_t *ir = &(f->irbuf[i]);
      if (ir->target && i > 0 && bb->first != i) {
         if (!bb->returns && !bb->aborts)
            cfg_add_edge(cfg, bb, bb + 1);
         (++bb)->first = i;
      }

      bb->last = i;

      if (ir->op == J_RET)
         bb->returns = 1;
      else if (jit_will_abort(ir))
         bb->aborts = 1;

      if (cfg_is_terminator(f, ir) && i + 1 < f->nirs) {
         if ((ir->op == J_JUMP && ir->cc != JIT_CC_NONE)
             || ir->op == MACRO_CASE)
            cfg_add_edge(cfg, bb, bb + 1);   // Fall-through case
         (++bb)->first = i + 1;
      }
   }

   for (int i = 0; i < f->nirs; i++) {
      jit_ir_t *ir = &(f->irbuf[i]);
      jit_label_t label = JIT_LABEL_INVALID;

      if (ir->op == J_JUMP)
         label = ir->arg1.label;
      else if (ir->op == MACRO_CASE)
         label = ir->arg2.label;

      if (label != JIT_LABEL_INVALID) {
         assert(label < f->nirs);
         jit_block_t *from = jit_block_for(cfg, i);
         jit_block_t *to = jit_block_for(cfg, label);
         cfg_add_edge(cfg, from, to);
      }
   }

   cfg_liveness(cfg, f);

   return (f->cfg = cfg);
}

void jit_free_cfg(jit_func_t *f)
{
   if (f->cfg != NULL) {
      for (int i = 0; i < f->cfg->nblocks; i++) {
         jit_block_t *b = &(f->cfg->blocks[i]);
         mask_free(&b->livein);
         mask_free(&b->liveout);
         mask_free(&b->varkill);

         if (b->in.max > 4) free(b->in.u.external);
         if (b->out.max > 4) free(b->out.u.external);
      }

      free(f->cfg);
      f->cfg = NULL;
   }
}

jit_block_t *jit_block_for(jit_cfg_t *cfg, int pos)
{
   for (int low = 0, high = cfg->nblocks - 1; low <= high; ) {
      const int mid = (low + high) / 2;
      jit_block_t *bb = &(cfg->blocks[mid]);

      if (bb->last < pos)
         low = mid + 1;
      else if (bb->first > pos)
         high = mid - 1;
      else
         return bb;
   }

   fatal_trace("operation %d is not in any block", pos);
}

int jit_get_edge(jit_edge_list_t *list, int nth)
{
   assert(nth < list->count);
   if (list->max <= 4)
      return list->u.edges[nth];
   else
      return list->u.external[nth];
}

////////////////////////////////////////////////////////////////////////////////
// Local value numbering and simple peepholes

#define FOR_ALL_SIZES(size, macro) do {                 \
      switch (size) {                                   \
      case JIT_SZ_8: macro(int8_t); break;              \
      case JIT_SZ_16: macro(int16_t); break;            \
      case JIT_SZ_32: macro(int32_t); break;            \
      case JIT_SZ_UNSPEC:                               \
      case JIT_SZ_64: macro(int64_t); break;            \
      }                                                 \
   } while (0)

typedef unsigned valnum_t;

#define VN_INVALID  UINT_MAX
#define SMALL_CONST 100
#define MAX_CONSTS  32
#define FIRST_VN    (SMALL_CONST + MAX_CONSTS)

#define LVN_REG(r) ((jit_value_t){ .kind = JIT_VALUE_REG, .reg = (r) })
#define LVN_CONST(i) ((jit_value_t){ .kind = JIT_VALUE_INT64, .int64 = (i) })

typedef struct _lvn_tab lvn_tab_t;

typedef struct {
   jit_func_t *func;
   valnum_t   *regvn;
   valnum_t    nextvn;
   lvn_tab_t  *hashtab;
   size_t      tabsz;
   int64_t     consttab[MAX_CONSTS];
   unsigned    nconsts;
} lvn_state_t;

typedef struct _lvn_tab {
   jit_ir_t  *ir;
   valnum_t   vn;
} lvn_tab_t;

static void jit_lvn_mov(jit_ir_t *ir, lvn_state_t *state);

static inline valnum_t lvn_new_value(lvn_state_t *state)
{
   return state->nextvn++;
}

static bool lvn_get_const(valnum_t vn, lvn_state_t *state, int64_t *cval)
{
   if (vn < SMALL_CONST) {
      *cval = vn;
      return true;
   }
   else if (vn < SMALL_CONST + MAX_CONSTS) {
      *cval = state->consttab[vn - SMALL_CONST];
      return true;
   }
   else
      return false;
}

static bool lvn_is_const(jit_value_t value, lvn_state_t *state, int64_t *cval)
{
   switch (value.kind) {
   case JIT_VALUE_INT64:
      *cval = value.int64;
      return true;
   case JIT_VALUE_REG:
      return lvn_get_const(state->regvn[value.reg], state, cval);
   default:
      return false;
   }
}

static inline bool lvn_can_fold(jit_ir_t *ir, lvn_state_t *state,
                                int64_t *arg1, int64_t *arg2)
{
   return lvn_is_const(ir->arg1, state, arg1)
      && lvn_is_const(ir->arg2, state, arg2);
}

static void lvn_convert_mov(jit_ir_t *ir, lvn_state_t *state, jit_value_t value)
{
   ir->op        = J_MOV;
   ir->size      = JIT_SZ_UNSPEC;
   ir->cc        = JIT_CC_NONE;
   ir->arg1      = value;
   ir->arg2.kind = JIT_VALUE_INVALID;

   jit_lvn_mov(ir, state);
}

static void lvn_convert_nop(jit_ir_t *ir)
{
   ir->op        = J_NOP;
   ir->size      = JIT_SZ_UNSPEC;
   ir->cc        = JIT_CC_NONE;
   ir->arg1.kind = JIT_VALUE_INVALID;
   ir->arg2.kind = JIT_VALUE_INVALID;
}

static valnum_t lvn_value_num(jit_value_t value, lvn_state_t *state)
{
   switch (value.kind) {
   case JIT_VALUE_REG:
      if (state->regvn[value.reg] != VN_INVALID)
         return state->regvn[value.reg];
      else
         return (state->regvn[value.reg] = lvn_new_value(state));

   case JIT_VALUE_INT64:
      if (value.int64 >= 0 && value.int64 < SMALL_CONST)
         return value.int64;
      else {
         for (int i = 0; i < state->nconsts; i++) {
            if (state->consttab[i] == value.int64)
               return SMALL_CONST + i;
         }

         if (state->nconsts < MAX_CONSTS) {
            state->consttab[state->nconsts] = value.int64;
            return SMALL_CONST + state->nconsts++;
         }
         else
            return lvn_new_value(state);
      }

   case JIT_VALUE_INVALID:
      return VN_INVALID;

   case JIT_VALUE_HANDLE:
   case JIT_VALUE_DOUBLE:
      return lvn_new_value(state);

   default:
      fatal_trace("cannot handle value kind %d in lvn_value_num", value.kind);
   }
}

static inline bool lvn_is_commutative(jit_op_t op)
{
   return op == J_ADD || op == J_MUL || op == J_AND || op == J_OR;
}

static void lvn_commute_const(jit_ir_t *ir, lvn_state_t *state)
{
   assert(lvn_is_commutative(ir->op));

   int64_t cval;
   if (lvn_is_const(ir->arg1, state, &cval)) {
      jit_value_t tmp = ir->arg1;
      ir->arg1 = ir->arg2;
      ir->arg2 = tmp;
   }
}

static void lvn_get_tuple(jit_ir_t *ir, lvn_state_t *state, int tuple[3])
{
   tuple[0] = ir->op | ir->size << 8 | ir->cc << 11;

   const valnum_t vn1 = lvn_value_num(ir->arg1, state);
   const valnum_t vn2 = lvn_value_num(ir->arg2, state);

   if (lvn_is_commutative(ir->op) && vn1 > vn2) {
      tuple[1] = vn2;
      tuple[2] = vn1;
   }
   else {
      tuple[1] = vn1;
      tuple[2] = vn2;
   }
}

static void jit_lvn_generic(jit_ir_t *ir, lvn_state_t *state, valnum_t vn)
{
   assert(ir->result != JIT_REG_INVALID);

   int tuple[3];
   lvn_get_tuple(ir, state, tuple);

   const unsigned hash = tuple[0]*29 + tuple[1]*1093 + tuple[2]*6037;

   for (int idx = hash & (state->tabsz - 1), limit = 0, stale = -1; limit < 10;
        idx = (idx + 1) & (state->tabsz - 1), limit++) {

      lvn_tab_t *tab = &(state->hashtab[idx]);
      if (tab->ir == NULL) {
         if (stale >= 0)  // Reuse earlier stale entry if possible
            tab = &(state->hashtab[stale]);
         tab->ir = ir;
         tab->vn = state->regvn[ir->result] =
            (vn == VN_INVALID ? lvn_new_value(state) : vn);
         break;
      }
      else if (tab->vn != state->regvn[tab->ir->result]) {
         // Stale entry may be reused if we do not find matching value
         stale = idx;
         continue;
      }
      else {
         int cmp[3];
         lvn_get_tuple(tab->ir, state, cmp);

         if (cmp[0] == tuple[0] && cmp[1] == tuple[1] && cmp[2] == tuple[2]) {
            assert(tab->ir->result != JIT_REG_INVALID);

            ir->op   = J_MOV;
            ir->size = JIT_SZ_UNSPEC;
            ir->cc   = JIT_CC_NONE;

            // Propagate constants where possible
            int64_t cval;
            if (lvn_get_const(state->regvn[tab->ir->result], state, &cval))
               ir->arg1 = LVN_CONST(cval);
            else
               ir->arg1 = LVN_REG(tab->ir->result);

            ir->arg2.kind = JIT_VALUE_INVALID;

            state->regvn[ir->result] = tab->vn;
            break;
         }
      }
   }
}

static void jit_lvn_mul(jit_ir_t *ir, lvn_state_t *state)
{
   int64_t lhs, rhs;
   if (lvn_can_fold(ir, state, &lhs, &rhs)) {
#define FOLD_MUL(type) do {                                             \
         type result;                                                   \
         if (!__builtin_mul_overflow(lhs, rhs, &result)) {              \
            lvn_convert_mov(ir, state, LVN_CONST(result));              \
            return;                                                     \
         }                                                              \
      } while (0)

      FOR_ALL_SIZES(ir->size, FOLD_MUL);
#undef FOLD_MUL
   }

   lvn_commute_const(ir, state);

   if (lvn_is_const(ir->arg2, state, &rhs)) {
      if (rhs == 0) {
         lvn_convert_mov(ir, state, LVN_CONST(0));
         return;
      }
      else if (rhs == 1) {
         lvn_convert_mov(ir, state, LVN_REG(ir->arg1.reg));
         return;
      }
      else if (rhs > 0 && is_power_of_2(rhs) && ir->size == JIT_SZ_UNSPEC) {
         ir->op = J_SHL;
         ir->arg2 = LVN_CONST(ilog2(rhs));
         jit_lvn_generic(ir, state, VN_INVALID);
         return;
      }
   }

   jit_lvn_generic(ir, state, VN_INVALID);
}

static void jit_lvn_div(jit_ir_t *ir, lvn_state_t *state)
{
   int64_t lhs, rhs;
   if (lvn_can_fold(ir, state, &lhs, &rhs) && rhs != 0) {
      // XXX: potential bug here with INT_MIN/-1
#define FOLD_DIV(type) do {                                             \
         type result = (type)lhs / (type)rhs;                           \
         lvn_convert_mov(ir, state, LVN_CONST(result));                 \
         return;                                                        \
      } while (0)

      FOR_ALL_SIZES(ir->size, FOLD_DIV);
#undef FOLD_DIV
   }
   else if (lvn_is_const(ir->arg2, state, &rhs) && rhs == 1) {
      lvn_convert_mov(ir, state, LVN_REG(ir->arg1.reg));
      return;
   }

   jit_lvn_generic(ir, state, VN_INVALID);
}

static void jit_lvn_add(jit_ir_t *ir, lvn_state_t *state)
{
   int64_t lhs, rhs;
   if (lvn_can_fold(ir, state, &lhs, &rhs)) {
#define FOLD_ADD(type) do {                                             \
         type result;                                                   \
         if (!__builtin_add_overflow(lhs, rhs, &result)) {              \
            lvn_convert_mov(ir, state, LVN_CONST(result));              \
            return;                                                     \
         }                                                              \
      } while (0)

      FOR_ALL_SIZES(ir->size, FOLD_ADD);
#undef FOLD_ADD
   }

   lvn_commute_const(ir, state);

   if (lvn_is_const(ir->arg2, state, &rhs) && rhs == 0) {
      lvn_convert_mov(ir, state, LVN_REG(ir->arg1.reg));
      return;
   }

   jit_lvn_generic(ir, state, VN_INVALID);
}

static void jit_lvn_sub(jit_ir_t *ir, lvn_state_t *state)
{
   int64_t lhs, rhs;
   if (lvn_can_fold(ir, state, &lhs, &rhs)) {
#define FOLD_SUB(type) do {                                             \
         type result;                                                   \
         if (!__builtin_sub_overflow(lhs, rhs, &result)) {              \
            lvn_convert_mov(ir, state, LVN_CONST(result));              \
            return;                                                     \
         }                                                              \
      } while (0)

      FOR_ALL_SIZES(ir->size, FOLD_SUB);
#undef FOLD_SUB
   }

   if (lvn_is_const(ir->arg2, state, &rhs) && rhs == 0) {
      lvn_convert_mov(ir, state, LVN_REG(ir->arg1.reg));
      return;
   }
   else if (lvn_is_const(ir->arg1, state, &lhs) && lhs == 0
            && ir->cc == JIT_CC_NONE && ir->size == JIT_SZ_UNSPEC) {
     ir->op        = J_NEG;
     ir->arg1      = ir->arg2;
     ir->arg2.kind = JIT_VALUE_INVALID;
     jit_lvn_generic(ir, state, VN_INVALID);
     return;
   }

   jit_lvn_generic(ir, state, VN_INVALID);
}

static void jit_lvn_neg(jit_ir_t *ir, lvn_state_t *state)
{
   int64_t value;
   if (lvn_is_const(ir->arg1, state, &value))
      lvn_convert_mov(ir, state, LVN_CONST(-value));
   else
      jit_lvn_generic(ir, state, VN_INVALID);
}

static void jit_lvn_mov(jit_ir_t *ir, lvn_state_t *state)
{
   if (ir->arg1.kind == JIT_VALUE_REG && ir->arg1.reg == ir->result) {
      lvn_convert_nop(ir);
      return;
   }

   valnum_t vn = lvn_value_num(ir->arg1, state);
   if (state->regvn[ir->result] == vn) {
      // Result register already contains this value
      lvn_convert_nop(ir);
      return;
   }

   jit_lvn_generic(ir, state, vn);
}

static void jit_lvn_cmp(jit_ir_t *ir, lvn_state_t *state)
{
   int64_t lhs, rhs;
   if (lvn_can_fold(ir, state, &lhs, &rhs)) {
      bool result = false;
      switch (ir->cc) {
      case JIT_CC_EQ: result = (lhs == rhs); break;
      case JIT_CC_NE: result = (lhs != rhs); break;
      case JIT_CC_LT: result = (lhs < rhs); break;
      case JIT_CC_GT: result = (lhs > rhs); break;
      case JIT_CC_LE: result = (lhs <= rhs); break;
      case JIT_CC_GE: result = (lhs >= rhs); break;
      default:
         fatal_trace("unhandled condition code in jit_lvn_cmp");
      }

      state->regvn[state->func->nregs] = result;
   }
}

static void jit_lvn_csel(jit_ir_t *ir, lvn_state_t *state)
{
   const int fconst = state->regvn[state->func->nregs];
   if (fconst != VN_INVALID)
      lvn_convert_mov(ir, state, fconst ? ir->arg1 : ir->arg2);
}

static void jit_lvn_cset(jit_ir_t *ir, lvn_state_t *state)
{
   const int fconst = state->regvn[state->func->nregs];
   if (fconst != VN_INVALID)
      lvn_convert_mov(ir, state, LVN_CONST(fconst));
}

static void jit_lvn_jump(jit_ir_t *ir, lvn_state_t *state)
{
   assert(ir->arg1.label < state->func->nirs);
   jit_ir_t *dest = &(state->func->irbuf[ir->arg1.label]);

   const int fconst = state->regvn[state->func->nregs];
   if (ir->cc != JIT_CC_NONE && fconst != VN_INVALID) {
      if (fconst == (ir->cc == JIT_CC_T))
         ir->cc = JIT_CC_NONE;
      else {
         lvn_convert_nop(ir);
         return;
      }
   }

   if (dest == ir + 1)
      lvn_convert_nop(ir);
   else if (dest->op == J_JUMP && dest->cc == JIT_CC_NONE)
      ir->arg1 = dest->arg1;     // Simple jump threading
}

static void jit_lvn_clamp(jit_ir_t *ir, lvn_state_t *state)
{
   int64_t value;
   if (lvn_is_const(ir->arg1, state, &value))
      lvn_convert_mov(ir, state, LVN_CONST(value < 0 ? 0 : value));
   else
      jit_lvn_generic(ir, state, VN_INVALID);
}

static void jit_lvn_cneg(jit_ir_t *ir, lvn_state_t *state)
{
   const int fconst = state->regvn[state->func->nregs];
   if (fconst != VN_INVALID) {
      if (fconst) {
         ir->op = J_NEG;
         jit_lvn_neg(ir, state);
         return;
      }
      else {
         lvn_convert_mov(ir, state, ir->arg1);
         return;
      }
   }

   jit_lvn_generic(ir, state, VN_INVALID);
}

static void jit_lvn_copy(jit_ir_t *ir, lvn_state_t *state)
{
   // Clobbers the count register
   state->regvn[ir->result] = lvn_new_value(state);
}

static void jit_lvn_bzero(jit_ir_t *ir, lvn_state_t *state)
{
   // Clobbers the count register
   state->regvn[ir->result] = lvn_new_value(state);
}

static void jit_lvn_exp(jit_ir_t *ir, lvn_state_t *state)
{
   int64_t base, exp;
   if (lvn_can_fold(ir, state, &base, &exp))
      lvn_convert_mov(ir, state, LVN_CONST(ipow(base, exp)));
   else if (lvn_is_const(ir->arg1, state, &base) && base == 2) {
      ir->op = J_SHL;
      ir->arg1.int64 = 1;
      jit_lvn_generic(ir, state, VN_INVALID);
   }
   else
      jit_lvn_generic(ir, state, VN_INVALID);
}

void jit_do_lvn(jit_func_t *f)
{
   lvn_state_t state = {
      .tabsz  = next_power_of_2(f->nirs),
      .func   = f,
      .nextvn = FIRST_VN
   };

   state.regvn = xmalloc_array(f->nregs + 1, sizeof(valnum_t));
   state.hashtab = xcalloc_array(state.tabsz, sizeof(lvn_tab_t));

   bool reset = true;
   for (jit_ir_t *ir = f->irbuf; ir < f->irbuf + f->nirs;
        reset = cfg_is_terminator(f, ir), ir++) {

      if (reset || ir->target) {
         for (int j = 0; j < f->nregs + 1; j++)
            state.regvn[j] = VN_INVALID;
      }

      if (jit_writes_flags(ir))
         state.regvn[f->nregs] = VN_INVALID;

      switch (ir->op) {
      case J_MUL: jit_lvn_mul(ir, &state); break;
      case J_DIV: jit_lvn_div(ir, &state); break;
      case J_ADD: jit_lvn_add(ir, &state); break;
      case J_SUB: jit_lvn_sub(ir, &state); break;
      case J_NEG: jit_lvn_neg(ir, &state); break;
      case J_MOV: jit_lvn_mov(ir, &state); break;
      case J_CMP: jit_lvn_cmp(ir, &state); break;
      case J_CSEL: jit_lvn_csel(ir, &state); break;
      case J_CSET: jit_lvn_cset(ir, &state); break;
      case J_CNEG: jit_lvn_cneg(ir, &state); break;
      case J_JUMP: jit_lvn_jump(ir, &state); break;
      case J_CLAMP: jit_lvn_clamp(ir, &state); break;
      case MACRO_COPY: jit_lvn_copy(ir, &state); break;
      case MACRO_BZERO: jit_lvn_bzero(ir, &state); break;
      case MACRO_EXP: jit_lvn_exp(ir, &state); break;
      default: break;
      }
   }

   jit_free_cfg(f);   // Jump threading may have changed CFG

   free(state.regvn);
   free(state.hashtab);
}

////////////////////////////////////////////////////////////////////////////////
// Copy propagation

void jit_do_cprop(jit_func_t *f)
{
   jit_value_t *map LOCAL = xmalloc_array(f->nregs, sizeof(jit_value_t));

   bool reset = true;
   for (jit_ir_t *ir = f->irbuf; ir < f->irbuf + f->nirs;
        reset = cfg_is_terminator(f, ir), ir++) {

      if (reset || ir->target) {
         for (int j = 0; j < f->nregs; j++)
            map[j].kind = JIT_VALUE_INVALID;
      }

      if (ir->arg1.kind == JIT_VALUE_REG) {
         jit_value_t copy = map[ir->arg1.reg];
         if (copy.kind != JIT_VALUE_INVALID)
            ir->arg1 = copy;
      }

      if (ir->arg2.kind == JIT_VALUE_REG) {
         jit_value_t copy = map[ir->arg2.reg];
         if (copy.kind != JIT_VALUE_INVALID)
            ir->arg2 = copy;
      }

      if (ir->op == J_MOV)
         map[ir->result] = ir->arg1;
      else if (ir->result != JIT_REG_INVALID)
         map[ir->result].kind = JIT_VALUE_INVALID;
   }
}

////////////////////////////////////////////////////////////////////////////////
// Dead code elimination

static inline void dce_count_use(jit_value_t value, int *count)
{
   if (value.kind == JIT_VALUE_REG || value.kind == JIT_ADDR_REG)
      count[value.reg]++;
}

void jit_do_dce(jit_func_t *f)
{
   int *count LOCAL = xcalloc_array(f->nregs, sizeof(int));

   for (jit_ir_t *ir = f->irbuf; ir < f->irbuf + f->nirs; ir++) {
      dce_count_use(ir->arg1, count);
      dce_count_use(ir->arg2, count);

      if (cfg_reads_result(ir))
         count[ir->result]++;
   }

   for (jit_ir_t *ir = f->irbuf; ir < f->irbuf + f->nirs; ir++) {
      if (jit_writes_flags(ir))
          continue;
      else if (ir->result != JIT_REG_INVALID && count[ir->result] == 0) {
         ir->op        = J_NOP;
         ir->result    = JIT_REG_INVALID;
         ir->cc        = JIT_CC_NONE;
         ir->size      = JIT_SZ_UNSPEC;
         ir->arg1.kind = JIT_VALUE_INVALID;
         ir->arg2.kind = JIT_VALUE_INVALID;
      }
   }
}

////////////////////////////////////////////////////////////////////////////////
// NOP deletion

void jit_delete_nops(jit_func_t *f)
{
   jit_label_t *map LOCAL = xmalloc_array(f->nirs, sizeof(jit_label_t));

   int wptr = 0;
   for (jit_ir_t *ir = f->irbuf; ir < f->irbuf + f->nirs; ir++) {
      map[ir - f->irbuf] = wptr;

      if (ir->op != J_NOP) {
         jit_ir_t *dest = f->irbuf + wptr++;
         if (dest != ir)
            *dest = *ir;
      }
   }

   for (jit_ir_t *ir = f->irbuf; ir < f->irbuf + f->nirs; ir++) {
      if (ir->arg1.kind == JIT_VALUE_LABEL) {
         ir->arg1.label = map[ir->arg1.label];
         f->irbuf[ir->arg1.label].target = 1;
      }
      if (ir->arg2.kind == JIT_VALUE_LABEL) {
         ir->arg2.label = map[ir->arg2.label];
         f->irbuf[ir->arg2.label].target = 1;
      }
   }

   f->nirs = wptr;
}
