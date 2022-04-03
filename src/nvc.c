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

#include "lib.h"
#include "util.h"
#include "array.h"
#include "phase.h"
#include "common.h"
#include "rt/rt.h"
#include "rt/cover.h"

#include <unistd.h>
#include <getopt.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <assert.h>

const char *copy_string =
   "Copyright (C) 2011-2022  Nick Gasson\n"
   "This program comes with ABSOLUTELY NO WARRANTY. This is free software, "
   "and\nyou are welcome to redistribute it under certain conditions. See "
   "the GNU\nGeneral Public Licence for details.";
const char *version_string =
   PACKAGE_STRING " (Using LLVM " LLVM_VERSION ")" DEBUG_ONLY(" [debug]");

static ident_t top_level = NULL;
static char *top_level_orig = NULL;

static int process_command(int argc, char **argv);
static int parse_int(const char *str);

static ident_t to_unit_name(const char *str)
{
   char *name = xstrdup(str);
   for (char *p = name; *p; p++)
      *p = toupper((int)*p);

   ident_t i = ident_prefix(lib_name(lib_work()),
                            ident_new(name), '.');
   free(name);
   return i;
}

static unsigned parse_relax(const char *str)
{
   char *copy = xstrdup(str);
   assert(copy);

   unsigned mask = 0;

   char *token = strtok(copy, ",");
   while (token != NULL) {
      if (strcmp(token, "prefer-explicit") == 0)
         mask |= RELAX_PREFER_EXPLICT;
      else if (strcmp(token, "locally-static") == 0)
         mask |= RELAX_LOCALLY_STATIC;
      else if (strcmp(token, "generic-static") == 0) {
         warnf("relax option 'generic-static' is deprecated: use "
               "'locally-static' instead");
         mask |= RELAX_LOCALLY_STATIC;
      }
      else if (strcmp(token, "universal-bound") == 0)
         mask |= RELAX_UNIVERSAL_BOUND;
      else if (strcmp(token, "pure-files") == 0)
         mask |= RELAX_PURE_FILES;
      else if (strcmp(token, "impure") == 0)
         mask |= RELAX_IMPURE;
      else
         fatal("invalid relax option '%s'", token);

      token = strtok(NULL, ",");
   }

   free(copy);
   return mask;
}

static int scan_cmd(int start, int argc, char **argv)
{
   const char *commands[] = {
      "-a", "-e", "-r", "--dump", "--make", "--syntax", "--list"
   };

   for (int i = start; i < argc; i++) {
      for (size_t j = 0; j < ARRAY_LEN(commands); j++) {
         if (strcmp(argv[i], commands[j]) == 0)
            return i;
      }
   }

   return argc;
}

static void bad_option(const char *what, char **argv)
{
   if (optopt == 0)
      fatal("unrecognised %s option %s", what, argv[optind - 1]);
   else
      fatal("unrecognised %s option -%c", what, optopt);
}

static int analyse(int argc, char **argv)
{
   static struct option long_options[] = {
      { "bootstrap",       no_argument,       0, 'b' },
      { "error-limit",     required_argument, 0, 'l' },
      { "dump-json",       required_argument, 0, 'j' },
      { "dump-llvm",       no_argument,       0, 'D' },
      { "dump-vcode",      optional_argument, 0, 'v' },
      { "prefer-explicit", no_argument,       0, 'p' },   // DEPRECATED
      { "relax",           required_argument, 0, 'R' },
      { 0, 0, 0, 0 }
   };

   const int next_cmd = scan_cmd(2, argc, argv);
   int c, index = 0;
   const char *spec = "";

   opt_set_int("error-limit", 20);

   while ((c = getopt_long(next_cmd, argv, spec, long_options, &index)) != -1) {
      switch (c) {
      case 0:
         // Set a flag
         break;
      case '?':
         bad_option("analyse", argv);
      case 'b':
         opt_set_int("bootstrap", 1);
         break;
      case 'D':
         opt_set_int("dump-llvm", 1);
         break;
      case 'v':
         opt_set_str("dump-vcode", optarg ?: "");
         break;
      case 'j':
         opt_set_str("dump-json", optarg ?: "");
         break;
      case 'p':
         warnf("the --prefer-explict option is deprecated: use "
               "--relax=prefer-explict instead");
         set_relax_rules(RELAX_PREFER_EXPLICT);
         break;
      case 'R':
         set_relax_rules(parse_relax(optarg));
         break;
      case 'l':
         opt_set_int("error-limit", parse_int(optarg));
         break;
      default:
         abort();
      }
   }

   SCOPED_A(tree_t) units = AINIT;

   lib_t work = lib_work();

   for (int i = optind; i < next_cmd; i++) {
      input_from_file(argv[i]);

      tree_t unit;
      while ((unit = parse())) {
         if (sem_check(unit) && error_count() == 0) {
            APUSH(units, unit);

            simplify_local(unit);
            bounds_check(unit);

            if (error_count() == 0 && unit_needs_cgen(unit)) {
               vcode_unit_t vu = lower_unit(unit, NULL);
               lib_put_vcode(work, unit, vu);
            }
         }
      }
   }

   if (error_count() > 0)
      return EXIT_FAILURE;

   if (opt_get_str("dump-json"))
      dump_json(units.items, units.count, opt_get_str("dump-json"));

   lib_save(work);

   argc -= next_cmd - 1;
   argv += next_cmd - 1;

   return argc > 1 ? process_command(argc, argv) : EXIT_SUCCESS;
}

static void parse_generic(const char *str)
{
   char *copy LOCAL = xstrdup(str);

   char *split = strchr(copy, '=');
   if (split == NULL || *(split + 1) == '\0' || *copy == '\0')
      fatal("invalid generic specification '%s' (use -gNAME=VALUE)", str);

   *split = '\0';

   for (char *p = copy; *p != '\0'; p++)
      *p = toupper((int)*p);

   elab_set_generic(copy, split + 1);
}

static void set_top_level(char **argv, int next_cmd)
{
   if (optind == next_cmd) {
      if (top_level == NULL)
         fatal("missing top-level unit name");
   }
   else {
      free(top_level_orig);
      top_level_orig = xstrdup(argv[optind]);
      top_level = to_unit_name(top_level_orig);
   }
}

static int elaborate(int argc, char **argv)
{
   static struct option long_options[] = {
      { "disable-opt", no_argument,       0, 'o' },    // DEPRECATED
      { "dump-llvm",   no_argument,       0, 'd' },
      { "dump-vcode",  optional_argument, 0, 'v' },
      { "native",      no_argument,       0, 'n' },    // DEPRECATED
      { "cover",       no_argument,       0, 'c' },
      { "verbose",     no_argument,       0, 'V' },
      { 0, 0, 0, 0 }
   };

   const int next_cmd = scan_cmd(2, argc, argv);
   int c, index = 0;
   const char *spec = "Vg:O:";
   while ((c = getopt_long(next_cmd, argv, spec, long_options, &index)) != -1) {
      switch (c) {
      case 'o':
         warnf("The '--disable-opt' option is deprecated: use '-O0' instead");
         opt_set_int("optimise", 0);
         break;
      case 'O':
         {
            char *eptr;
            const int level = strtoul(optarg, &eptr, 10);
            if (level > 3)
               fatal("Invalid optimisation level %s", optarg);
            opt_set_int("optimise", level);
         }
         break;
      case 'd':
         opt_set_int("dump-llvm", 1);
         break;
      case 'v':
         opt_set_str("dump-vcode", optarg ?: "");
         break;
      case 'n':
         warnf("--native is now a global option: place before the -e command");
         break;
      case 'c':
         opt_set_int("cover", 1);
         break;
      case 'V':
         opt_set_int("verbose", 1);
         break;
      case 'g':
         parse_generic(optarg);
         break;
      case 0:
         // Set a flag
         break;
      case '?':
         bad_option("elaborate", argv);
      default:
         abort();
      }
   }

   set_top_level(argv, next_cmd);

   progress("initialising");

   tree_t unit = lib_get(lib_work(), top_level);
   if (unit == NULL)
      fatal("cannot find unit %s in library %s",
            istr(top_level), istr(lib_name(lib_work())));

   progress("loading top-level unit");

   tree_t top = elab(unit);
   if (top == NULL)
      return EXIT_FAILURE;

   progress("elaborating design");

   cover_tagging_t *cover = NULL;
   if (opt_get_int("cover")) {
      cover = cover_tag(top);
      progress("generating coverage information");
   }

   vcode_unit_t vu = lower_unit(top, cover);
   progress("generating intermediate code");

   eopt_build(top);
   progress("optimising design");

   if (error_count() > 0)
      return EXIT_FAILURE;

   lib_save(lib_work());
   progress("saving library");

   cgen(top, vu, cover);

   argc -= next_cmd - 1;
   argv += next_cmd - 1;

   return argc > 1 ? process_command(argc, argv) : EXIT_SUCCESS;
}

static uint64_t parse_time(const char *str)
{
   char     unit[4];
   unsigned base;
   uint64_t mult = 1;

   if (sscanf(str, "%u%3s", &base, unit) != 2)
      fatal("invalid time format: %s", str);

   if      (strcmp(unit, "fs") == 0)  mult = 1;
   else if (strcmp(unit, "ps") == 0)  mult = 1000;
   else if (strcmp(unit, "ns") == 0)  mult = 1000000;
   else if (strcmp(unit, "us") == 0)  mult = 1000000000;
   else if (strcmp(unit, "ms") == 0)  mult = 1000000000000;
   else if (strcmp(unit, "sec") == 0) mult = 1000000000000000;
   else
      fatal("invalid unit: %s", unit);

   return base * mult;
}

static int parse_int(const char *str)
{
   char *eptr = NULL;
   int n = strtol(str, &eptr, 0);
   if ((eptr == NULL) || (*eptr != '\0'))
      fatal("invalid integer: %s", str);
   return n;
}

static bool parse_on_off(const char *str)
{
   if (strcasecmp(str, "on") == 0)
      return true;
   else if (strcasecmp(str, "off") == 0)
      return false;

   fatal("specifiy 'on' or 'off' instead of '%s'", str);
}

static rt_severity_t parse_severity(const char *str)
{
   if (strcasecmp(str, "note") == 0)
      return SEVERITY_NOTE;
   else if (strcasecmp(str, "warning") == 0)
      return SEVERITY_WARNING;
   else if (strcasecmp(str, "error") == 0)
      return SEVERITY_ERROR;
   else if (strcasecmp(str, "failure") == 0)
      return SEVERITY_FAILURE;
   else
      fatal("invalid severity level: %s", str);
}

static int run(int argc, char **argv)
{
   static struct option long_options[] = {
      { "trace",         no_argument,       0, 't' },
      { "profile",       no_argument,       0, 'p' },
      { "stop-time",     required_argument, 0, 's' },
      { "stats",         no_argument,       0, 'S' },
      { "wave",          optional_argument, 0, 'w' },
      { "stop-delta",    required_argument, 0, 'd' },
      { "format",        required_argument, 0, 'f' },
      { "include",       required_argument, 0, 'i' },
      { "ieee-warnings", required_argument, 0, 'I' },
      { "exclude",       required_argument, 0, 'e' },
      { "exit-severity", required_argument, 0, 'x' },
      { "dump-arrays",   no_argument,       0, 'a' },
#if ENABLE_VHPI
      { "load",          required_argument, 0, 'l' },
      { "vhpi-trace",    no_argument,       0, 'T' },
#endif
      { 0, 0, 0, 0 }
   };

   wave_output_t wave_fmt = WAVE_OUTPUT_FST;
   uint64_t      stop_time = TIME_HIGH;
   const char   *wave_fname = NULL;
   const char   *vhpi_plugins = NULL;

   static bool have_run = false;
   if (have_run)
      fatal("multiple run commands are not supported");

   have_run = true;

   const int next_cmd = scan_cmd(2, argc, argv);

   int c, index = 0;
   const char *spec = "w::l:";
   while ((c = getopt_long(next_cmd, argv, spec, long_options, &index)) != -1) {
      switch (c) {
      case 0:
         // Set a flag
         break;
      case '?':
         bad_option("run", argv);
      case 't':
         opt_set_int("rt_trace_en", 1);
         break;
      case 'p':
         opt_set_int("rt_profile", 1);
         break;
      case 'T':
         opt_set_int("vhpi_trace_en", 1);
         break;
      case 's':
         stop_time = parse_time(optarg);
         break;
      case 'f':
         if (strcmp(optarg, "vcd") == 0)
            wave_fmt = WAVE_OUTPUT_VCD;
         else if (strcmp(optarg, "fst") == 0)
            wave_fmt = WAVE_OUTPUT_FST;
         else
            fatal("invalid waveform format: %s", optarg);
         break;
      case 'S':
         opt_set_int("rt-stats", 1);
         break;
      case 'w':
         if (optarg == NULL)
            wave_fname = "";
         else
            wave_fname = optarg;
         break;
      case 'd':
         opt_set_int("stop-delta", parse_int(optarg));
         break;
      case 'i':
         wave_include_glob(optarg);
         break;
      case 'e':
         wave_exclude_glob(optarg);
         break;
      case 'l':
         vhpi_plugins = optarg;
         break;
      case 'x':
         rt_set_exit_severity(parse_severity(optarg));
         break;
      case 'I':
         opt_set_int("ieee-warnings", parse_on_off(optarg));
         break;
      case 'a':
         opt_set_int("dump-arrays", 1);
         break;
      default:
         abort();
      }
   }

   set_top_level(argv, next_cmd);

   ident_t ename = ident_prefix(top_level, ident_new("elab"), '.');
   tree_t top = lib_get_check_stale(lib_work(), ename);
   if (top == NULL)
      fatal("%s not elaborated", istr(top_level));

   e_node_t e = lib_get_eopt(lib_work(), top);

   if (wave_fname != NULL) {
      const char *name_map[] = { "FST", "VCD" };
      const char *ext_map[]  = { "fst", "vcd" };
      char *tmp LOCAL = NULL;

      if (*wave_fname == '\0') {
         tmp = xasprintf("%s.%s", top_level_orig, ext_map[wave_fmt]);
         wave_fname = tmp;
         notef("writing %s waveform data to %s", name_map[wave_fmt], tmp);
      }

      wave_include_file(argv[optind]);
      wave_init(wave_fname, top, wave_fmt);
   }

   rt_start_of_tool(top, e);

   if (vhpi_plugins != NULL)
      vhpi_load_plugins(top, vhpi_plugins);

   rt_restart(e);
   rt_run_sim(stop_time);
   rt_end_of_tool(top, e);

   argc -= next_cmd - 1;
   argv += next_cmd - 1;

   return argc > 1 ? process_command(argc, argv) : EXIT_SUCCESS;
}

static int make_cmd(int argc, char **argv)
{
   static struct option long_options[] = {
      { "deps-only", no_argument, 0, 'd' },
      { "posix",     no_argument, 0, 'p' },
      { 0, 0, 0, 0 }
   };

   const int next_cmd = scan_cmd(2, argc, argv);
   int c, index = 0;
   const char *spec = "";
   while ((c = getopt_long(next_cmd, argv, spec, long_options, &index)) != -1) {
      switch (c) {
      case 0:
         // Set a flag
         break;
      case '?':
         bad_option("make", argv);
      case 'd':
         opt_set_int("make-deps-only", 1);
         break;
      case 'p':
         opt_set_int("make-posix", 1);
         break;
      default:
         abort();
      }
   }

   const int count = next_cmd - optind;
   tree_t *targets = xmalloc_array(count, sizeof(tree_t));

   lib_t work = lib_work();

   for (int i = optind; i < next_cmd; i++) {
      ident_t name = to_unit_name(argv[i]);
      ident_t elab = ident_prefix(name, ident_new("elab"), '.');
      if ((targets[i - optind] = lib_get(work, elab)) == NULL) {
         if ((targets[i - optind] = lib_get(work, name)) == NULL)
            fatal("cannot find unit %s in library %s",
                  istr(name), istr(lib_name(work)));
      }
   }

   make(targets, count, stdout);

   argc -= next_cmd - 1;
   argv += next_cmd - 1;

   return argc > 1 ? process_command(argc, argv) : EXIT_SUCCESS;
}

static void list_walk_fn(lib_t lib, ident_t ident, int kind, void *context)
{
   const char *pretty = "???";
   switch (kind) {
   case T_ELAB: pretty = "Elaborated"; break;
   case T_ARCH: pretty = "Architecture"; break;
   case T_ENTITY: pretty = "Entity"; break;
   case T_PACKAGE: pretty = "Package"; break;
   case T_PACK_BODY: pretty = "Package body"; break;
   case T_CONFIGURATION: pretty = "Configuration"; break;
   }

   printf("%-30s  : %s\n", istr(ident), pretty);
}

static int list_cmd(int argc, char **argv)
{
   static struct option long_options[] = {
      { 0, 0, 0, 0 }
   };

   const int next_cmd = scan_cmd(2, argc, argv);
   int c, index = 0;
   const char *spec = "";
   while ((c = getopt_long(next_cmd, argv, spec, long_options, &index)) != -1) {
      switch (c) {
      case 0:
         // Set a flag
         break;
      case '?':
         bad_option("list", argv);
      default:
         abort();
      }
   }

   lib_walk_index(lib_work(), list_walk_fn, NULL);

   argc -= next_cmd - 1;
   argv += next_cmd - 1;

   return argc > 1 ? process_command(argc, argv) : EXIT_SUCCESS;
}

static int syntax_cmd(int argc, char **argv)
{
   static struct option long_options[] = {
      { 0, 0, 0, 0 }
   };

   const int next_cmd = scan_cmd(2, argc, argv);
   int c, index = 0;
   const char *spec = "";
   while ((c = getopt_long(next_cmd, argv, spec, long_options, &index)) != -1) {
      switch (c) {
      case 0:
         // Set a flag
         break;
      case '?':
         bad_option("syntax", argv);
      default:
         abort();
      }
   }

   for (int i = optind; i < next_cmd; i++) {
      input_from_file(argv[i]);
      while (parse())
         ;
   }

   if (error_count() > 0)
      return EXIT_FAILURE;

   argc -= next_cmd - 1;
   argv += next_cmd - 1;

   return argc > 1 ? process_command(argc, argv) : EXIT_SUCCESS;
}

static int dump_cmd(int argc, char **argv)
{
   static struct option long_options[] = {
      { "elab", no_argument, 0, 'E' },
      { "body", no_argument, 0, 'b' },
      { 0, 0, 0, 0 }
   };

   const int next_cmd = scan_cmd(2, argc, argv);
   bool add_elab = false, add_body = false;
   int c, index = 0;
   const char *spec = "Eb";
   while ((c = getopt_long(next_cmd, argv, spec, long_options, &index)) != -1) {
      switch (c) {
      case 0:
         // Set a flag
         break;
      case '?':
         bad_option("dump", argv);
      case 'E':
         add_elab = true;
         break;
      case 'b':
         add_body = true;
         break;
      default:
         abort();
      }
   }

   set_top_level(argv, next_cmd);

   ident_t name = top_level;
   if (add_elab)
      name = ident_prefix(name, ident_new("elab"), '.');
   else if (add_body)
      name = ident_prefix(name, ident_new("body"), '-');

   tree_t top = lib_get(lib_work(), name);
   if (top == NULL)
      fatal("%s not analysed", istr(name));

   dump(top);

   argc -= next_cmd - 1;
   argv += next_cmd - 1;

   return argc > 1 ? process_command(argc, argv) : EXIT_SUCCESS;
}

static void set_default_opts(void)
{
   opt_set_int("rt-stats", 0);
   opt_set_int("rt_trace_en", 0);
   opt_set_int("vhpi_trace_en", 0);
   opt_set_int("dump-llvm", 0);
   opt_set_int("optimise", 2);
   opt_set_int("bootstrap", 0);
   opt_set_str("dump-json", NULL);
   opt_set_int("cover", 0);
   opt_set_int("stop-delta", 1000);
   opt_set_int("unit-test", 0);
   opt_set_int("make-deps-only", 0);
   opt_set_int("make-posix", 0);
   opt_set_str("dump-vcode", NULL);
   opt_set_int("relax", 0);
   opt_set_int("ignore-time", 0);
   opt_set_int("force-init", 0);
   opt_set_int("verbose", 0);
   opt_set_int("rt_profile", 0);
   opt_set_int("synthesis", 0);
   opt_set_int("missing-body", 1);
   opt_set_int("error-limit", -1);
   opt_set_int("ieee-warnings", 1);
   opt_set_int("arena-size", 1 << 24);
   opt_set_int("dump-arrays", 0);
}

static void usage(void)
{
   printf("Usage: %s [OPTION]... COMMAND [OPTION]...\n"
          "\n"
          "COMMAND is one of:\n"
          " -a [OPTION]... FILE...\t\tAnalyse FILEs into work library\n"
          " -e [OPTION]... UNIT\t\tElaborate and generate code for UNIT\n"
          " -r [OPTION]... UNIT\t\tExecute previously elaborated UNIT\n"
          " --dump [OPTION]... UNIT\tPrint out previously analysed UNIT\n"
          " --list\t\t\t\tPrint all units in the library\n"
          " --make [OPTION]... [UNIT]...\tGenerate makefile to rebuild UNITs\n"
          " --syntax FILE...\t\tCheck FILEs for syntax errors only\n"
          "\n"
          "Global options may be placed before COMMAND:\n"
          "     --force-init\tCreate a library in an existing directory\n"
          " -h, --help\t\tDisplay this message and exit\n"
          "     --ignore-time\tSkip source file timestamp check\n"
          " -L PATH\t\tAdd PATH to library search paths\n"
          " -M SIZE\t\tDesign unit heap space limit in bytes\n"
          "     --map=LIB:PATH\tMap library LIB to PATH\n"
          "     --messages=STYLE\tSelect full or compact message format\n"
          "     --native\t\tGenerate native code shared library\n"
          "     --std=REV\t\tVHDL standard revision to use\n"
          " -v, --version\t\tDisplay version and copyright information\n"
          "     --work=NAME\tUse NAME as the work library\n"
          "\n"
          "Analyse options:\n"
          "     --bootstrap\tAllow compilation of STANDARD package\n"
          "     --error-limit=NUM\tStop after NUM errors\n"
          "     --relax=RULES\tDisable certain pedantic rule checks\n"
          "\n"
          "Elaborate options:\n"
          "     --cover\t\tEnable code coverage reporting\n"
          "     --dump-llvm\tDump generated LLVM IR\n"
          "     --dump-vcode\tPrint generated intermediate code\n"
          " -g NAME=VALUE\t\tSet top level generic NAME to VALUE\n"
          " -O0, -O1, -O2, -O3\tSet optimisation level (default is -O2)\n"
          " -V, --verbose\t\tPrint resource usage at each step\n"
          "\n"
          "Run options:\n"
          "     --dump-arrays\tInclude nested arrays in waveform dump\n"
          "     --exclude=GLOB\tExclude signals matching GLOB from wave dump\n"
          "     --exit-severity=S\tExit after assertion failure of severity S\n"
          "     --format=FMT\tWaveform format is either fst or vcd\n"
          "     --ieee-warnings=\tEnable ('on') or disable ('off') warnings\n"
          "     \t\t\tfrom IEEE packages\n"
          "     --include=GLOB\tInclude signals matching GLOB in wave dump\n"
#ifdef ENABLE_VHPI
          "     --load=PLUGIN\tLoad VHPI plugin at startup\n"
#endif
          "     --profile\t\tDisplay detailed statistics at end of run\n"
          "     --stats\t\tPrint time and memory usage at end of run\n"
          "     --stop-delta=N\tStop after N delta cycles (default %d)\n"
          "     --stop-time=T\tStop after simulation time T (e.g. 5ns)\n"
          "     --trace\t\tTrace simulation events\n"
#ifdef ENABLE_VHPI
          "     --vhpi-trace\tTrace VHPI calls and events\n"
#endif
          " -w, --wave=FILE\tWrite waveform data; file name is optional\n"
          "\n"
          "Dump options:\n"
          " -e, --elab\t\tDump an elaborated unit\n"
          " -b, --body\t\tDump package body\n"
          "\n"
          "Make options:\n"
          "     --deps-only\tOutput dependencies without actions\n"
          "     --posix\t\tStrictly POSIX compliant makefile\n"
          "\n",
          PACKAGE,
          opt_get_int("stop-delta"));

   LOCAL_TEXT_BUF tb = tb_new();
   lib_print_search_paths(tb);
   printf("Library search paths:%s\n", tb_get(tb));

   printf("\nReport bugs to %s\n", PACKAGE_BUGREPORT);
}

static vhdl_standard_t parse_standard(const char *str)
{
   char *eptr = NULL;
   const int year = strtol(str, &eptr, 10);
   if ((eptr != NULL) && (*eptr == '\0')) {
      switch (year) {
      case 1987:
      case 87:
         fatal("VHDL standard 1076-1987 is not supported");
      case 1993:
      case 93:
         return STD_93;
      case 2000:
      case 0:
         return STD_00;
      case 2002:
      case 2:
         return STD_02;
      case 2008:
      case 8:
         return STD_08;
      }
   }

   fatal("invalid standard revision: %s (allowed 1993, 2000, 2002, 2008)", str);
}

static message_style_t parse_message_style(const char *str)
{
   if (strcmp(optarg, "full") == 0)
      return MESSAGE_FULL;
   else if (strcmp(optarg, "compact") == 0)
      return MESSAGE_COMPACT;

   fatal("invalid message style '%s' (allowed are 'full' and 'compact')", str);
}

static size_t parse_size(const char *str)
{
   char *eptr;
   const long size = strtol(str, &eptr, 0);

   if (size <= 0)
      fatal("invalid size '%s' (must be positive)", str);
   else if (*eptr == '\0')
      return size;
   else if (strcasecmp(eptr, "k") == 0)
      return size * 1024;
   else if (strcasecmp(eptr, "m") == 0)
      return size * 1024 * 1024;
   else if (strcasecmp(eptr, "g") == 0)
      return size * 1024 * 1024 * 1024;

   fatal("invalid size '%s' (expected a number with optional k, m, "
         "or g suffix)", str);
}

static void parse_library_map(char *str)
{
   char *split = strchr(str, ':');
   if (split == NULL)
      fatal("invalid library map syntax '%s': use NAME:PATH", str);

   *split = '\0';

   if (strcasecmp(str, "work") == 0)
      fatal("use --work option to specify work library name and path");

   lib_add_map(str, split + 1);
}

static void parse_work_name(char *str, const char **name, const char **path)
{
   char *split = strchr(str, ':');
   if (split == NULL) {
      char *slash = strrchr(str, *DIR_SEP) ?: strrchr(str, '/');
      if (slash == NULL)
         *name = *path = str;
      else {
         *name = slash + 1;
         *path = str;
      }
   }
   else {
      *split = '\0';
      *name = str;
      *path = split + 1;
   }
}

static int process_command(int argc, char **argv)
{
   static struct option long_options[] = {
      { "dump",    no_argument, 0, 'd' },
      { "make",    no_argument, 0, 'm' },
      { "syntax",  no_argument, 0, 's' },
      { "list",    no_argument, 0, 'l' },
      { 0, 0, 0, 0 }
   };

   opterr = 0;
   optind = 1;

   int index = 0;
   const char *spec = "aer";
   switch (getopt_long(MIN(argc, 2), argv, spec, long_options, &index)) {
   case 'a':
      return analyse(argc, argv);
   case 'e':
      return elaborate(argc, argv);
   case 'r':
      return run(argc, argv);
   case 'd':
      return dump_cmd(argc, argv);
   case 'm':
      return make_cmd(argc, argv);
   case 's':
      return syntax_cmd(argc, argv);
   case 'l':
      return list_cmd(argc, argv);
   default:
      fatal("missing command, try %s --help for usage", PACKAGE);
      return EXIT_FAILURE;
   }
}

int main(int argc, char **argv)
{
   term_init();
   set_default_opts();
   intern_strings();
   register_signal_handlers();

   atexit(fbuf_cleanup);

   static struct option long_options[] = {
      { "help",        no_argument,       0, 'h' },
      { "version",     no_argument,       0, 'v' },
      { "work",        required_argument, 0, 'w' },
      { "std",         required_argument, 0, 's' },
      { "messages",    required_argument, 0, 'I' },
      { "native",      no_argument,       0, 'n' },   // DEPRECATED
      { "map",         required_argument, 0, 'p' },
      { "ignore-time", no_argument,       0, 'i' },
      { "force-init",  no_argument,       0, 'f' },
      { 0, 0, 0, 0 }
   };

   opterr = 0;

   const char *work_name = "work";
   const char *work_path = work_name;
   lib_t work = NULL;

   const int next_cmd = scan_cmd(1, argc, argv);
   int c, index = 0;
   const char *spec = "aehrvL:M:";
   while ((c = getopt_long(next_cmd, argv, spec, long_options, &index)) != -1) {
      switch (c) {
      case 0:
         // Set a flag
         break;
      case 'h':
         usage();
         exit(EXIT_SUCCESS);
      case 'v':
         printf("%s\n%s\n", version_string, copy_string);
         exit(EXIT_SUCCESS);
      case 'w':
         parse_work_name(optarg, &work_name, &work_path);
         break;
      case 'L':
         lib_add_search_path(optarg);
         break;
      case 's':
         set_standard(parse_standard(optarg));
         break;
      case 'I':
         set_message_style(parse_message_style(optarg));
         break;
      case 'p':
         parse_library_map(optarg);
         break;
      case 'i':
         opt_set_int("ignore-time", 1);
         break;
      case 'f':
         opt_set_int("force-init", 1);
         break;
      case 'n':
         warnf("the --native option is deprecated and has no effect");
         break;
      case 'M':
         opt_set_int("arena-size", parse_size(optarg));
         break;
      case '?':
         bad_option("global", argv);
      default:
         abort();
      }
   }

   work = lib_new(work_name, work_path);
   lib_set_work(work);

   argc -= next_cmd - 1;
   argv += next_cmd - 1;

   return process_command(argc, argv);
}
