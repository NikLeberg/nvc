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

#ifndef _JIT_FFI_H
#define _JIT_FFI_H

#include "prim.h"
#include "jit/jit.h"

typedef char ffi_type_t;
#define FFI_VOID 'v'
#define FFI_INT8 'b'
#define FFI_INT16 'w'
#define FFI_INT32 'd'
#define FFI_INT64 'q'
#define FFI_UINT8 'B'
#define FFI_UINT16 'W'
#define FFI_UINT32 'D'
#define FFI_FLOAT 'f'
#define FFI_POINTER 'p'
#define FFI_UARRAY 'u'
#define FFI_SIGNAL 's'

typedef union {
   struct {
      ffi_type_t embed[7];
      uint8_t    count;
   };
   const ffi_type_t *ext;
   uint64_t          bits;
} ffi_spec_t;

STATIC_ASSERT(sizeof(ffi_spec_t) == 8);

#define ffi_spec_valid(s) ((s).bits != 0)
#define ffi_spec_get(s, n) ((s).count ? (s).embed[(n)] : s.ext[(n)])
#define ffi_spec_has(s, n) \
   (((s).count == 0 && (s).ext && (s).ext[(n)]) || (n) < (s).count)

// The code generator knows the layout of this struct
typedef struct {
   int64_t left;
   int64_t length;
} ffi_dim_t;

// The code generator knows the layout of this struct
typedef struct _ffi_uarray {
   void      *ptr;
   ffi_dim_t  dims[1];   // Flexible member
} ffi_uarray_t;

#define ffi_array_length(d1) ((d1) ^ ((d1) >> 63))
#define ffi_array_right(d0, d1) ((d0 + d1) + (d1 < 0 ? 2 : -1))
#define ffi_array_dir(d1) (d1 < 0)

// The code generator knows the layout of this struct
typedef struct _ffi_closure {
   jit_handle_t  handle;
   void         *context;
} ffi_closure_t;

ffi_spec_t ffi_spec_new(const ffi_type_t *types, size_t count);

ffi_uarray_t ffi_wrap(void *ptr, int64_t left, int64_t right);
void ffi_return_string(const char *str, jit_scalar_t *args, tlab_t *tlab);
bool ffi_is_integral(ffi_type_t type);
int64_t ffi_widen_int(ffi_type_t type, const void *input);
void ffi_store_int(ffi_type_t type, uint64_t value, void *output);

typedef struct _jit_dll jit_dll_t;

typedef void (*ffi_internal_t)(jit_scalar_t *, tlab_t *);

jit_dll_t *ffi_load_dll(const char *path);
void ffi_unload_dll(jit_dll_t *dll);
void *ffi_find_symbol(jit_dll_t *dll, const char *name);

#endif   // _JIT_FFI_H
