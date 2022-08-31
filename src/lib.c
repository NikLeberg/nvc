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
#include "common.h"
#include "diag.h"
#include "hash.h"
#include "lib.h"
#include "object.h"
#include "opt.h"
#include "tree.h"
#include "vcode.h"

#include <assert.h>
#include <limits.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <errno.h>
#include <time.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/time.h>

typedef struct _search_path search_path_t;
typedef struct _lib_index   lib_index_t;
typedef struct _lib_list    lib_list_t;
typedef struct _lib_unit    lib_unit_t;

#define INDEX_FILE_MAGIC 0x55225511

struct _lib_unit {
   tree_t        top;
   tree_kind_t   kind;
   bool          dirty;
   bool          error;
   lib_mtime_t   mtime;
   vcode_unit_t  vcode;
   lib_unit_t   *next;
};

struct _lib_index {
   ident_t      name;
   tree_kind_t  kind;
   lib_index_t *next;
};

struct _lib {
   char         *path;
   ident_t       name;
   hash_t       *lookup;
   lib_unit_t   *units;
   lib_index_t  *index;
   lib_mtime_t   index_mtime;
   off_t         index_size;
   int           lock_fd;
   bool          readonly;
};

struct _lib_list {
   lib_t            item;
   lib_list_t      *next;
   vhdl_standard_t  standard;
};

struct _search_path {
   search_path_t *next;
   const char    *path;
};

static lib_t          work = NULL;
static lib_list_t    *loaded = NULL;
static search_path_t *search_paths = NULL;

static text_buf_t *lib_file_path(lib_t lib, const char *name);
static lib_mtime_t lib_stat_mtime(struct stat *st);

static const char *standard_suffix(vhdl_standard_t std)
{
   static const char *ext[] = {
      "87", "93", "00", "02", "08"
   };

   assert(std < ARRAY_LEN(ext));
   return ext[std];
}

static ident_t upcase_name(const char *name)
{
   char *name_copy LOCAL = xstrdup(name);

   char *last_slash = strrchr(name_copy, '/');
   while ((last_slash != NULL) && (*(last_slash + 1) == '\0')) {
      *last_slash = '\0';
      last_slash = strrchr(name_copy, '/');
   }

   char *name_up = (last_slash != NULL) ? last_slash + 1 : name_copy;
   for (char *p = name_up; *p != '\0'; p++)
      *p = toupper((int)*p);

   char *last_dot = strrchr(name_up, '.');
   if (last_dot != NULL)
      *last_dot = '\0';

   if (*name_up == '\0')
      fatal("invalid library name %s", name);

   return ident_new(name_up);
}

static void lib_add_to_index(lib_t lib, ident_t name, tree_kind_t kind)
{
   // Keep the index in sorted order to make library builds reproducible
   lib_index_t **it;
   for (it = &(lib->index);
        *it != NULL && ident_compare((*it)->name, name) < 0;
        it = &((*it)->next))
      ;

   if (*it != NULL && (*it)->name == name) {
      // Already in the index
      (*it)->kind = kind;
   }
   else {
      lib_index_t *new = xmalloc(sizeof(lib_index_t));
      new->name = name;
      new->kind = kind;
      new->next = *it;

      *it = new;
   }
}

static void lib_read_index(lib_t lib)
{
   fbuf_t *f = lib_fbuf_open(lib, "_index", FBUF_IN, FBUF_CS_NONE);
   if (f != NULL) {
      struct stat st;
      if (stat(fbuf_file_name(f), &st) < 0)
         fatal_errno("%s", fbuf_file_name(f));

      const uint32_t magic = read_u32(f);
      if (magic != INDEX_FILE_MAGIC) {
         warnf("ignoring library index %s from an old version of " PACKAGE,
               fbuf_file_name(f));
         return;
      }

      lib->index_mtime = lib_stat_mtime(&st);
      lib->index_size  = st.st_size;

      ident_rd_ctx_t ictx = ident_read_begin(f);
      lib_index_t **insert = &(lib->index);

      const int entries = read_u32(f);
      for (int i = 0; i < entries; i++) {
         ident_t name = ident_read(ictx);
         tree_kind_t kind = read_u16(f);
         assert(kind < T_LAST_TREE_KIND);

         while (*insert && ident_compare((*insert)->name, name) < 0)
            insert = &((*insert)->next);

         if (*insert && (*insert)->name == name) {
            (*insert)->kind = kind;
            insert = &((*insert)->next);
         }
         else {
            lib_index_t *new = xmalloc(sizeof(lib_index_t));
            new->name = name;
            new->kind = kind;
            new->next = *insert;

            *insert = new;
            insert = &(new->next);
         }
      }

      ident_read_end(ictx);
      fbuf_close(f, NULL);
   }
}

static lib_t lib_init(const char *name, const char *rpath, int lock_fd)
{
   lib_t l = xcalloc(sizeof(struct _lib));
   l->name     = upcase_name(name);
   l->index    = NULL;
   l->lock_fd  = lock_fd;
   l->readonly = false;
   l->lookup   = hash_new(128);

   char abspath[PATH_MAX];
   if (rpath == NULL)
      l->path = NULL;
   else if (realpath(rpath, abspath) == NULL)
      l->path = xstrdup(rpath);
   else
      l->path = xstrdup(abspath);

   lib_list_t *el = xcalloc(sizeof(lib_list_t));
   el->item     = l;
   el->next     = loaded;
   el->standard = standard();
   loaded = el;

   if (l->lock_fd == -1 && rpath != NULL) {
      LOCAL_TEXT_BUF lock_path = lib_file_path(l, "_NVC_LIB");

      // Try to open the lock file read-write as this is required for
      // exlusive locking on some NFS implementations
      int mode = O_RDWR;
      if (access(tb_get(lock_path), mode) != 0) {
         if (errno == EACCES || errno == EPERM) {
            mode = O_RDONLY;
            l->readonly = true;
         }
         else
            fatal_errno("access: %s", tb_get(lock_path));
      }

      if ((l->lock_fd = open(tb_get(lock_path), mode)) < 0)
         fatal_errno("open: %s", tb_get(lock_path));

      file_read_lock(l->lock_fd);
   }

   lib_read_index(l);

   if (l->lock_fd != -1)
      file_unlock(l->lock_fd);

   return l;
}

static lib_index_t *lib_find_in_index(lib_t lib, ident_t name)
{
   lib_index_t *it;
   for (it = lib->index;
        (it != NULL) && (it->name != name);
        it = it->next)
      ;

   return it;
}

static lib_unit_t *lib_put_aux(lib_t lib, tree_t unit, bool dirty, bool error,
                               lib_mtime_t mtime, vcode_unit_t vu)
{
   assert(lib != NULL);
   assert(unit != NULL);

   ident_t name = tree_ident(unit);
   assert(ident_until(name, '.') == lib->name);

   bool fresh = false;
   lib_unit_t *where = hash_get(lib->lookup, name);
   if (where == NULL) {
      where = xcalloc(sizeof(lib_unit_t));
      fresh = true;
   }
   else {
      hash_delete(lib->lookup, where->top);

      if (where->vcode != NULL) {
         vcode_unit_unref(where->vcode);
         where->vcode = NULL;
      }
   }

   where->top   = unit;
   where->dirty = dirty;
   where->error = error;
   where->mtime = mtime;
   where->kind  = tree_kind(unit);
   where->vcode = vu;

   if (fresh) {
      lib_unit_t **it;
      for (it = &(lib->units); *it; it = &(*it)->next) {
         assert((*it)->top != unit);
         assert(tree_ident((*it)->top) != name);
      }
      *it = where;
   }

   lib_add_to_index(lib, name, tree_kind(unit));

   hash_put(lib->lookup, name, where);
   hash_put(lib->lookup, unit, where);

   return where;
}

static lib_t lib_find_at(const char *name, const char *path, bool exact)
{
   LOCAL_TEXT_BUF dir = tb_new();
   tb_cat(dir, path);
   tb_cat(dir, DIR_SEP);

   if (!exact) {
      DIR *d = opendir(path);
      if (d == NULL)
         return NULL;

      char *best LOCAL = NULL;
      const char *std_suffix = standard_suffix(standard());

      struct dirent *e;
      while ((e = readdir(d))) {
         if (!isalpha(e->d_name[0]))
            continue;

         const char *dot = strchr(e->d_name, '.');
         if (dot != NULL) {
            if (strncasecmp(name, e->d_name, dot - e->d_name) != 0)
               continue;
            else if (strcmp(dot + 1, std_suffix) != 0)
               continue;
         }
         else {
            if (strcasecmp(name, e->d_name) != 0)
               continue;
            else if (best != NULL)
               continue;
         }

         free(best);
         best = xstrdup(e->d_name);
      }

      closedir(d);

      if (best == NULL)
         return NULL;

      tb_cat(dir, best);
   }
   else if (access(path, F_OK) < 0)
      return NULL;

   char *marker LOCAL = xasprintf("%s" DIR_SEP "_NVC_LIB", tb_get(dir));
   if (access(marker, F_OK) < 0)
      return NULL;

   return lib_init(name, tb_get(dir), -1);
}

static text_buf_t *lib_file_path(lib_t lib, const char *name)
{
   text_buf_t *tb = tb_new();
   tb_printf(tb, "%s" DIR_SEP "%s", lib->path, name);
   return tb;
}

lib_t lib_loaded(ident_t name_i)
{
   if (name_i == well_known(W_WORK) && work != NULL)
      return work;

   for (lib_list_t *it = loaded; it != NULL; it = it->next) {
      if (lib_name(it->item) == name_i && it->standard == standard())
         return it->item;
   }

   return NULL;
}

lib_t lib_new(const char *name, const char *path)
{
   ident_t name_i = upcase_name(name);

   lib_t lib = lib_loaded(name_i);
   if (lib != NULL)
      return lib;
   else if ((lib = lib_find_at(name, path, false)) != NULL)
      return lib;

   const char *last_dot = strrchr(name, '.');
   if (last_dot != NULL) {
      const char *ext = standard_suffix(standard());
      if (strcmp(last_dot + 1, ext) != 0)
         fatal("library directory suffix must be '%s' for this standard", ext);
   }

   for (const char *p = name; *p && p != last_dot; p++) {
      if (!isalnum((int)*p) && (*p != '_'))
         fatal("invalid character '%c' in library name", *p);
   }

   char *lockf LOCAL = xasprintf("%s" DIR_SEP "%s", path, "_NVC_LIB");

   struct stat buf;
   if (stat(path, &buf) == 0) {
      if (S_ISDIR(buf.st_mode)) {
         struct stat sb;
         if (stat(lockf, &sb) != 0)
            fatal("directory %s already exists and is not an NVC library",
                  path);
      }
      else
         fatal("path %s already exists and is not a directory", path);
   }

   make_dir(path);

   int fd = open(lockf, O_CREAT | O_EXCL | O_RDWR, 0777);
   if (fd < 0) {
      // If errno is EEXIST we raced with another process to create the
      // lock file.  Calling into lib_init with fd as -1 will cause it
      // to be opened again without O_CREAT.
      if (errno != EEXIST)
         fatal_errno("lib_new: %s", lockf);
   }
   else {
      file_write_lock(fd);

      const char *marker = PACKAGE_STRING "\n";
      if (write(fd, marker, strlen(marker)) < 0)
         fatal_errno("write: %s", path);
   }

   return lib_init(name, path, fd);
}

lib_t lib_at(const char *path)
{
   for (lib_list_t *it = loaded; it != NULL; it = it->next) {
      if (it->item->path == NULL)
         continue;   // Temporary library
      else if (strncmp(path, it->item->path, strlen(it->item->path)) == 0
               && it->standard == standard())
         return it->item;
   }

   return NULL;
}

lib_t lib_tmp(const char *name)
{
   // For unit tests, avoids creating files
   return lib_init(name, NULL, -1);
}

static void push_path(const char *path)
{
   for (search_path_t *it = search_paths; it != NULL; it = it->next) {
      if (strcmp(it->path, path) == 0)
         return;
   }

   search_path_t *s = xmalloc(sizeof(search_path_t));
   s->next = search_paths;
   s->path = strdup(path);

   search_paths = s;
}

static void lib_default_search_paths(void)
{
   if (search_paths == NULL) {
      push_path(LIBDIR);

      const char *home_env = getenv("HOME");
      if (home_env) {
         char *LOCAL path = xasprintf("%s/.%s/lib", home_env, PACKAGE);
         push_path(path);
      }

      char *LOCAL env_copy = NULL;
      const char *libpath_env = getenv("NVC_LIBPATH");
      if (libpath_env) {
         env_copy = strdup(libpath_env);

         char *path_tok = strtok(env_copy, ":");
         do {
            push_path(path_tok);
         } while ((path_tok = strtok(NULL, ":")));
      }
   }
}

void lib_add_search_path(const char *path)
{
   lib_default_search_paths();
   push_path(path);
}

void lib_add_map(const char *name, const char *path)
{
   lib_t lib = lib_find_at(name, path, true);
   if (lib == NULL)
      warnf("library %s not found at %s", name, path);
}

void lib_print_search_paths(text_buf_t *tb)
{
   lib_default_search_paths();

   for (search_path_t *it = search_paths; it != NULL; it = it->next)
      tb_printf(tb, "\n  %s", it->path);
}

void lib_search_paths_to_diag(diag_t *d)
{
   lib_default_search_paths();

   LOCAL_TEXT_BUF tb = tb_new();
   tb_printf(tb, "library search path contains: ");

   for (search_path_t *it = search_paths; it != NULL; it = it->next)
      tb_printf(tb, "%s%s", it != search_paths ? ", " : "", it->path);

   diag_hint(d, NULL, "%s", tb_get(tb));
   diag_hint(d, NULL, "add additional directories to the search path with "
             "the $bold$-L$$ option");
}

lib_t lib_find(ident_t name_i)
{
   lib_t lib = lib_loaded(name_i);
   if (lib != NULL)
      return lib;

   lib_default_search_paths();

   const char *name_str = istr(name_i);
   for (search_path_t *it = search_paths; it != NULL; it = it->next) {
      if ((lib = lib_find_at(name_str, it->path, false)))
         break;
   }

   return lib;
}

lib_t lib_require(ident_t name)
{
   lib_t lib = lib_find(name);
   if (lib == NULL)
      fatal("required library %s not found", istr(name));

   return lib;
}

FILE *lib_fopen(lib_t lib, const char *name, const char *mode)
{
   assert(lib != NULL);
   LOCAL_TEXT_BUF path = lib_file_path(lib, name);
   return fopen(tb_get(path), mode);
}

fbuf_t *lib_fbuf_open(lib_t lib, const char *name,
                      fbuf_mode_t mode, fbuf_cs_t csum)
{
   assert(lib != NULL);
   if (lib->path == NULL)
      return NULL;   // Temporary library for unit test
   else {
      LOCAL_TEXT_BUF path = lib_file_path(lib, name);
      return fbuf_open(tb_get(path), mode, csum);
   }
}

void lib_free(lib_t lib)
{
   assert(lib != NULL);
   assert(lib != work);

   if (lib->lock_fd != -1)
      close(lib->lock_fd);

   for (lib_list_t **it = &loaded; *it; it = &((*it)->next)) {
      if ((*it)->item == lib) {
         lib_list_t *tmp = *it;
         *it = (*it)->next;
         free(tmp);
         break;
      }
   }

   while (lib->index) {
      lib_index_t *tmp = lib->index->next;
      free(lib->index);
      lib->index = tmp;
   }

   for (lib_unit_t *lu = lib->units, *tmp; lu; lu = tmp) {
      tmp = lu->next;
      free(lu);
   }
   hash_free(lib->lookup);

   free(lib->path);
   free(lib);
}

void lib_destroy(lib_t lib)
{
   // This is convenience function for testing: remove all
   // files associated with a library

   assert(lib != NULL);

   close(lib->lock_fd);
   lib->lock_fd = -1;

   DIR *d = opendir(lib->path);
   if (d == NULL) {
      perror("opendir");
      return;
   }

   char buf[PATH_MAX];
   struct dirent *e;
   while ((e = readdir(d))) {
      if (e->d_name[0] != '.') {
         checked_sprintf(buf, sizeof(buf), "%s" DIR_SEP "%s",
                         lib->path, e->d_name);
         if (unlink(buf) < 0)
            perror("unlink");
      }
   }

   closedir(d);

   if (rmdir(lib->path) < 0)
      perror("rmdir");
}

lib_t lib_work(void)
{
   assert(work != NULL);
   return work;
}

void lib_set_work(lib_t lib)
{
   work = lib;
}

const char *lib_path(lib_t lib)
{
   assert(lib != NULL);
   return lib->path;
}

static lib_mtime_t lib_time_to_usecs(time_t t)
{
   return (lib_mtime_t)t * 1000 * 1000;
}

static lib_mtime_t lib_mtime_now(void)
{
   struct timeval tv;
   if (gettimeofday(&tv, NULL) != 0)
      fatal_errno("gettimeofday");

   return ((lib_mtime_t)tv.tv_sec * 1000000) + tv.tv_usec;
}

void lib_put(lib_t lib, tree_t unit)
{
   lib_put_aux(lib, unit, true, false, lib_mtime_now(), NULL);
}

void lib_put_error(lib_t lib, tree_t unit)
{
   lib_put_aux(lib, unit, true, true, lib_mtime_now(), NULL);
}

static lib_unit_t *lib_find_unit(lib_t lib, tree_t unit)
{
   lib_unit_t *lu = hash_get(lib->lookup, unit);
   if (lu == NULL)
      fatal_trace("unit %s not stored in library %s", istr(tree_ident(unit)),
                  istr(lib->name));

   return lu;
}

bool lib_contains(lib_t lib, tree_t unit)
{
   return hash_get(lib->lookup, unit) != NULL;
}

void lib_put_vcode(lib_t lib, tree_t unit, vcode_unit_t vu)
{
   lib_unit_t *where = lib_find_unit(lib, unit);

   if (where->vcode != NULL)
      fatal_trace("vcode already stored for %s", istr(tree_ident(unit)));

   where->vcode = vu;
   where->dirty = true;
}

vcode_unit_t lib_get_vcode(lib_t lib, tree_t unit)
{
   lib_unit_t *where = lib_find_unit(lib, unit);

   if (where->vcode == NULL)
      fatal_trace("vcode not stored for %s", istr(tree_ident(unit)));

   return where->vcode;
}

static lib_mtime_t lib_stat_mtime(struct stat *st)
{
   lib_mtime_t mt = lib_time_to_usecs(st->st_mtime);
#if defined HAVE_STRUCT_STAT_ST_MTIMESPEC_TV_NSEC
   mt += st->st_mtimespec.tv_nsec / 1000;
#elif defined HAVE_STRUCT_STAT_ST_MTIM_TV_NSEC
   mt += st->st_mtim.tv_nsec / 1000;
#endif
   return mt;
}

static lib_unit_t *lib_read_unit(lib_t lib, const char *fname)
{
   fbuf_t *f = lib_fbuf_open(lib, fname, FBUF_IN, FBUF_CS_ADLER32);

   ident_rd_ctx_t ident_ctx = ident_read_begin(f);
   loc_rd_ctx_t *loc_ctx = loc_read_begin(f);

   vcode_unit_t vu = NULL;
   tree_t top = NULL;

   char tag;
   while ((tag = read_u8(f))) {
      switch (tag) {
      case 'T':
         top = tree_read(f, lib_get_qualified, ident_ctx, loc_ctx);
         break;
      case 'V':
         vu = vcode_read(f, ident_ctx, loc_ctx);
         break;
      default:
         fatal_trace("unhandled tag %c in %s", tag, fname);
      }
   }

   loc_read_end(loc_ctx);
   ident_read_end(ident_ctx);

   uint32_t checksum;
   fbuf_close(f, &checksum);

   if (top == NULL)
      fatal_trace("%s did not contain tree", fname);

   arena_set_checksum(tree_arena(top), checksum);

   LOCAL_TEXT_BUF path = lib_file_path(lib, fname);

   struct stat st;
   if (stat(tb_get(path), &st) < 0)
      fatal_errno("%s", fname);

   lib_mtime_t mt = lib_stat_mtime(&st);

   return lib_put_aux(lib, top, false, false, mt, vu);
}

static lib_unit_t *lib_get_aux(lib_t lib, ident_t ident)
{
   assert(lib != NULL);

   // Handle aliased library names and names without the library
   ident_t uname = ident, lname = ident_walk_selected(&uname);
   if (uname == NULL)
      ident = ident_prefix(lib->name, lname, '.');
   else if (lname != lib->name)
      ident = ident_prefix(lib->name, uname, '.');

   lib_unit_t *lu = hash_get(lib->lookup, ident);
   if (lu != NULL)
      return lu;

   if (lib->path == NULL)   // Temporary library
      return NULL;

   assert(lib->lock_fd != -1);   // Should not be called in unit tests
   file_read_lock(lib->lock_fd);

   // Otherwise search in the filesystem
   DIR *d = opendir(lib->path);
   if (d == NULL)
      fatal("%s: %s", lib->path, strerror(errno));

   const char *search = istr(ident);
   struct dirent *e;
   while ((e = readdir(d))) {
      if (strcmp(e->d_name, search) == 0) {
         lu = lib_read_unit(lib, e->d_name);
         break;
      }
   }

   closedir(d);
   file_unlock(lib->lock_fd);

   if (lu == NULL && lib_find_in_index(lib, ident) != NULL)
      fatal("library %s corrupt: unit %s present in index but missing "
            "on disk", istr(lib->name), istr(ident));

   if (lu != NULL && !opt_get_int(OPT_IGNORE_TIME)) {
      const loc_t *loc = tree_loc(lu->top);

      bool stale = false;
      struct stat st;
      if (stat(loc_file_str(loc), &st) == 0)
         stale = (lu->mtime < lib_stat_mtime(&st));

      if (stale) {
         diag_t *d = diag_new(DIAG_WARN, NULL);
         diag_printf(d, "design unit %s is older than its source file "
                     "%s and should be reanalysed",
                     istr(ident), loc_file_str(loc));
         diag_hint(d, NULL, "you can use the $bold$--ignore-time$$ option "
                   "to skip this check");
         diag_emit(d);
      }
   }

   return lu;
}

static void lib_ensure_writable(lib_t lib)
{
   if (lib->readonly)
      fatal("cannot write to read-only library %s", istr(lib->name));
}

lib_mtime_t lib_mtime(lib_t lib, ident_t ident)
{
   lib_unit_t *lu = lib_get_aux(lib, ident);
   assert(lu != NULL);
   return lu->mtime;
}

bool lib_stat(lib_t lib, const char *name, lib_mtime_t *mt)
{
   struct stat buf;
   LOCAL_TEXT_BUF path = lib_file_path(lib, name);
   if (stat(tb_get(path), &buf) == 0) {
      if (mt != NULL)
         *mt = lib_stat_mtime(&buf);
      return true;
   }
   else
      return false;
}

tree_t lib_get(lib_t lib, ident_t ident)
{
   lib_unit_t *lu = lib_get_aux(lib, ident);
   if (lu != NULL) {
      if (lu->error)
         fatal_at(tree_loc(lu->top), "design unit %s was analysed with errors",
                  istr(tree_ident(lu->top)));
      else
         return lu->top;
   }
   else
      return NULL;
}

tree_t lib_get_allow_error(lib_t lib, ident_t ident, bool *error)
{
   lib_unit_t *lu = lib_get_aux(lib, ident);
   if (lu != NULL) {
      *error = lu->error;
      return lu->top;
   }
   else {
      *error = false;
      return NULL;
   }
}

tree_t lib_get_qualified(ident_t qual)
{
   ident_t lname = ident_until(qual, '.');
   if (lname == NULL)
      return NULL;

   lib_t lib = lib_find(lname);
   if (lib == NULL)
      return NULL;

   return lib_get(lib, qual);
}

ident_t lib_name(lib_t lib)
{
   assert(lib != NULL);
   return lib->name;
}

static void lib_save_unit(lib_t lib, lib_unit_t *unit)
{
   const char *name = istr(tree_ident(unit->top));
   fbuf_t *f = lib_fbuf_open(lib, name, FBUF_OUT, FBUF_CS_ADLER32);
   if (f == NULL)
      fatal("failed to create %s in library %s", name, istr(lib->name));

   write_u8('T', f);

   ident_wr_ctx_t ident_ctx = ident_write_begin(f);
   loc_wr_ctx_t *loc_ctx = loc_write_begin(f);

   tree_write(unit->top, f, ident_ctx, loc_ctx);

   if (unit->vcode != NULL) {
      write_u8('V', f);
      vcode_write(unit->vcode, f, ident_ctx, loc_ctx);
   }

   write_u8('\0', f);

   loc_write_end(loc_ctx);
   ident_write_end(ident_ctx);

   uint32_t checksum;
   fbuf_close(f, &checksum);

   arena_set_checksum(tree_arena(unit->top), checksum);

   assert(unit->dirty);
   unit->dirty = false;
}

void lib_save(lib_t lib)
{
   assert(lib != NULL);

   assert(lib->lock_fd != -1);   // Should not be called in unit tests
   lib_ensure_writable(lib);
   file_write_lock(lib->lock_fd);

   for (lib_unit_t *lu = lib->units; lu; lu = lu->next) {
      if (lu->dirty) {
         if (lu->error)
            fatal_trace("attempting to save unit %s with errors",
                        istr(tree_ident(lu->top)));
         else
            lib_save_unit(lib, lu);
      }
   }

   LOCAL_TEXT_BUF index_path = lib_file_path(lib, "_index");
   struct stat st;
   if (stat(tb_get(index_path), &st) == 0
       && (lib_stat_mtime(&st) != lib->index_mtime
           || st.st_size != lib->index_size)) {
      // Library was updated concurrently: re-read the index while we
      // have the lock
      lib_read_index(lib);
   }

   int index_sz = lib_index_size(lib);

   fbuf_t *f = lib_fbuf_open(lib, "_index", FBUF_OUT, FBUF_CS_NONE);
   if (f == NULL)
      fatal_errno("failed to create library %s index", istr(lib->name));

   write_u32(INDEX_FILE_MAGIC, f);

   ident_wr_ctx_t ictx = ident_write_begin(f);

   write_u32(index_sz, f);
   for (lib_index_t *it = lib->index; it != NULL; it = it->next) {
      ident_write(it->name, ictx);
      write_u16(it->kind, f);
   }

   ident_write_end(ictx);
   fbuf_close(f, NULL);

   if (stat(tb_get(index_path), &st) != 0)
      fatal_errno("stat: %s", tb_get(index_path));
   lib->index_mtime = lib_stat_mtime(&st);
   lib->index_size  = st.st_size;

   file_unlock(lib->lock_fd);
}

int lib_index_kind(lib_t lib, ident_t ident)
{
   lib_index_t *it = lib_find_in_index(lib, ident);
   return it != NULL ? it->kind : T_LAST_TREE_KIND;
}

void lib_walk_index(lib_t lib, lib_index_fn_t fn, void *context)
{
   assert(lib != NULL);

   lib_index_t *it;
   for (it = lib->index; it != NULL; it = it->next)
      (*fn)(lib, it->name, it->kind, context);
}

void lib_for_all(lib_walk_fn_t fn, void *ctx)
{
   for (lib_list_t *it = loaded; it != NULL; it = it->next) {
      if (!(*fn)(it->item, ctx))
         break;
   }
}

unsigned lib_index_size(lib_t lib)
{
   assert(lib != NULL);

   unsigned n = 0;
   for (lib_index_t *it = lib->index; it != NULL; it = it->next)
      n++;

   return n;
}

void lib_realpath(lib_t lib, const char *name, char *buf, size_t buflen)
{
   assert(lib != NULL);

   if (name)
      checked_sprintf(buf, buflen, "%s" DIR_SEP "%s", lib->path, name);
   else
      strncpy(buf, lib->path, buflen);
}

void lib_mkdir(lib_t lib, const char *name)
{
   LOCAL_TEXT_BUF path = lib_file_path(lib, name);
   make_dir(tb_get(path));
}

void lib_delete(lib_t lib, const char *name)
{
   assert(lib != NULL);
   LOCAL_TEXT_BUF path = lib_file_path(lib, name);
   if (remove(tb_get(path)) != 0 && errno != ENOENT)
      fatal_errno("remove: %s", name);
}
