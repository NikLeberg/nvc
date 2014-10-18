//
//  Copyright (C) 2014  Nick Gasson
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
#include "phase.h"
#include "vcode.h"

#include <assert.h>
#include <stdlib.h>

static ident_t builtin_i;
static bool    verbose = false;

static vcode_reg_t lower_expr(tree_t expr);

static vcode_reg_t lower_func_arg(tree_t fcall, int nth)
{
   assert(nth < tree_params(fcall));

   tree_t param = tree_param(fcall, nth);

   assert(tree_subkind(param) == P_POS);
   assert(tree_pos(param) == nth);

   return lower_expr(tree_value(param));
}

static vcode_reg_t lower_builtin(tree_t fcall, ident_t builtin)
{
   if (icmp(builtin, "eq")) {
      return emit_cmp(VCODE_CMP_EQ,
                      lower_func_arg(fcall, 0),
                      lower_func_arg(fcall, 1));
   }
   else
      fatal_at(tree_loc(fcall), "cannot lower builtin %s", istr(builtin));
}

static vcode_reg_t lower_fcall(tree_t fcall)
{
   tree_t decl = tree_ref(fcall);

   ident_t builtin = tree_attr_str(decl, builtin_i);
   if (builtin != NULL)
      return lower_builtin(fcall, builtin);

   const int nargs = tree_params(fcall);
   vcode_reg_t args[nargs];
   for (int i = 0; i < nargs; i++)
      args[i] = lower_func_arg(fcall, i);

   return emit_fcall(tree_ident(decl), args, nargs);
}

static vcode_reg_t lower_literal(tree_t lit)
{
   switch (tree_subkind(lit)) {
   case L_INT:
      return emit_const(tree_ival(lit));

   default:
      fatal_at(tree_loc(lit), "cannot lower literal kind %d",
               tree_subkind(lit));
   }
}

static vcode_reg_t lower_expr(tree_t expr)
{
   switch (tree_kind(expr)) {
   case T_FCALL:
      return lower_fcall(expr);
   case T_LITERAL:
      return lower_literal(expr);
   default:
      fatal_at(tree_loc(expr), "cannot lower expression kind %s",
               tree_kind_str(tree_kind(expr)));
   }
}

static void lower_assert(tree_t stmt)
{
   emit_assert(lower_expr(tree_value(stmt)));
}

static void lower_wait(tree_t wait)
{
   vcode_reg_t rfor = VCODE_INVALID_REG;
   if (tree_has_value(wait))
      rfor = lower_expr(tree_value(wait));

   vcode_block_t resume = emit_block();
   emit_wait(resume, rfor);

   vcode_emit_to(resume);
}

static void lower_stmt(tree_t stmt)
{
   switch (tree_kind(stmt)) {
   case T_ASSERT:
      lower_assert(stmt);
      break;
   case T_WAIT:
      lower_wait(stmt);
      break;
   default:
      fatal_at(tree_loc(stmt), "cannot lower statement kind %s",
               tree_kind_str(tree_kind(stmt)));
   }
}

static void lower_process(tree_t proc)
{
   vcode_unit_t vu = emit_process(tree_ident(proc));

   const int nstmts = tree_stmts(proc);
   for (int i = 0; i < nstmts; i++)
      lower_stmt(tree_stmt(proc, i));

   if (verbose)
      vcode_dump(vu);

   tree_set_code(proc, vu);
}

static void lower_elab(tree_t unit)
{
   const int nstmts = tree_stmts(unit);
   for (int i = 0; i < nstmts; i++) {
      tree_t s = tree_stmt(unit, i);
      assert(tree_kind(s) == T_PROCESS);
      lower_process(s);
   }
}

void lower_unit(tree_t unit)
{
   builtin_i = ident_new("builtin");

   verbose = (getenv("NVC_LOWER_VERBOSE") != NULL);

   switch (tree_kind(unit)) {
   case T_ELAB:
      lower_elab(unit);
      break;
   default:
      fatal("cannot lower to level unit kind %s to vcode",
            tree_kind_str(tree_kind(unit)));
   }

   vcode_close();
}
