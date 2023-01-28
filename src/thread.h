//
//  Copyright (C) 2021-2022  Nick Gasson
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

#ifndef _THREAD_H
#define _THREAD_H

#include <stdint.h>

#define atomic_add(p, n) __atomic_add_fetch((p), (n), __ATOMIC_SEQ_CST)
#define atomic_fetch_add(p, n) __atomic_fetch_add((p), (n), __ATOMIC_SEQ_CST)
#define atomic_load(p) __atomic_load_n((p), __ATOMIC_SEQ_CST)
#define atomic_store(p, v) __atomic_store_n((p), (v), __ATOMIC_SEQ_CST)
#define atomic_xchg(p, v) __atomic_exchange_n((p), (v), __ATOMIC_SEQ_CST)

#define __atomic_cas(p, old, new)                                       \
   __atomic_compare_exchange_n((p), (old), (new), false,                \
                               __ATOMIC_SEQ_CST, __ATOMIC_RELAXED)

#define atomic_cas(p, old, new) ({                                      \
      typeof(*p) __cmp = (old);                                         \
      __atomic_cas((p), &__cmp, (new));                                 \
    })

#define relaxed_add(p, n) __atomic_add_fetch((p), (n), __ATOMIC_RELAXED)
#define relaxed_fetch_add(p, n) __atomic_fetch_add((p), (n), __ATOMIC_RELAXED)
#define relaxed_load(p) __atomic_load_n((p), __ATOMIC_RELAXED)
#define relaxed_store(p, v) __atomic_store_n((p), (v), __ATOMIC_RELAXED)

#define store_release(p, v) __atomic_store_n((p), (v), __ATOMIC_RELEASE)
#define load_acquire(p) __atomic_load_n((p), __ATOMIC_ACQUIRE)

#define full_barrier() __atomic_thread_fence(__ATOMIC_SEQ_CST)

#define MAX_THREADS 64

typedef struct _nvc_thread nvc_thread_t;

void thread_init(void);
int thread_id(void);
bool thread_attached(void);
void thread_sleep(int usec);

typedef void *(*thread_fn_t)(void *);

nvc_thread_t *thread_create(thread_fn_t fn, void *arg, const char *fmt, ...)
  __attribute__((format(printf, 3, 4)));
void *thread_join(nvc_thread_t *thread);

nvc_thread_t *get_thread(int id);

void spin_wait(void);

typedef int8_t nvc_lock_t;

void nvc_lock(nvc_lock_t *lock);
void nvc_unlock(nvc_lock_t *lock);

#ifdef DEBUG
void assert_lock_held(nvc_lock_t *lock);
#else
#define assert_lock_held(lock)
#endif

void __scoped_unlock(nvc_lock_t **plock);

#define SCOPED_LOCK(lock)                               \
   __attribute__((cleanup(__scoped_unlock), unused))    \
   nvc_lock_t *UNIQUE(__lock) = &(lock);                \
   nvc_lock(&(lock));

typedef struct _workq workq_t;

typedef void (*task_fn_t)(void *, void *);
typedef void (*scan_fn_t)(void *, void *, void *);

workq_t *workq_new(void *context);
void workq_free(workq_t *wq);
void workq_start(workq_t *wq);
void workq_do(workq_t *wq, task_fn_t fn, void *arg);
void workq_drain(workq_t *wq);
void workq_scan(workq_t *wq, scan_fn_t fn, void *arg);
void workq_not_thread_safe(workq_t *wq);

void async_do(task_fn_t fn, void *context, void *arg);
void async_barrier(void);

struct cpu_state;
typedef void (*stop_world_fn_t)(int, struct cpu_state *, void *);

void stop_world(stop_world_fn_t callback, void *arg);
void start_world(void);

#endif  // _THREAD_H
