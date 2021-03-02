/** \file    envtool_py.c
 *  \ingroup Envtool_PY
 *  \brief The Python functions for the envtool program.
 *
 * All functions require that `py_init()` was called first.
 * The output from `call_python_func()` is captured and can be parsed.
 * Look at `py_print_modinfo()` for an example.
 *
 * By Gisle Vanem <gvanem@yahoo.no> 2011 - 2020.
 */

#include <stddef.h>
#include <fcntl.h>
#include <errno.h>
#include <assert.h>

#include "color.h"
#include "ignore.h"
#include "envtool.h"
#include "envtool_py.h"
#include "dirlist.h"
#include "report.h"
#include "cache.h"
#include "get_file_assoc.h"

/* No need to include <Python.h> just for this:
 */

/** \typedef PyObject
 *  Most things in Python are objects of something we don't care about.
 */
typedef void PyObject;

/** \typedef Py_ssize_t
 *  The storage-size type.
 */
typedef long Py_ssize_t;

/** \typedef python_path
 *  The layout of each `sys.path[]` entry.
 */
typedef struct python_path {
        char dir [_MAX_PATH];  /**< Fully qualified directory of this entry. */
        int  exist;            /**< does it exist? */
        int  is_dir;           /**< and is it a dir; `_S_ISDIR()` */
        int  is_zip;           /**< or is it a zip; an .egg, .zip or a .whl file. */
      } python_path;

/** \typedef python_module
 *  The layout of each `pi->modules[]` entry.
 */
typedef struct python_module {
        char name [30];               /**<  */
        char version [40];            /**< This length should match the `p.version` in `PY_LIST_MODULES()` */
        char location [_MAX_PATH];    /**<  */
        char meta_path [_MAX_PATH];   /**< path to `METADATA` or `PKG-INFO` file. Equals "-" if none. */
        BOOL is_zip;                  /**<  */
        BOOL is_actual;               /**< Already called `get_actual_filename (&m->location)` */

       /** \todo
        * Put the module/package `RECORD` information here as a smartlist of `struct record_info`.
        * smartlist_t *record;
        */
      } python_module;

/**\typedef arg_vector
 * The structure used in `PySys_SetArgvEx()`.
 */
typedef struct arg_vector {
        int       argc;         /**< The number of element in one of the below arrays. */
        char    **ascii;        /**< Only used for Python 2.x. */
        wchar_t **wide;         /**< Only used for Python 3.x. */
      } arg_vector;

/**
 * The type of Python we're currently using.
 *
 * Ref: \ref g_pi
 *      \ref bitness
 */
enum python_variants py_which = DEFAULT_PYTHON;

/**
 * \typedef python_info
 *  All important data for each Python we support.
 */
typedef struct python_info {

  /** The basename of the specific Python interpreter.
   */
  char program [20];

  /** Which variant is this?
   */
  enum python_variants variant;

  /** Only a CPython program can be embeddable from a C-program.
   */
  BOOL is_embeddable;

  /** The list of expected .DLLs for this specific Python.
   *  Tested for existance in either `%SystemDir%` and/or directory
   *  of `exe_name`.
   */
  const char *libraries [3];

  /** The FQFN of the Python `program`.
   */
  char *exe_name;

  /** The FQFN of the .dll that matches the first `libraries[]` format above.<br>
   *  If this Python `is_embeddable`, use this `dll_name` in `LoadLibrary()`
   *  during `py_init_embedding()`.
   */
  char *dll_name;

  /** The directory of above `exe_name`.
   */
  char exe_dir [_MAX_PATH];

  /** The `sys.path[]` array of above Python `exe_name`.
   * A smartlist of `struct python_path`.
   */
  smartlist_t *sys_path;

  /** The Python modules and packages array.
   *  A smartlist of `struct python_module`.
   */
  smartlist_t *modules;

  /** The user's `site-packages` path (if any).
   */
  char *user_site_path;

  /** Warn once if the above is not set.
   */
  BOOL do_warn_user_site;

  /** The Python version information.
   */
  int ver_major, ver_minor, ver_micro;

  /** Bitness of `dll_name`. `enum Bitness::bit_32` or `enum Bitness::bit_64`.
   * \anchor bitness
   */
  enum Bitness bitness;

  /** Embedding requires that the bitness of this CPython is the same as this calling program.
   */
  BOOL bitness_ok;

  /** This is the only default; i.e. the first Python `program` on `%PATH`.
   */
  BOOL is_default;

  /** Is this a CygWin Python?
   */
  BOOL is_cygwin;

  /** The `sys.prefix` used in `(*Py_SetPythonHome)()`.
   */
  char    *home_a;  /**< ASCII-version for Python 2.x */
  wchar_t *home_w;  /**< Widechar-version for Python 3.x */

  /** The program-names used in `(*Py_SetProgramName)()`.
   */
  char    *prog_a;  /**< ASCII-version for Python 2.x */
  wchar_t *prog_w;  /**< Widechar-version for Python 3.x */

  /** Warn once if `home_a` is not set.
   */
  BOOL     do_warn_home;

  /** Holds a reference to the `sys.stdout` catcher object.
   *  Only set if `is_embeddable == TRUE`:
   */
  PyObject *catcher;

  /** Holds the handle from `LoadLibrary()`.
   *  Only set if `is_embeddable == TRUE`:
   */
  HANDLE dll_hnd;

  /** The index of this Python in the `py_programs` smartlist.
   * I.e. in the cache-file "python_in_pathX = ...", `py_index == X`.
   */
  int py_index;

} python_info;

/**
 * List of all Pythons we care about. Ignore the console-less `pythonw.exe` programs.
 */
static struct python_info all_py_programs[] = {

    /* PyPy */
    { "pypy.exe",   PYPY_PYTHON,   FALSE, { "%s\\libpypy-c.dll", NULL }, },

    /* CPython */
    { "python.exe", PY3_PYTHON,    TRUE,  { "~\\libpython%d.%d.dll", "%s\\python%d%d.dll", NULL }, },
    { "python.exe", PY2_PYTHON,    TRUE,  { "~\\libpython%d.%d.dll", "%s\\python%d%d.dll", NULL }, },

    /* IronPython */
    { "ipy.exe",    IRON2_PYTHON,  FALSE, { "~\\IronPython.dll", NULL }, },
    { "ipy64.exe",  IRON2_PYTHON,  FALSE, { "~\\IronPython.dll", NULL }, },

    /* JavaPython */
    { "jython.exe", JYTHON_PYTHON, FALSE, { "~\\jpython.dll", NULL }, }
  };

/**
 * The global Python instance pointer.
 * We need this mostly for `build_sys_path()` to work.
 */
static struct python_info *g_pi;

/**
 * The bitness (32/64-bit) of our running program.
 * To interact directly between the C-world and the Python-world, the
 * first priority is that these are the same.
 */
static enum Bitness our_bitness;

/**
 * Help index for `py_select_first()` and `py_select_next()`.
 */
static int py_index = -1;

/**
 * Some temporary globals for retrieving the Python version and
 * the path for user's `site-packages` via "sysconfig.get_path('purelib'...".
 */
static int  tmp_ver_major, tmp_ver_minor, tmp_ver_micro;
static char tmp_user_site [_MAX_PATH];

static int longest_py_program = 0;  /** set in py_init() */
static int longest_py_version = 0;  /** set in py_init() */
static int global_indent = 0;

/**
 * Variables used in the generic callback helper function popen_append_out().
 */
static char  *popen_tmp = NULL;
static char  *popen_out = NULL;
static size_t popen_out_sz = 0;
static BOOL   popen_py_crash = FALSE;

#if !defined(_DEBUG)
  static HANDLE exc_hnd = NULL;
#endif

static int  get_python_version (const char *exe_name);
static BOOL py_add_module (struct python_info *pi, const struct python_module *m);

/**
 * The list of Pythons from the `"%PATH"` and from the
 * `"HKLM\Software\Python\PythonCore\xx\InstallPath"` locations.
 * This is an array of `struct python_info`.
 */
static smartlist_t *py_programs;

/**
 * \def LOAD_FUNC(pi, is_opt, f)
 *   A `GetProcAddress()` helper.
 *   \param pi      the `struct python_info` to work on.
 *   \param is_opt  `== 1` if the function is optional.
 *   \param f       the name of the function (without any `"`).
 */
#define LOAD_FUNC(pi, is_opt, f)  do {                                               \
                                    f = (func_##f) GetProcAddress (pi->dll_hnd, #f); \
                                    if (!f && !is_opt) {                             \
                                      WARN ("Failed to find \"%s()\" in %s.\n",      \
                                            #f, pi->dll_name);                       \
                                      goto failed;                                   \
                                    }                                                \
                                    TRACE (3, "Function %s(): %*s 0x%p.\n",          \
                                           #f, 23-(int)strlen(#f), "", f);           \
                                  } while (0)

/**
 * \def LOAD_INT_PTR(is_opt, f)
 *   A `GetProcAddress()` helper for setting an int-pointer.
 *   \param pi   the `struct python_info` to work on.
 *   \param ptr  the name of the variable (without any `"`).
 */
#define LOAD_INT_PTR(pi, ptr)  do {                                                        \
                                 ptr = (int*) GetProcAddress (pi->dll_hnd, #ptr);          \
                                 if (!ptr)                                                 \
                                      TRACE (2, "Failed to find \"%s\" in %s.\n",          \
                                             #ptr, pi->dll_name);                          \
                                 else TRACE (3, "Variable %s: %*s 0x%p = %d.\n",           \
                                             #ptr, 25-(int)strlen(#ptr), "", ptr, *(ptr)); \
                               } while (0)

/**
 * We only need 1 set of function-pointers for each embeddable Python program
 * since we currently only embed 1 Python at a time.
 *
 * But ideally, these function-pointers should be put in `struct python_info` to be able
 * to use several Pythons without calling `py_init_embedding()` and `py_exit_embedding()`
 * for each embeddable Python variant.
 *
 * \def DEF_FUNC(ret,f,(args))
 *   define the `typedef` and declare the function-pointer for
 *   the Python-2/3 function we want to import.
 *   \param ret    the return value type (or `void`)
 *   \param f      the name of the function (without any `"`).
 *   \param (args) the function arguments (as one list).
 */
#define DEF_FUNC(ret,f,args)  typedef ret (__cdecl *func_##f) args; \
                              static func_##f f

DEF_FUNC (void,        Py_InitializeEx,        (int init_sigs));
DEF_FUNC (int,         Py_IsInitialized,       (void));
DEF_FUNC (void,        Py_Finalize,            (void));
DEF_FUNC (void,        PySys_SetArgvEx,        (int argc, wchar_t **argv, int update_path));
DEF_FUNC (void,        Py_FatalError,          (const char *message));
DEF_FUNC (void,        Py_SetProgramName,      (char *name));
DEF_FUNC (void,        Py_SetPythonHome,       (void *home));
DEF_FUNC (int,         PyRun_SimpleString,     (const char *cmd));
DEF_FUNC (PyObject*,   PyImport_AddModule,     (const char *name));
DEF_FUNC (PyObject*,   PyObject_GetAttrString, (PyObject *o, char *attr));
DEF_FUNC (char *,      PyString_AsString,      (PyObject *o));
DEF_FUNC (char *,      PyBytes_AsString,       (PyObject *o));
DEF_FUNC (Py_ssize_t,  PyString_Size,          (PyObject *o));
DEF_FUNC (Py_ssize_t,  PyBytes_Size,           (PyObject *o));
DEF_FUNC (void,        PyObject_Free,          (PyObject *o));
DEF_FUNC (void,        Py_DecRef,              (PyObject *o));
DEF_FUNC (PyObject*,   PyObject_CallMethod,    (PyObject *o, char *method, char *fmt, ...));
DEF_FUNC (wchar_t*,    Py_DecodeLocale,        (const char *arg, size_t *size));
DEF_FUNC (void,        PyMem_RawFree,          (void *ptr));
DEF_FUNC (void,        PyErr_PrintEx,          (int set_sys_last_vars));
DEF_FUNC (PyObject*,   PyErr_Occurred,         (void));
DEF_FUNC (void,        PyErr_Clear,            (void));
DEF_FUNC (void,        initposix,              (void));
DEF_FUNC (void,        PyInit_posix,           (void));
DEF_FUNC (const char*, Anaconda_GetVersion,    (void));

static int *Py_InspectFlag = NULL;

static const struct search_list short_names[] = {
                  { ALL_PYTHONS,   "all"     },
                  { PY2_PYTHON,    "py2"     },
                  { PY3_PYTHON,    "py3"     },
                  { IRON2_PYTHON,  "ipy"     },
                  { IRON2_PYTHON,  "ipy2"    },
                  { IRON3_PYTHON,  "ipy3"    },
                  { PYPY_PYTHON,   "pypy"    },
                  { JYTHON_PYTHON, "jython"  }
                };

static const struct search_list full_names[] = {
                  { ALL_PYTHONS,   "All"         },
                  { PY2_PYTHON,    "Python2"     },
                  { PY3_PYTHON,    "Python3"     },
                  { IRON2_PYTHON,  "IronPython"  },
                  { IRON2_PYTHON,  "IronPython2" },
                  { IRON3_PYTHON,  "IronPython3" },
                  { PYPY_PYTHON,   "PyPy"        },
                  { JYTHON_PYTHON, "Jython"      }
                };

/**
 * Return a variant value from a short or a full name.
 *
 * E.g. `py_variant_value("all",NULL)` -> \ref ALL_PYTHONS.
 */
int py_variant_value (const char *short_name, const char *full_name)
{
  unsigned v = UINT_MAX;

  if (short_name)
     v = list_lookup_value (short_name, short_names, DIM(short_names));
  else if (full_name)
     v = list_lookup_value (full_name, full_names, DIM(full_names));
  if (v == UINT_MAX)
     v = UNKNOWN_PYTHON;
  return (int) v;
}

/**
 * Return a full-name from a variant value.
 *
 * E.g. `py_variant_name` (\ref ALL_PYTHONS) -> `"All"`.
 */
const char *py_variant_name (enum python_variants v)
{
  switch (v)
  {
    case UNKNOWN_PYTHON:
         return ("Unknown");
    case DEFAULT_PYTHON:
         return ("Default");
    default:
         return list_lookup_name (v, full_names, DIM(full_names));
  }
}

/**
 * A `qsort()` helper used in py_get_variants().
 */
static int MS_CDECL compare_strings (const void *_s1, const void *_s2)
{
  const char *s1 = *(const char**) _s1;
  const char *s2 = *(const char**) _s2;
  return stricmp (s1, s2);
}

/**
 * Returns a list of all supported variants.
 */
const char **py_get_variants (void)
{
  static char *result [DIM(all_py_programs)+1];
  const struct python_info *pi;
  int   i, j;

  if (result[0])  /* Already done this */
     return (const char**) result;

  for (i = j = 0, pi = all_py_programs; i < DIM(all_py_programs); pi++, i++)
  {
    switch (pi->variant)
    {
      case PY2_PYTHON:
           result [j++] = "py2";
           break;
      case PY3_PYTHON:
           result [j++] = "py3";
           break;
      case IRON2_PYTHON:
           result [j++] = "ipy2";
           break;
      case IRON3_PYTHON:
           result [j++] = "ipy3";
           break;
      case PYPY_PYTHON:
           result [j++] = "pypy";
           break;
      case JYTHON_PYTHON:
           result [j++] = "jython";
           break;
      case DEFAULT_PYTHON:
           result [j++] = "py";
           break;
      default:
           FATAL ("What?");
           break;
    }
  }

  result [j++] = "all";

  TRACE (3, "j: %d\n", j);
  for (i = 0; i < DIM(result); i++)
     TRACE (3, "py_get_variants(); result[%d] = %s\n", i, result[i]);

  qsort (&result[0], j, sizeof(char*), compare_strings);

  /* Make a unique result list.
   */
  for (i = 0; i < j; i++)
    if (i > 0 && result[i] && !strcmp(result[i],result[i-1]))
    {
      memmove (&result[i-1], &result[i], (DIM(result) - i) * sizeof(const char*));
      j--;
    }

  TRACE_NL (3);
  for (i = 0; i < j; i++)
     TRACE (3, "py_get_variants(); result[%d] = %s\n", i, result[i]);

  j = i;
  ASSERT (j < DIM(result));

  result [j] = NULL;
  return (const char**) result;
}

/**
 * Select a Python that is found on `%PATH` and that we've found the DLL for
 * and that is of a suitable variant.
 * We cannot select \ref ALL_PYTHONS here.
 */
static struct python_info *py_select (enum python_variants which)
{
  struct python_info *pi;
  int    i, max = smartlist_len (py_programs);

  for (i = 0; i < max; i++)
  {
    pi = smartlist_get (py_programs, i);
    if (!pi->exe_name || !pi->dll_name)
       continue;

    if ((which == DEFAULT_PYTHON && pi->is_default) || which == pi->variant)
    {
      TRACE (1, "py_select (%d); \"%s\" -> \"%s\"\n", which, py_variant_name(pi->variant), pi->exe_name);
      return (pi);
    }
  }
  TRACE (1, "py_select (%d); \"%s\" not possible.\n", which, py_variant_name(which));
  return (NULL);
}

/**
 * Returns some information for the selected Python.
 *
 * \param[in] exe A pointer to a `char*` filled with .EXE-name of selected Python program.
 * \param[in] dll A pointer to a `char*` filled with .DLL-name of selected Python library.
 * \param[in] ver A pointer to a `struct ver_info` filled with version information.
 *
 * \retval 0  On some error; none of the in-params gets filled.
 *         1  If all went okay.
 */
int py_get_info (char **exe, char **dll, struct ver_info *ver)
{
  const struct python_info *pi;

  if (exe)
     *exe = NULL;

  if (dll)
     *dll = NULL;

  if (py_which == ALL_PYTHONS)          /* Not possible here */
       pi = py_select (DEFAULT_PYTHON);
  else pi = py_select (py_which);

  if (!pi)
     return (0);

  if (exe)
     *exe = STRDUP (pi->exe_name);

  if (dll)
     *dll = STRDUP (pi->dll_name);

  if (ver)
  {
    ver->val_1 = pi->ver_major;
    ver->val_2 = pi->ver_minor;
    ver->val_3 = pi->ver_micro;
    ver->val_4 = 0;
  }
  return (1);
}

/**
 * Since both Python2 and Python3 usually have the same `python.exe` name, we
 * don't know it's variant for sure before obtaining it's `py->ver_major` value.
 * If the `value` from `all_py_programs[]` is either `PY2_PYTHON` or
 * `PY3_PYTHON`, fix the `py->py_variant` member accordingly.
 */
static void fix_python_variant (struct python_info *pi, enum python_variants v)
{
  if (v == PY2_PYTHON || v == PY3_PYTHON)
  {
    if (pi->ver_major >= 3)
         pi->variant = PY3_PYTHON;
    else pi->variant = PY2_PYTHON;
  }
  else
    pi->variant = v;
}

/**
 * Returns a `sys.path[]` directory for printing.
 *
 * Add a trailing slash (`\\` or c `/`) for true directories only.<br>
 * Not for ZIP/EGGs in `sys.path[]`.
 */
static const char *dir_name (const char *dir, BOOL is_dir)
{
  static char ret [_MAX_PATH];
  char  *end;
  char   slash = opt.show_unix_paths ? '/' : '\\';

  slashify2 (ret, dir, slash);
  end = strchr (ret, '\0');
  if (is_dir && end-ret-1 < _MAX_PATH && !IS_SLASH(end[-1]))
  {
    *end++ = slash;
    *end = '\0';
  }
  return (ret);
}

/**
 * As part of `py_test()` print the `sys.path[]` for this Python.
 */
static void print_sys_path (const struct python_info *pi, int indent)
{
  int i, max = smartlist_len (pi->sys_path);

  for (i = 0; i < max; i++)
  {
    const struct python_path *pp  = smartlist_get (pi->sys_path, i);
    const char               *dir = dir_name (pp->dir, pp->is_dir);

    if (indent)
    {
      if (i == 0)
           C_printf ("%*s %s\n", indent+global_indent, "~7sys.path[]:~0", dir);
      else C_printf ("%*s %s\n", indent+global_indent-4, "", dir);
    }
    else
      C_printf ("~6%3d: ~0%s\n", i, dir);
  }
}

static void print_home_path (const struct python_info *pi, int indent)
{
  C_printf ("%*s", indent+global_indent, "~7Home-path:~0");
  if (pi->home_a)
     C_printf (" %s\n", dir_name(pi->home_a, TRUE));
  else if (pi->home_w)
     C_printf (" %" WIDESTR_FMT "\n", pi->home_w);
  else
     C_puts (" ~5<none>~0\n");
}

static void print_user_site_path (const struct python_info *pi, int indent)
{
  C_printf ("%*s", indent+global_indent, "~7User-site:~0");
  if (pi->user_site_path)
       C_printf (" %s %s~0\n", dir_name(pi->user_site_path, TRUE),
                 is_directory(pi->user_site_path) ? "~2OK" : "~5Does not exist");
  else C_puts (" ~5<none>~0\n");
}

/**
 * Get the name of the calling program. This allocated ASCII string is used in
 * `(*Py_SetProgramName)()` and `(*Py_SetPythonHome)()`.
 */
static char *get_prog_name_ascii (const struct python_info *pi)
{
  char  prog [_MAX_PATH];
  char *p = NULL;

  if (GetModuleFileNameA(NULL, prog, DIM(prog)))
     p = prog;

  if (pi->is_cygwin)
     p = make_cyg_path (prog, prog);
  return (p ? STRDUP(p) : NULL);
}

static wchar_t *get_prog_name_wchar (const struct python_info *pi)
{
  wchar_t  prog [_MAX_PATH];
  wchar_t *p = NULL;

  if (GetModuleFileNameW(NULL, prog, DIM(prog)))
     p = prog;

  if (pi->is_cygwin)
     p = make_cyg_pathw (prog, prog);
  return (p ? WCSDUP(p) : NULL);
}

/**
 * Setup the `py->prog_a` or `py->prog_w`.
 *
 * The allocated string (ASCII or Unicode) is freed in py_exit().
 */
static void set_python_prog (struct python_info *pi)
{
  if (pi->ver_major >= 3)
  {
    pi->prog_w = get_prog_name_wchar (pi);
    pi->prog_a = NULL;
  }
  else
  {
    pi->prog_a = get_prog_name_ascii (pi);
    pi->prog_w = NULL;
  }
}

/**
 * This should be the same as `sys.prefix`.
 *
 * The allocated string (ASCII or Unicode) gets freed in `py_exit()`.
 */
static void set_python_home (struct python_info *pi)
{
  char *dir = pi->exe_dir;

  if (pi->ver_major >= 3)
  {
    wchar_t buf [_MAX_PATH];

    buf[0] = L'\0';
    MultiByteToWideChar (CP_ACP, 0, dir, -1, buf, DIM(buf));
    if (pi->is_cygwin)
    {
      pi->home_w = WCSDUP (L"/usr");
      pi->home_a = STRDUP ("/usr");
    }
    else
    {
      pi->home_w = WCSDUP (buf);
      pi->home_a = STRDUP (dir);
    }
  }
  else
  {
    /* Reallocate again because FREE() is used in py_exit()!
     */
    if (pi->is_cygwin)
         pi->home_a = STRDUP ("/usr");
    else pi->home_a = STRDUP (dir);
    pi->home_w = NULL;
  }
}

/**
 * Set the imported `Py_InspectFlag`.
 */
static void py_set_inspect_flag (int v)
{
  if (Py_InspectFlag)
     *Py_InspectFlag = v;
}

/**
 * Free the Python DLL handle allocated by `py_init_embedding()`.
 */
static void py_exit_embedding (struct python_info *pi)
{
  if (pi->dll_hnd && pi->dll_hnd != INVALID_HANDLE_VALUE)
  {
    py_set_inspect_flag (0);
    Py_InspectFlag = NULL;
    if (Py_Finalize)
    {
      TRACE (4, "Calling Py_Finalize().\n");
      (*Py_Finalize)();
    }
    if (!IsDebuggerPresent())
       CloseHandle (pi->dll_hnd);
  }
  pi->dll_hnd = INVALID_HANDLE_VALUE;
}

/**
 * Some smartlist_wipe() helpers:
 *
 * Free a `sys.path[]` element for a single `python_info` program.
 */
static void free_sys_path (void *e)
{
  struct python_path *pp = (struct python_path*) e;

  FREE (pp);
}

/**
 * Free a `modules[]` element for a single `python_info` program.
 */
static void free_module (void *_m)
{
  struct python_module *m = (struct python_module*) _m;

  FREE (m);
}

/**
 * A smartlist_wipe() helper:
 *
 * Free one element in \ref py_programs.
 */
static void free_py_program (void *_pi)
{
  struct python_info *pi = (struct python_info*) _pi;

  FREE (pi->prog_a);
  FREE (pi->prog_w);
  FREE (pi->home_a);
  FREE (pi->home_w);
  FREE (pi->dll_name);
  FREE (pi->exe_name);
  FREE (pi->user_site_path);

  if (pi->is_embeddable)
     py_exit_embedding (pi);

  if (pi->sys_path)
  {
    smartlist_wipe (pi->sys_path, free_sys_path);
    smartlist_free (pi->sys_path);
  }
  if (pi->modules)
  {
    smartlist_wipe (pi->modules, free_module);
    smartlist_free (pi->modules);
  }
  FREE (pi);
}

/**
 * Free all memory allocated during py_init().
 */
void py_exit (void)
{
  if (py_programs)
  {
    smartlist_wipe (py_programs, free_py_program);
    smartlist_free (py_programs);
  }

#if !defined(_DEBUG)
  if (exc_hnd && exc_hnd != INVALID_HANDLE_VALUE)
      FreeLibrary (exc_hnd);
  exc_hnd = NULL;
#endif

  g_pi = NULL;
}

/**
 * Setup a class-instance for catching all output written
 * using `sys.stdout`. I.e. `print(...)` and `os.write(1,...)`.
 *
 * This instance must reside at the global `__main__` level.
 *
 * Thus the Python printed strings are retrieved in the C-world by
 * `catcher.value` obtained with: <br>
 *  `obj = (*PyObject_GetAttrString) (py->catcher, "value");` <br>
 *
 * \todo
 *   Use `StringIO()` class instead?
 *
 * \note
 *   The line-endings in the `self.value` are normally Unix-type `"\n"`.
 *   These will become `"\r\n"` when this string gets printed to `stdout`
 *   in the C-world (since it's in text-mode by default).
 *
 * Ref:
 *   http://stackoverflow.com/questions/4307187/how-to-catch-python-stdout-in-c-code
 */
static PyObject *setup_stdout_catcher (void)
{
  static char code[] = "import sys\n"                               \
                       "PY3 = (sys.version_info[0] >= 3)\n"         \
                       "Empty = ['', b''][PY3]\n"                   \
                       "\n"                                         \
                       "class catch_stdout:\n"                      \
                       "  def __init__ (self):\n"                   \
                       "    self.value = Empty\n"                   \
                       "  def write (self, txt):\n"                 \
                       "    if PY3:\n"                              \
                       "      self.value += bytes(txt,\"UTF-8\")\n" \
                       "    else:\n"                                \
                       "      self.value += txt\n"                  \
                       "  def reset (self):\n"                      \
                       "    self.value = Empty\n"                   \
                       "  def flush (self):\n"                      \
                       "    self.reset()\n"                         \
                       "\n"                                         \
                       "old_stdout = sys.stdout\n"                  \
                       "sys.stdout = catcher = catch_stdout()\n";

  PyObject *mod = (*PyImport_AddModule) ("__main__");          /* create `__main__` module */
  int       rc  = (*PyRun_SimpleString) (code);                /* invoke code to redirect */
  PyObject *obj = (*PyObject_GetAttrString) (mod, "catcher");  /* get a reference to `catcher` created above */

  TRACE (5, "code: '%s'\n", code);
  TRACE (4, "mod: %p, rc: %d, obj: %p\n", mod, rc, obj);
  return (obj);
}

/**
 * Do NOT call this unless `py->is_embeddable == TRUE`.
 */
static BOOL py_init_embedding (struct python_info *pi)
{
  char *exe = pi->exe_name;
  char *dll = pi->dll_name;

  if (!dll)
  {
    WARN ("Failed to find Python DLL for %s.\n", exe);
    return (FALSE);
  }

  pi->dll_hnd = LoadLibrary (dll);
  if (!pi->dll_hnd)
  {
    WARN ("Failed to load %s; %s\n", dll, win_strerror(GetLastError()));
    pi->is_embeddable = FALSE;  /* Do not do this again */
    return (FALSE);
  }

  TRACE (2, "Full DLL name: \"%s\". Handle: 0x%p\n", pi->dll_name, pi->dll_hnd);

  LOAD_FUNC (pi, 0, Py_InitializeEx);
  LOAD_FUNC (pi, 0, Py_IsInitialized);
  LOAD_FUNC (pi, 0, Py_Finalize);
  LOAD_FUNC (pi, 0, PySys_SetArgvEx);
  LOAD_FUNC (pi, 0, Py_FatalError);
  LOAD_FUNC (pi, 0, Py_SetProgramName);
  LOAD_FUNC (pi, 0, Py_SetPythonHome);
  LOAD_FUNC (pi, 0, PyRun_SimpleString);
  LOAD_FUNC (pi, 0, PyObject_GetAttrString);
  LOAD_FUNC (pi, 0, PyImport_AddModule);
  LOAD_FUNC (pi, 0, PyObject_CallMethod);
  LOAD_FUNC (pi, 0, PyObject_Free);
  LOAD_FUNC (pi, 0, Py_DecRef);
  LOAD_FUNC (pi, 0, PyErr_Occurred);
  LOAD_FUNC (pi, 0, PyErr_PrintEx);
  LOAD_FUNC (pi, 0, PyErr_Clear);
  LOAD_FUNC (pi, 1, PyString_AsString);      /* CPython 2.x */
  LOAD_FUNC (pi, 1, PyBytes_AsString);       /* CPython 3.x */
  LOAD_FUNC (pi, 1, PyString_Size);          /* CPython 2.x */
  LOAD_FUNC (pi, 1, PyBytes_Size);           /* CPython 3.x */
  LOAD_FUNC (pi, 1, Py_DecodeLocale);        /* CPython 3.x */
  LOAD_FUNC (pi, 1, PyMem_RawFree);          /* CPython 3.x */
  LOAD_FUNC (pi, 1, initposix);              /* In Cygwin's libpython2.x.dll */
  LOAD_FUNC (pi, 1, PyInit_posix);           /* In Cygwin's libpython3.x.dll */
  LOAD_FUNC (pi, 1, Anaconda_GetVersion);    /* In Anaconda's python3x.dll. Not used yet. */

  LOAD_INT_PTR (pi, Py_InspectFlag);

  if (initposix || PyInit_posix)
     pi->is_cygwin = TRUE;

  if (pi->ver_major >= 3)
  {
    PyString_AsString = PyBytes_AsString;
    PyString_Size     = PyBytes_Size;

    TRACE (2, "Py_SetProgramName (\"%" WIDESTR_FMT "\")\n", pi->prog_w);
    (*Py_SetProgramName) ((char*)pi->prog_w);

    TRACE (2, "Py_SetPythonHome (\"%" WIDESTR_FMT "\")\n", pi->home_w);
    (*Py_SetPythonHome) (pi->home_w);
  }
  else
  {
    TRACE (2, "Py_SetProgramName (\"%s\")\n", pi->prog_a);
    (*Py_SetProgramName) (pi->prog_a);

    TRACE (2, "Py_SetPythonHome (\"%s\")\n", pi->home_a);
    (*Py_SetPythonHome) (pi->home_a);
  }

  (*Py_InitializeEx) (0);

  py_set_inspect_flag (1);

  pi->catcher = setup_stdout_catcher();
  if (pi->catcher)
     return (TRUE);   /* Success! */

  /* Fail, fall through */

failed:
  py_exit_embedding (pi);
  return (FALSE);
}

/**
 * Dump a `py_prog` to `stdout` or `stderr` in case of errors.
 * Add leading line numbers while printing the `py_prog`.
 *
 * \param[in] out      the `FILE*` to print to; either `stdout` or `stderr`.
 * \param[in] py_prog  the Python program to dump.
 * \param[in] where    the line-number where the `py_prog` was executed.
 */
static char *py_prog_dump (FILE *out, const char *py_prog, unsigned where)
{
  char *chunk, *tok_end;
  char *prog = STRDUP (py_prog);
  int   line;

  if (out == stderr)
     Beep (1000, 30);

  fprintf (out, "py_prog at line %u:\n"
                "---------------------------\n", where);

  for (line = 1, chunk = _strtok_r(prog, "\n", &tok_end);
       chunk;
       chunk = _strtok_r(NULL, "\n", &tok_end), line++)
  {
    fprintf (out, "%2d: %s\n", line, chunk);
    if (*tok_end == '\n')
       fprintf (out, "%2d:\n", line++); /* empty program line */
  }

  fputs ("---------------------------\n", out);
  fflush (out);
  FREE (prog);
  return (NULL);
}

/**
 * Call some Python code (for an embedded Python only).
 *
 * \param[in] pi     The currently used Python-info. Should be the same as \ref g_pi.
 * \param[in] prog   The code to run.
 * \param[in] line   The line where this function was called (for tracing).
 * \retval !NULL     The output from Python. **Must** be freed using `FREE()`.<br>
 *                   The `catcher.value` is reset.
 * \retval NULL      The `PyRun_SimpleString()` failed (a syntax error?). <br>
 *                   The `PyObject_GetAttrString()` failed to get the `catcher.value`.<br>
 *                   The `PyString_Size()` returned 0 (no `print()` was called?).<br>
 *                   The `catcher.value` is *not* reset.
 *
 * \note if `opt.debug >= 3`:
 *       \li the program is dumped one line at a time prior to running it.
 *       \li the program output is parsed and the lines are counted. Then the size is
 *           compared against what a DOS-ified string-size would become (`dos_size`).<br>
 *           I.e. compare against a string with all `"\r\n"` line-endings.
 */
static char *call_python_func (struct python_info *pi, const char *prog, unsigned line)
{
  PyObject  *obj, *err;
  Py_ssize_t size = 0;
  char      *str = NULL;
  int        rc;

  if (opt.debug >= 3)
     py_prog_dump (stdout, prog, line);

  ASSERT (pi->catcher);
  rc = (*PyRun_SimpleString) (prog);

  err = (*PyErr_Occurred)();
  TRACE (2, "rc: %d, err: %p\n", rc, err);

  if (rc < 0)
  {
    (*PyErr_Clear)();
    WARN ("Failed script (%s): ", pi->exe_name);
    return py_prog_dump (stderr, prog, line);
  }

  /* Get a reference to `catcher.value` where the catched `sys.stdout` string is stored.
   */
  obj = (*PyObject_GetAttrString) (pi->catcher, "value");

  TRACE (4, "rc: %d, obj: %p\n", rc, obj);

  if (rc == 0 && obj)
  {
    size = (*PyString_Size) (obj);
    if (size > 0)
       str = STRDUP ((*PyString_AsString)(obj));

    /* Reset the `catcher.value` to prepare for next call
     * to this call_python_func().
     */
    (*PyObject_CallMethod) (pi->catcher, "reset", NULL);
  }
  TRACE (3, "PyString_Size(): %ld, output:\n%s\n", size, str);

  /* Count the lines in the output `str` and compare `size` against what
   * a DOS-ified string-size would become (`dos_size`).
   * I.e. compare against a string with all `"\r\n"` line-endings.
   */
  if (size > 0 && opt.debug >= 3)
  {
    char      *chunk, *tok_end;
    char      *copy = STRDUP (str);
    int        okay, lines = 0;
    Py_ssize_t dos_size = 0;

    for (chunk = _strtok_r(copy, "\n", &tok_end);
         chunk;
         chunk = _strtok_r(NULL, "\n", &tok_end), lines++)
        dos_size += strlen(chunk) + 2;   /* 2 for "\r\n" line-endings */

    FREE (copy);
    okay = (dos_size == size+lines);
    TRACE (3, "dos_size: %ld, size+lines: %ld, lines: %d; %s\n",
           dos_size, size+lines, lines, okay ? "OK" : "discrepancy in dos_size");
  }
  return (str);
}

/**
 * Return a `file` with correct slashes and filename casing.
 */
static const char *py_filename (const char *file)
{
  static char buf [_MAX_PATH];
  char *p = _strlcpy (buf, file, sizeof(buf));

  if (get_actual_filename(&p, FALSE))
  {
    _strlcpy (buf, p, sizeof(buf));
    FREE (p);
  }
  return (buf);
}

/**
 * A simple embedded test; capture the output from Python's
 * `print()` function.
 */
static BOOL test_python_funcs (struct python_info *pi, BOOL reinit)
{
  static char prog[] = "import sys\n"
                       "print (sys.version_info)\n"
                       "for i in range(3):\n"
                       "  print (\"  Hello world\")\n";
  const char *name = py_variant_name (pi->variant);
  char *str;

  if (reinit && (!pi->bitness_ok || !py_init_embedding(pi)))
     return (FALSE);

  /* The `str` should now contain the Python output of above `prog`.
   */
  str = call_python_func (pi, prog, __LINE__);
  C_printf ("~3Captured output of %s:~0\n** %s **\n", name, str);
  FREE (str);

  /* Restore `sys.stdout` to it's old value. Thus this should return no output.
   */
  C_printf ("~3The rest should not be captured:~0\n");
  str = call_python_func (pi, "sys.stdout = old_stdout\n", __LINE__);
  FREE (str);

  str = call_python_func (pi, prog, __LINE__);
  C_printf ("~3Captured output of %s now:~0\n** %s **\n", name, str);
  FREE (str);
  return (TRUE);
}

/**
 * Create a `%TEMP%`-file and write a .py-script to it.
 * The file-name `tmp` is allocated in \ref misc.c.
 *
 * \retval !NULL the name of the `%TEMP%`-file.
 *               Caller should call `unlink()` and `FREE()` on this return-value
 *               after use.
 * \retval NULL  the function failed. `errno` should be set.
 */
static char *tmp_fputs (const struct python_info *pi, const char *buf)
{
  FILE *fil;
  char *tmp = create_temp_file();

  if (!tmp)
     return (NULL);

  fil = fopen (tmp, "w+t");
  if (!fil)
  {
    FREE (tmp);
    return (NULL);
  }
  fprintf (fil, "#\n"
                "# Temp-file %s for command \"%s %s\".\n"
                "# Created %s (%s).\n"
                "#\n", tmp, pi->exe_name, tmp,
                get_time_str(time(NULL)), _tzname[0]);
  fwrite (buf, 1, strlen(buf), fil);
  fclose (fil);
  return (tmp);
}

/**
 * Append `str` to `popen_out` updating `popen_out_sz` as needed.
 *
 * This **must** be used if the Python is *not* embeddable.
 * But it can be used if the Python is embeddable (allthough
 * this is much slower than using `call_python_func()` directly).
 *
 * Write the Python-code (\ref PY_LIST_MODULES() or \ref PY_ZIP_LIST()) to a
 * temporary file and `call popen_run()` on it. Then parse the output.
 */
static int popen_append_out (char *str, int index)
{
  /**
   * Report this Python "crash" to `popen_run_py()`.
   * Hopefully it will happen only at the first line (`index=0`).
   * This also needs `stderr` to be redirected into `stdout`.
   */
  if (!strncmp(str,"Traceback (",11) || !strncmp(str,"ImportError:",13))
     popen_py_crash = TRUE;

  if (!popen_out || popen_out_sz - strlen(popen_out) < 4*strlen(str))
  {
    popen_out_sz += 4 * strlen (str);
    popen_out = REALLOC (popen_out, popen_out_sz);
  }

  TRACE (2, "index: %d, strlen(popen_out): %d, popen_out_sz: %d\n",
         index, (int)strlen(popen_out), (int)popen_out_sz);

  if (index == 0)
     popen_out[0] = '\0';

  str_cat (popen_out, popen_out_sz, str);
  str_cat (popen_out, popen_out_sz-1, "\n");
  popen_out_sz = strlen (popen_out);
  return (1);
}

/**
 * Clear up things after `popen_run_py()` has finished.
 */
static void popen_append_clear (void)
{
  if (popen_tmp && !opt.keep_temp)
     unlink (popen_tmp);

  popen_out_sz = 0;
  popen_py_crash = FALSE;
  FREE (popen_out);
  FREE (popen_tmp);
}

/**
 * The `popen_run()` function for running a non-embedded Python program.
 */
static char *popen_run_py (const struct python_info *pi, const char *prog)
{
  char        report [1000];
  const char *py_exe = pi->exe_name;
  int         rc;

  popen_tmp = tmp_fputs (pi, prog);
  if (!popen_tmp)
  {
    WARN ("\"%s\": Failed to create a temp-file; errno: %d.\n", pi->exe_name, errno);
    popen_append_clear();
    return (NULL);
  }

  /** \todo: need to check 'pi->is_cygwin' first
   */
#ifdef __CYGWIN__
  char cyg_exe_name [_MAX_PATH];
  char win_tmp_name [_MAX_PATH];

  if (cygwin_conv_path(CCP_WIN_A_TO_POSIX, pi->exe_name, cyg_exe_name, sizeof(cyg_exe_name)) != 0)
     return (NULL);
  if (cygwin_conv_path(CCP_POSIX_TO_WIN_A, popen_tmp, win_tmp_name, sizeof(win_tmp_name)) != 0)
     return (NULL);

  py_exe = cyg_exe_name;
  rc = popen_run (popen_append_out, cyg_exe_name, "%s 2>&1", win_tmp_name);
#else
  rc = popen_run (popen_append_out, py_exe, "%s 2>&1", popen_tmp);
#endif

  if (rc < 0)
  {
    snprintf (report, sizeof(report), "\"%s %s\"; errno: %d\n", py_exe, popen_tmp, errno);
    Beep (1000, 30);
    WARN ("Failed script (%s): ", py_exe);
    C_puts (report);
    popen_append_clear();
    return (NULL);
  }

  if (popen_py_crash)
  {
    snprintf (report, sizeof(report), "\"%s %s\":\n%s\n", py_exe, popen_tmp, popen_out);
    Beep (1000, 30);
    WARN ("Failed script (%s): ", py_exe);
    C_puts (report);
    popen_append_clear();
    return (NULL);
  }
  return (popen_out);
}

/**
 * Print the list of installed Python modules with
 * their location and version.
 *
 * \param[in] spec         Print only those module-names `m->name` that matches `spec`.
 * \param[in] get_details  Print more details, like meta information.
 *
 * \retval The number of modules printed.
 */
static int py_print_modinfo (const char *spec, BOOL get_details)
{
  BOOL match_all = !strcmp (spec, "*");
  int  found = 0, zips_found = 0;

  int i, max = g_pi->modules ? smartlist_len (g_pi->modules) : 0;

  for (i = 0; i < max; i++)
  {
    struct python_module *m = smartlist_get (g_pi->modules, i);

    if (fnmatch(spec, m->name, fnmatch_case(0)) != FNM_MATCH)
       continue;

#if 0
    if (opt.use_cache && m->meta_path[0] && !FILE_EXISTS(m->meta_path))
    {
      cache_delf (SECTION_PYTHON, "python_modules%d_%d", g_pi->py_index, i);
      FREE (m);
      smartlist_del (g_pi->modules, i);
      max--;
      continue;
    }
#endif

    found++;
    if (m->is_zip)
       zips_found++;

    if (get_details)
    {
      const char *loc = py_filename (m->location);

      C_printf ("~6%3d:~0 %-25s %-10s -> %s%s\n", found, m->name, m->version,
                m->is_actual ? m->location : loc,
                m->is_zip ? " (ZIP)" : "");

      if (m->meta_path[0] != '-')
         C_printf ("      meta: %s\n", py_filename(m->meta_path));

      _strlcpy (m->location, loc, sizeof(m->location));
      m->is_actual = TRUE;
      C_putc ('\n');
    }
    else
      C_printf ("   %s%s\n", m->version, m->is_zip ? " (ZIP)" : "");
  }
  if (match_all && !get_details)
     C_printf ("~6Found %d modules~0 (%d are ZIP/EGG files).\n", found, zips_found);
  return (found);
}

/**
 * \todo
 * Check if there is a `RECORD` file on `m->meta_path`.
 * If there is:
 *   \li build a list of files for this package.
 *   \li extract the size and SHA256 value for the file from a RECORD-line like:

         wheel/__init__.py,sha256=YumT_ajakW9VAgnV3umrUYypy6VzpbLKE-OPbVnWm8M,96
         ^                        ^                                           ^
         |                        |                                           |__ `__init__.py` size
         |                        |___ Base64 encoded?
         |_ relative to parent dir?
 */
static void py_get_meta_info (struct python_module *m)
{
#if 0

#else
  ARGSUSED (m);
#endif
}


/**
 * \def PY_LIST_MODULES
 *   The Python program for getting the modules information in `py_get_module_info()`.
 *   If the `pip.get_installed_distributions()` is an obsoleted PIP function,
 *   try the `pkg_resources.working_set` instead.
 *
 * \note The details of Modules versus Packages:
 *       https://docs.python.org/3/reference/import.html#packages
 */
#define PY_LIST_MODULES()                                                              \
        "import os, sys, re, zipfile\n"                                                \
        "\n"                                                                           \
        "def list_modules (spec):\n"                                                   \
        "  try:\n"                                                                     \
        "    import pip\n"                                                             \
        "    packages = pip.get_installed_distributions (local_only=False, skip=())\n" \
        "  except (AttributeError, ImportError):\n"                                    \
        "    import pkg_resources\n"                                                   \
        "    packages = pkg_resources.working_set\n"                                   \
        "  package_list = []\n"                                                        \
        "  for p in packages:\n"                                                       \
        "    if os.path.isdir (p.location):\n"                                         \
        "      loc = p.location + '\\\\'\n"                                            \
        "    elif zipfile.is_zipfile (p.location):\n"                                  \
        "      loc = p.location + ' (ZIP)\'\n"                                         \
        "    else:\n"                                                                  \
        "      loc = p.location\n"                                                     \
        "    ver = \"%.40s\" % p.version\n"                                            \
        "    try:\n"                                                                   \
        "      meta = p._get_metadata_path_for_display (p.PKG_INFO)\n"                 \
        "    except AttributeError:\n"                                                 \
        "      meta = \"-\"\n"                                                         \
        "    package_list.append ('%s;%s;%s;%s' % (p.key,ver,loc,meta))\n"             \
        "\n"                                                                           \
        "  for p in sorted (package_list):\n"                                          \
        "    print (p)\n"                                                              \
        "\n"                                                                           \
        "list_modules (\"*\")\n"

/**
 * \def PY_LIST_MODULES2
 *   A more detailed version of `PY_LIST_MODULES()` returning
 *   modules and packages at runtime.
 */
#define PY_LIST_MODULES2()                                                               \
        "from __future__ import print_function\n"                                        \
        "import os, sys, pip, imp\n"                                                     \
        "\n"                                                                             \
        "types = { imp.PY_SOURCE:     'source file',      # = 1\n"                       \
        "          imp.PY_COMPILED:   'object file',\n"                                  \
        "          imp.C_EXTENSION:   'shared library',   # = 3\n"                       \
        "          imp.PKG_DIRECTORY: 'package directory',\n"                            \
        "          imp.C_BUILTIN:     'built-in module',  # = 6\n"                       \
        "          imp.PY_FROZEN:     'frozen module'\n"                                 \
        "        }\n"                                                                    \
        "mod_paths    = {}\n"                                                            \
        "mod_builtins = {}\n"                                                            \
        "prev_pathname = ''\n"                                                           \
        "prev_modtype  = None\n"                                                         \
        "\n"                                                                             \
        "def get_module_path (mod, mod_type, mod_path):\n"                               \
        "  next_scope = mod.index ('.') + 1\n"                                           \
        "  last_scope = mod.rindex ('.') + 1\n"                                          \
        "\n"                                                                             \
        "  fname = mod_path + '\\\\' + mod[next_scope:].replace('.','\\\\\') + '.py'\n"  \
        "  if os.path.exists(fname):\n"                                                  \
        "    return fname\n"                                                             \
        "\n"                                                                             \
        "  init = mod_path + '\\\\' + mod[last_scope:] + '\\\\__init__.py'\n"            \
        "  if os.path.exists(init):\n"                                                   \
        "    return init\n"                                                              \
        "\n"                                                                             \
        "  try:\n"                                                                       \
        "    if mod_builtins[mod[last_scope:]] == 1:\n"                                  \
        "      return '__builtin__ ' + mod[last_scope:]\n"                               \
        "  except KeyError:\n"                                                           \
        "    pass\n"                                                                     \
        "\n"                                                                             \
        "  try:\n"                                                                       \
        "    return mod_paths [mod[last_scope:]] + ' !'\n"                               \
        "  except KeyError:\n"                                                           \
        "    pass\n"                                                                     \
        "\n"                                                                             \
        "  try:\n"                                                                       \
        "    _x, pathname, _y = imp.find_module (mod, mod_path)\n"                       \
        "    return pathname\n"                                                          \
        "  except ImportError:\n"                                                        \
        "    return '<unknown>'\n"                                                       \
        "  except RuntimeError:\n"                                                       \
        "    mod_builtins [mod] = 1\n"                                                   \
        "    return '__builtin__ ' + mod[last_scope:]\n"                                 \
        "\n"                                                                             \
        "for s in sorted(sys.modules):\n"                                                \
        "  print ('%s' % s, end='')\n"                                                   \
        "  if '.' in s:\n"                                                               \
        "    print (',%s' % get_module_path(s,prev_modtype,prev_pathname))\n"            \
        "    continue\n"                                                                 \
        "\n"                                                                             \
        "  try:\n"                                                                       \
        "    _, pathname, descr = imp.find_module (s)\n"                                 \
        "    t = types [descr[2]]\n"                                                     \
        "    prev_modtype = t\n"                                                         \
        "    print (',%s,' % t, end='')\n"                                               \
        "    if pathname and '\\\\' in pathname:\n"                                      \
        "      print ('%s' % pathname)\n"                                                \
        "      prev_pathname = pathname\n"                                               \
        "      prev_modtype  = t\n"                                                      \
        "      mod_paths [s] = pathname\n"                                               \
        "    else:\n"                                                                    \
        "      mod_builtins [s] = 1\n"                                                   \
        "  except ImportError:\n"                                                        \
        "    print (',<unknown>')\n"

/**
 * \todo
 *   Use 'python data-to-c.py file-with-PY_LIST_MODULES > generated-PY_LIST_MODULES.c' and a
 *   `#include "generated-PY_LIST_MODULES.c"` here instead.
 */

/**
 * Call the Python program and build up the `pi->modules` smartlist of the module information.
 *
 * If the Python is embedable, use `(*PyRun_SimpleString)()` directly.
 * Otherwise call the Python program via a temporary file in `popen_run_py()`.
 */
static void py_get_module_info (struct python_info *pi)
{
  char *line, *str = NULL;
  char *str2 = NULL;
  char *tok_end;

  if (opt.use_cache && smartlist_len(pi->modules) > 0)
     TRACE (1, "Calling %s() with a cache should not be neccesary.\n", __FUNCTION__);

  if (pi->is_embeddable)
  {
    if (!pi->bitness_ok || !py_init_embedding(pi))
       return;

    /**
     * Re-enable the catcher as `"sys.stdout = old_stdout"` was called
     * in above `test_python_funcs()`.
     */
    pi->catcher = setup_stdout_catcher();
    if (!pi->catcher)
    {
      WARN (" Failed to setup py_catcher.\n");
      return;
    }
    set_error_mode (0);
    str = call_python_func (pi, PY_LIST_MODULES(), __LINE__);
//  str2 = call_python_func (pi, PY_LIST_MODULES2(), __LINE__);
    ARGSUSED (str2);
    set_error_mode (1);
  }
  else
    str = popen_run_py (pi, PY_LIST_MODULES());

  if (str)
  {
    for (line = _strtok_r(str, "\n", &tok_end); line;
         line = _strtok_r(NULL, "\n", &tok_end))
    {
      /** The `print()` statement from `PY_LIST_MODULES()` should look like this:
       *  \code
       *    pygeoip,v.0.2.6,f:\programfiler\python27\lib\site-packages\pygeoip-0.2.6-py2.7.egg (ZIP),f:\programfiler\python27\lib\site-packages\pygeoip-0.2.6-py2.7.egg\EGG-INFO\PKG-INFO
       *  \endcode
       *
       * Split them into `module`, `version`, `path` and `meta` before adding
       * them to the `pi->modules` smartlist.
       */
      char module  [40+1]  = { "?" };
      char version [30+1]  = { "?" };
      char fname   [256+1] = { "?" };
      char meta    [256+1] = { "-" };
      int  num = sscanf (line, "%40[^;];%20[^;];%256[^;];%256s", module, version, fname, meta);

      if (num >= 3)  /* Watcom's 'sscanf()' is buggy */
      {
        struct python_module m;

        _strlcpy (m.name, module, sizeof(m.name));
        _strlcpy (m.version, version, sizeof(m.version));
        _strlcpy (m.location, fname, sizeof(m.location));
        _strlcpy (m.meta_path, "-", sizeof(m.meta_path));
        m.is_actual = m.is_zip = FALSE;
        if (str_endswith(fname," (ZIP)"))
        {
          m.location [strlen(m.location) - sizeof(" (ZIP)") + 1] = '\0';
          m.is_zip = TRUE;
        }
        if (meta[0] != '-')
        {
          _strlcpy (m.meta_path, meta, sizeof(m.meta_path));
          py_get_meta_info (&m);
        }
        py_add_module (pi, &m);
      }
      else
        TRACE (1, "Suspicious line: num: %d, '%s'\n", num, line);
    }
  }
  if (str == popen_out)
       popen_append_clear();
  else FREE (str);
}

/**
 * Check if a Python .DLL has the correct bitness for `LoadLibrary()`.
 */
static BOOL check_bitness (struct python_info *pi, char **needed_bits)
{
  if (sizeof(void*) == 4)
     our_bitness = bit_32;
  else if (sizeof(void*) == 8)
     our_bitness = bit_64;

  if (pi->bitness == bit_unknown)
     pi->bitness_ok = (pi->dll_name && check_if_PE(pi->dll_name, &pi->bitness));

  if (pi->bitness_ok && pi->bitness != our_bitness)
     pi->bitness_ok = FALSE;

  if (needed_bits)
     *needed_bits = (our_bitness == bit_32 ? "32" : "64");

  return (pi->bitness_ok);
}

/**
 * Parse the output line from the `PY_ZIP_LIST()` program below.
 * Each line on the form:
 * \code
 *    81053 20130327.164158 stem/control.py
 *    ^     ^
 *    size  time: YYYYMMDD.HHMMSS
 * \endcode
 */
static int report_zip_file (const char *zip_file, char *output)
{
  struct tm     tm;
  struct report r;
  const  char *space, *p, *file_within_zip;
  char   *q, report [1024];
  int    num;
  time_t mtime;
  long   fsize;

  space = strrchr (output, ' ');
  if (!space)
  {
    WARN (" (1) Unexpected 'zipfile' line: %s\n", output);
    return (0);
  }

  file_within_zip = space + 1;
  p = space - 1;
  while (p > output && *p != ' ')
      --p;

  if (p <= output)
  {
    WARN (" (2) Unexpected 'zipfile' line: %s\n", output);
    return (0);
  }

  p++;
  memset (&tm, 0, sizeof(tm));
  num = sscanf (output, "%ld %4u%2u%2u.%2u%2u%2u",
                &fsize, &tm.tm_year, &tm.tm_mon, &tm.tm_mday,
                        &tm.tm_hour, &tm.tm_min, &tm.tm_sec);
  if (num != 7)
  {
    WARN (" (3) Unexpected 'zipfile' line: %s\n", output);
    return (0);
  }

  tm.tm_year -= 1900;
  tm.tm_mon  -= 1;

  mtime = mktime (&tm);
  if (mtime == -1)
  {
    WARN (" (4) Unexpected timestamp: \"%.*s\".\n", (int)(space-p), p);
    TRACE (1, "parsed tm: %04u%02u%02u.%02u%02u%02u. num: %d\n",
           tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
           tm.tm_hour, tm.tm_min, tm.tm_sec, num);
    return (0);
  }

  /* The `zipfile` module always reports `file_within_zip` with forward slashes (`/`).
   */
  slashify2 (report, file_within_zip, opt.show_unix_paths ? '/' : '\\');
  q = strchr (report, '\0');
  snprintf (q, sizeof(report) - (q-report), "  (%s)", py_filename(zip_file));

  /** \todo incase `--pe-check` is specified and `report` file is a .pyd-file,
   *        we should save the .pyd to a `%TEMP`-file and examine it in `report_file()`.
   */
  r.file        = report;
  r.content     = opt.grep.content;
  r.mtime       = mtime;
  r.fsize       = fsize;
  r.is_dir      = FALSE;
  r.is_junction = FALSE;
  r.is_cwd      = FALSE;
  r.key         = HKEY_PYTHON_EGG;
  report_file (&r);
  return (1);
}

/**
 * List a ZIP/EGG-file for a matching `opt.file_spec`.
 *
 * \note
 *   We are not interested in the directory-part. Hence get the basename of
 *   `f.filename` first. Thus:
 *   \code
 *     EGG-INFO/requires.txt -> False
 *     egg-timer.txt         -> True
 *   \endcode
 *
 * \note
 *   `fnmatch.fnmatch ("EGG-INFO/dependency_links.txt", "egg*.txt")`
 *   will return True!
 *
 * \def PY_ZIP_LIST
 *   This text goes into a buffer used by `call_python_func()`.
 */
#define PY_ZIP_LIST()                                                               \
        "import os, sys, fnmatch, zipfile\n"                                        \
        "\n"                                                                        \
        "def trace (s):\n"                                                          \
        "  if %d >= 3:\n"                         /* opt.debug */                   \
        "    sys.stderr.write (s)\n"                                                \
        "\n"                                                                        \
        "def print_zip (f):\n"                                                      \
        "  base = os.path.basename (f.filename)\n"                                  \
        "  trace ('egg-file: %%s, base: %%s\\n' %% (f.filename, base))\n"           \
        "  if %d:\n"                              /* opt.case_sensitive */          \
        "    match = fnmatch.fnmatchcase\n"                                         \
        "  else:\n"                                                                 \
        "    match = fnmatch.fnmatch\n"                                             \
        "\n"                                                                        \
        "  file_spec = '%s'\n"                    /* opt.file_spec */               \
        "  if match(f.filename, file_spec) or match(base, file_spec):\n"            \
        "    date = \"%%4d%%02d%%02d\"  %% (f.date_time[0:3])\n"                    \
        "    time = \"%%02d%%02d%%02d\" %% (f.date_time[3:6])\n"                    \
        "    str = \"%%d %%s.%%s %%s\"  %% (f.file_size, date, time, f.filename)\n" \
        "    trace ('str: \"%%s\"\\n' %% str)\n"                                    \
        "    print (str)\n"                                                         \
        "\n"                                                                        \
        "zf = zipfile.ZipFile (r'%s', 'r')\n"   /* zfile */                         \
        "for f in zf.infolist():\n"                                                 \
        "  print_zip (f)\n"

static int process_zip (struct python_info *pi, const char *zfile)
{
  char  prog [sizeof(PY_ZIP_LIST()) + _MAX_PATH + 100];
  char *line, *tok_end, *str = NULL;
  int   found = 0;
  int   len = snprintf (prog, sizeof(prog), PY_ZIP_LIST(),
                        opt.debug, opt.case_sensitive, opt.file_spec, zfile);

  if (len < 0)
     FATAL ("`char prog[%d]` buffer too small. Approx. %d bytes needed.\n",
            (int)sizeof(prog), (int) (sizeof(PY_ZIP_LIST()) + _MAX_PATH));

  if (pi->is_embeddable)
       str = call_python_func (pi, prog, __LINE__);
  else str = popen_run_py (pi, prog);

#if 0
  /** \todo Add zip content to cache? */
  if (opt.use_cache)
  {
    snprintf (prog, sizeof(prog), PY_ZIP_LIST(), opt.debug, opt.case_sensitive, "*", zfile);
    for (i,z) in enumerate(zip_content):
      snprintf (format, sizeof(format), "python_zip%d_%d = %%s,%%s", py_index, i);
      cache_putf (SECTION_PYTHON, format, zfile, z);
  }
#endif

  if (str)
  {
    for (found = 0, line = _strtok_r(str, "\n", &tok_end);
         line;
         line = _strtok_r(NULL, "\n", &tok_end), found++)
    {
      TRACE (2, "line: \"%s\", found: %d\n", line, found);

      if (!strncmp(line,"str: ",5))   /* if opt.debug >= 3; ignore these from stderr */
         continue;
      if (!report_zip_file(zfile, line))
         break;
#if 0
      if (opt.verbose)
         py_print_modinfo (opt.file_spec, TRUE);
#endif
    }
  }
  popen_append_clear();

  if (str && found == 0)
     TRACE (1, "No matches in %s for %s.\n", zfile, opt.file_spec);

  FREE (str);
  return (found);
}

/**
 * Check if the `module` is already in the `pi->modules` smartlist.
 */
static BOOL py_module_found (const struct python_info *pi, const char *module)
{
  int i, max = smartlist_len (pi->modules);

  for (i = 0; i < max; i++)
  {
    const struct python_module *m = smartlist_get (pi->modules, i);

    if (!stricmp(m->name,module))
       return (TRUE);
  }
  return (FALSE);
}

/**
 * If `m->name` is not already in the `pi->modules` smartlist, allocate and copy it.
 */
static BOOL py_add_module (struct python_info *pi, const struct python_module *m)
{
  if (!py_module_found(pi, m->name))
  {
    struct python_module *m2 = MALLOC (sizeof(*m2));

    memcpy (m2, m, sizeof(*m2));
    smartlist_add (pi->modules, m2);
    return (TRUE);
  }
  TRACE (1, "module '%s' for '%s' already added!\n", m->name, pi->program);
  return (FALSE);
}

/**
 * Check if the `dir` is already in the `sys.path[]` smartlist.
 * Compare the `dir` with no case-sensitivity.
 *
 * Most often only the `site-packages` directory gets duplicated with different casings:
 * \code
 *   f:\ProgramFiler\Python36\lib\site-packages
 *   f:\programfiler\Python36\lib\site-packages
 * \endcode
 */
static BOOL py_path_found (const struct python_info *pi, const char *dir)
{
  int i, max = smartlist_len (pi->sys_path);

  for (i = 0; i < max; i++)
  {
    const struct python_path *pp = smartlist_get (pi->sys_path, i);

    if (!stricmp(pp->dir,dir))
       return (TRUE);
  }
  return (FALSE);
}

/**
 * Allocate a `python_path` node and add it to `pi->sys_path[]` smartlist.
 * Not called if `opt.use_cache == TRUE` and something found in file-cache.
 */
static void add_sys_path (const char *dir)
{
  if (!py_path_found(g_pi, dir))
  {
    struct python_path *pp = CALLOC (1, sizeof(*pp));
    struct stat st;

    _strlcpy (pp->dir, dir, sizeof(pp->dir));
    memset (&st, '\0', sizeof(st));
    pp->exist  = (stat(dir, &st) == 0);
    pp->is_dir = pp->exist && _S_ISDIR(st.st_mode);
    pp->is_zip = pp->exist && _S_ISREG(st.st_mode) && check_if_zip (dir);
    smartlist_add (g_pi->sys_path, pp);
  }
}

/**
 * Build up the `g_pi->sys_path[]` array.
 *
 * \param[in] str    the string to add to `g_pi->sys_path[]`.
 * \param[in] index  `index == -1`  we are called from `call_python_func()`.
 *                   `index != -1`  we are the `popen_run()` callback.
 */
static int build_sys_path (char *str, int index)
{
  if (index == -1)
  {
    char *tok_end, *tok = _strtok_r (str, "\n", &tok_end);

    for (index = 0; tok; index++)
    {
      TRACE (2, "index: %d: \"%s\"\n", index, tok);
      add_sys_path (tok);
      tok = _strtok_r (NULL, "\n", &tok_end);
    }
  }
  else
  {
    TRACE (2, "index: %d: \"%s\"\n", index, str);
    add_sys_path (str);
  }
  return (1);
}

/**
 * \def PY_GET_VERSION
 *   Run python with this on the command-line to get the version triplet.<br>
 *   Also in the same command, print the `user-site` path.
 *
 * For a MSVC-built Python, the below `os.name` will yield `nt`. Thus the command:
 *   \code
 *     print (sysconfig.get_path('purelib', 'nt'))
 *   \endcode
 *
 * will in my case print:
 *   \code
 *     c:\Users\Gisle\AppData\Roaming\Python\Python27\site-packages
 *   \endcode
 *
 * But for a Cygwin-built Python, `os.name` will yield `posix` and thus print:
 *   \code
 *    /cygdrive/c/Users/Gisle/AppData/Roaming/.local/lib/python2.7/site-packages
 *   \endcode
 *
 * \note
 *   The `site-packages` directory need not exist. It's a result of the
 *   hardcoded rules in `<PYTHON_ROOT>/lib/sysconfig.py`.\n
 *   E.g my `pypy` reports:
 *   \code
 *     c:\Users\Gisle\AppData\Roaming\Python\PyPy27\site-packages
 *   \endcode
 *
 *   which does *not* exists.
 */
#define PY_GET_VERSION()  "import os, sys, sysconfig; " \
                          "print (sys.version_info); "  \
                          "print (sysconfig.get_path('purelib', '%s_user' % os.name))"

/**
 * \def PY_PRINT_SYS_PATH_DIRECT
 *   The code which is used if `py->is_embeddable == TRUE`.
 *   Used in the parameter to `call_python_func()`.
 */
#define PY_PRINT_SYS_PATH_DIRECT()  "import sys\n" \
                                    "for (i,p) in enumerate(sys.path):\n" \
                                    "  print ('%s' % p)\n"

/**
 * \def PY_PRINT_SYS_PATH2_CMD
 *   Used in Python2 when `py->is_embeddable == FALSE`.
 */
#define PY_PRINT_SYS_PATH2_CMD()  "import os, sys; " \
                                  "[os.write(1,'%s\\n' % p) for (i,p) in enumerate(sys.path)]"

/**
 * \def PY_PRINT_SYS_PATH3_CMD
 *   Used in Python3 when `py->is_embeddable == FALSE`.
 */
#define PY_PRINT_SYS_PATH3_CMD()  "import sys; " \
                                  "[print(p) for (i,p) in enumerate(sys.path)]"

/**
 * \todo
 *   CygWin's Python doesn't like the `;` and `\\` in `%PYTHONPATH`.
 *   Try to detect Cygwin and please it before calling popen_run().
 *   Do something like `cygwin_create_path(CCP_WIN_A_TO_POSIX, dir)`
 *   does in CygWin.
 */
static void get_sys_path (const struct python_info *pi)
{
  if (smartlist_len(pi->sys_path) > 0)  /* We have this from cache */
     return;

  ASSERT (pi == g_pi);
  popen_run (build_sys_path, pi->exe_name, "-c \"%s\"",
             pi->ver_major >= 3 ? PY_PRINT_SYS_PATH3_CMD() : PY_PRINT_SYS_PATH2_CMD());
}

/**
 * \todo
 *   If multiple DLLs with same name but different time-stamps are found
 *   (in `pi->exe_dir` and `sys_dir`), report a warning.
 *   Check PE-version and/or MD5 finger-print?
 */
static int get_dll_name (struct python_info *pi, const char **libs)
{
  const char *lib_fmt;
  char        dll1 [_MAX_PATH] = { '\0' };
  char        dll2 [_MAX_PATH] = { '\0' };
  const char *use_this = NULL;
  const char *newest   = NULL;
  struct stat st1, st2, *use_st = NULL;
  BOOL       _st1, _st2, equal;
  size_t      i, len, num = DIM (pi->libraries);

  _st1 = _st2 = FALSE;

  for (i = 0, lib_fmt = libs[0]; lib_fmt && i < num; lib_fmt = libs[++i])
  {
    if (!strncmp(lib_fmt,"%s\\",3))
    {
      snprintf (dll1, sizeof(dll1), lib_fmt, pi->exe_dir, pi->ver_major, pi->ver_minor);
      snprintf (dll2, sizeof(dll2), lib_fmt, sys_dir, pi->ver_major, pi->ver_minor);
    }
    else if (!strncmp(lib_fmt,"~\\",2))
    {
      len = strlen (dll1);
      strcpy (dll1, pi->exe_dir);
      snprintf (dll1+len, sizeof(dll1)-len, lib_fmt+1, pi->ver_major, pi->ver_minor);
      dll2[0] = '\0';
    }
    else
    {
      snprintf (dll1, sizeof(dll1), lib_fmt, pi->ver_major, pi->ver_minor);
      dll2[0] = '\0';
    }

    TRACE (1, "checking for:\n"
              "             dll1: \"%s\"\n"
              "             dll2: \"%s\"\n", dll1, dll2);

    if (dll1[0] && FILE_EXISTS(dll1))
    {
      _st1 = (stat(dll1, &st1) == 0);
      use_this = dll1;
      use_st = &st1;
    }
    if (dll2[0] && FILE_EXISTS(dll2))
    {
      _st2 = (stat(dll2, &st2) == 0);
      use_this = dll2;
      use_st = &st2;
    }

    if (use_this)
       break;
  }

  if (_st1 && _st2)
  {
    equal = (st1.st_mtime == st2.st_mtime) && (st1.st_size == st2.st_size);
    if (equal)
    {
      newest = dll1;    /* The one in `exe_dir` */
      use_st = &st1;
    }
    else
    {
      newest = (st1.st_mtime > st2.st_mtime) ? dll1 : dll2;
      use_st = (st1.st_mtime > st2.st_mtime) ? &st1 : &st2;
      WARN ("%s and %s have different sizes and/or time-stamps. Using newest %s.\n",
            dll1, dll2, newest);
    }
  }
  else
    newest = use_this;

  if (newest)
  {
    pi->dll_name = _fix_path (newest, NULL);
    TRACE (1, "Found newest DLL: \"%s\", %s\n", newest, get_time_str(use_st->st_mtime));
  }
  return (newest != NULL);
}

/**
 * Run a Python, figure out the `sys.path[]` array and search along that
 * for matches. If a `sys.path[]` component contains a ZIP/EGG-file, use
 * `process_zip()` to list files inside it for a match.
 *
 * \note not all .EGG-files are ZIP-files. `check_if_zip()` is used to test
 *       that and set `pp->is_zip` accordingly.
 */
static int py_search_internal (struct python_info *pi, BOOL reinit)
{
  char *str = NULL;
  int   i, max, found;
  BOOL  use_cache = (opt.use_cache && smartlist_len(pi->modules) > 0);

  ASSERT (pi == g_pi);

  TRACE (1, "pi->variant: %d.\n", pi->variant);

  if (pi->is_embeddable)
  {
    if (reinit && !py_init_embedding(pi))
       return (0);

    /* We have this from the file-cache.
     */
    if (!use_cache)
    {
      str = call_python_func (pi, PY_PRINT_SYS_PATH_DIRECT(), __LINE__);
      if (!str)
         return (0);

      build_sys_path (str, -1);
      FREE (str);
    }
  }
  else if (!use_cache)
  {
    get_sys_path (pi);
  }

  found = 0;
  max = smartlist_len (pi->sys_path);

  for (i = 0; i < max; i++)
  {
    struct python_path *pp = smartlist_get (pi->sys_path, i);
    int    rc;

    /* Don't warn on missing .zip files in `sys.path[]` (unless in debug-mode)
     */
    if (opt.debug == 0 && !pp->exist && !stricmp(get_file_ext(pp->dir), "zip"))
       pp->exist = pp->is_dir = TRUE;

    if (pp->is_zip)
         rc = process_zip (pi, pp->dir);
    else rc = process_dir (pp->dir, 0, pp->exist, FALSE, pp->is_dir,
                           TRUE, "sys.path[]", HKEY_PYTHON_PATH, FALSE);
    found += rc;

#if 1
    /* If there was a hit for `opt.file_spec` along the `sys.path[]`, this could also
     * be a hit for a matching module.
     */
    if (rc >= 1 && opt.verbose)
    {
      C_printf ("  ~2Getting module-info matching ~6%s~0\n", opt.file_spec);
      py_print_modinfo (opt.file_spec, TRUE);
    }
#endif
  }
  return (found);
}

/**
 * Find the next Python that can be selected.
 */
static struct python_info *py_select_next (enum python_variants which)
{
  struct python_info *pi = NULL;
  int    i, max;
  BOOL   okay = FALSE;

  if (py_index == -1)
  {
    TRACE (1, "py_index: -1.\n");
    return (NULL);
  }

  max = smartlist_len (py_programs);

  for (i = py_index; i < max; i++)
  {
    pi = smartlist_get (py_programs, i);
    okay = (which == ALL_PYTHONS || pi->variant == which ||
            (which == DEFAULT_PYTHON && pi->is_default)) &&
           pi->exe_name;

    if (which == ALL_PYTHONS)    /* select the `ALL_PYTHONS` only once */
       which = pi->variant;

    TRACE (1, "py_index: %d: which: %d/%s, pi->variant: %d/%s, okay: %d.\n",
           py_index, which, py_variant_name(which), pi->variant, pi->exe_name, okay);

    if (okay)
       break;
  }

  /* Prepare for next call of `py_select_next()`.
   */
  if (!okay)
       py_index = -1;
  else py_index = i + 1;
  return (okay ? pi : NULL);
}

/**
 * Find the first Python that can be selected.
 */
static struct python_info *py_select_first (enum python_variants which)
{
  py_index = 0;
  return py_select_next (which);
}

/**
 * The public function for searching for a Python file-spec.
 *
 * Sets the global `g_pi` to the first suitable Python or one specified with the
 * `envtool --py=X` option. In case `X=all` (`py_which == ALL_PYTHONS`), this
 * function will loop over all found Pythons and do a search for each file-spec.
 */
int py_search (void)
{
  struct python_info *pi;
  int    i = 0, found = 0;

  for (pi = py_select_first(py_which);  /* if the 1st was okay, try the next too */
       pi;
       pi = py_select_next(py_which), i++)
  {
    BOOL reinit = (g_pi == pi) ? FALSE : TRUE; /* Need to call py_init_embedding() again? */

    report_header_set ("Matches in \"%s\" sys.path[]:\n", pi->exe_name);
    g_pi = pi;
    found += py_search_internal (pi, reinit);
  }

  if (i == 0)
  {
    if (py_which == ALL_PYTHONS)
         WARN ("No Pythons were found on PATH.\n");
    else WARN ("%s was not found on PATH.\n", py_variant_name(py_which));
  }
  return (found);
}

/**
 * Allocate a new `python_info` node, fill it and return it
 * for adding to the `py_programs` smartlist.
 */
static struct python_info *add_python (const struct python_info *pi, const char *exe)
{
  struct python_info *pi2 = CALLOC (sizeof(*pi2), 1);
  const char         *base = basename (exe);
  const char       **libs;

  _strlcpy (pi2->exe_dir, exe, base - exe);
  _strlcpy (pi2->program, pi->program, sizeof(pi2->program));
  pi2->exe_name = _fix_path (exe, NULL);
  pi2->dll_hnd  = INVALID_HANDLE_VALUE;
  pi2->do_warn_home      = TRUE;
  pi2->do_warn_user_site = TRUE;
  pi2->sys_path = smartlist_new();

  if (get_python_version(exe) >= 1)
  {
    pi2->ver_major = tmp_ver_major;
    pi2->ver_minor = tmp_ver_minor;
    pi2->ver_micro = tmp_ver_micro;
    pi2->user_site_path = tmp_user_site[0] ? STRDUP(tmp_user_site) : NULL;

    libs = (const char**) pi->libraries;
    if (get_dll_name(pi2, libs))
    {
     /** If embeddable, test the bitness of the .DLL to check
      *  if `LoadLibrary()` will succeed.
      */
      pi2->is_embeddable = pi->is_embeddable;
      if (pi2->is_embeddable)
         check_bitness (pi2, NULL);

      fix_python_variant (pi2, pi->variant);
      set_python_home (pi2);
      set_python_prog (pi2);
    }
  }
  return (pi2);
}

/**
 * For each directory in the `%PATH`, try to match a Python from the
 * ones in `all_py_programs[]`.
 *
 * If it's found in the ignore-list (by `cfg_ignore_lookup()`) do not add it.<br>
 * Figure out it's version and .DLL-name (if it's embeddable).
 */
static int match_python_exe (const char *dir)
{
  struct od2x_options opts;
  struct dirent2     *de;
  const char *base;
  DIR2       *dp;
  int         i, rc = 1;
  int         dbg_save = opt.debug;
  static int  found = 0;

  if (opt.debug >= 3)
     opt.debug = 1;  /* relax the trace in here */

  memset (&opts, '\0', sizeof(opts));

  /* We cannot simply use "python*.exe" since we care about "ipy*.exe" etc. too
   */
  opts.pattern = "*.exe";

  dp = opendir2x (dir, &opts);
  if (!dp)
  {
    opt.debug = dbg_save;
    return (rc);
  }

  while ((de = readdir2(dp)) != NULL)
  {
    const struct python_info *all_pi;
    struct python_info       *pi;

    if ((de->d_attrib & FILE_ATTRIBUTE_DIRECTORY) ||
        (de->d_attrib & FILE_ATTRIBUTE_DEVICE))
      continue;

    base = basename (de->d_name);
    for (i = 0, all_pi = all_py_programs; i < DIM(all_py_programs); all_pi++, i++)
    {
      if (stricmp(base, all_pi->program) != 0)
         continue;

      if (cfg_ignore_lookup("[Python]",de->d_name))
         continue;

      found++;
      TRACE (1, "de->d_name: %s matches: '%s', variant: %d\n",
             de->d_name, all_pi->program, all_pi->variant);

      pi = add_python (all_pi, de->d_name);

      /* First Python found is default.
       */
      if (found == 1)
         pi->is_default = TRUE;

      smartlist_add (py_programs, pi);

      /* If we specified `envtool -V`, just show the first Python found.
       */
      if (opt.do_version == 1 && !opt.use_cache)
         rc = 0;

      /* If we found all Pythons we can handle, there is no point searching
       * further along the PATH. Stop by returning 0.
       */
      if (found >= DIM(all_py_programs))
         rc = 0;
      break;
    }
  }
  closedir2 (dp);
  opt.debug = dbg_save;
  return (rc);
}

/**
 * Build up the `pi->modules` list from the file-cache.
 */
static int get_modules_from_cache (struct python_info *pi)
{
  int i = 0;
  int dups = 0;

  if (!opt.use_cache)
     return (0);

  while (1)
  {
    struct python_module m;
    char   format [50];
    char  *mod_name, *mod_version, *location, *meta_path;
    int    rc, is_zip;

    snprintf (format, sizeof(format), "python_modules%d_%d = %%s,%%s,%%d,%%s,%%s", pi->py_index, i);
    rc = cache_getf (SECTION_PYTHON, format, &mod_name, &mod_version, &is_zip, &location, &meta_path);
    TRACE (2, "rc: %d.\n", rc);

    if (rc < 5)
       break;

    m.is_zip    = is_zip;
    m.is_actual = TRUE;
    _strlcpy (m.name, mod_name, sizeof(m.name));
    _strlcpy (m.version, mod_version, sizeof(m.version));
    _strlcpy (m.location, location, sizeof(m.location));
    _strlcpy (m.meta_path, meta_path, sizeof(m.meta_path));
    if (!py_add_module(pi, &m))
       dups++;
    i++;
  }
  return (i - dups);
}

/**
 * Try to get all Python information from the cache.
 */
static BOOL get_pythons_from_cache (void)
{
  struct python_info *pi;
  int    i, j, found;

  if (!opt.use_cache)
     return (FALSE);

  i = found = 0;

  while (1)
  {
    char   format [50];
    char  *py_prog, *py_exe, *py_dll, *version, *user_site;
    char  *base;
    int    rc1, rc2, variant, is_default, is_embeddable, bitness, num_modules;

    /* This MUST match the format used in `cache_putf()`.
     */
    snprintf (format, sizeof(format), "pythons_on_path%d = %%s,%%s,%%s,%%d,%%d,%%d,%%d,%%d,%%s,%%s", i);

    rc1 = cache_getf (SECTION_PYTHON, format,
                      &py_prog, &py_exe, &py_dll, &variant, &is_default, &num_modules,
                      &is_embeddable, &bitness, &version, &user_site);
    if (rc1 != 10)
       break;

    TRACE (1, "rc1: %d\n     py_prog: '%s', py_exe: '%s', py_dll: '%s', variant: %d, "
              "is_default: %d, num_modules: %d, is_embeddable: %d, bitness: %d, version: '%s', user_site: '%s'.\n",
           rc1, py_prog, py_exe, py_dll, variant, is_default, num_modules, is_embeddable, bitness, version, user_site);

    found++;

    pi = CALLOC (sizeof(*pi), 1);
    rc2 = sscanf (version, "(%d.%d.%d)", &pi->ver_major, &pi->ver_minor, &pi->ver_micro);
    TRACE (1, "rc2: %d. ver: %d.%d.%d\n", rc2, pi->ver_major, pi->ver_minor, pi->ver_micro);

    base = basename (py_exe);
    _strlcpy (pi->exe_dir, py_exe, base - py_exe);
    _strlcpy (pi->program, py_prog, sizeof(pi->program));
    pi->exe_name       = STRDUP (py_exe);
    pi->dll_name       = STRDUP (py_dll);
    pi->user_site_path = STRDUP (user_site);
    pi->variant        = variant;
    pi->is_default     = is_default;
    pi->is_embeddable  = is_embeddable;
    pi->bitness        = (bitness == 0) ? bit_unknown : (bitness == 32 ? bit_32 : bit_64);
    pi->sys_path       = smartlist_new();

    set_python_home (pi);
    set_python_prog (pi);

    if (pi->is_embeddable)
    {
      pi->bitness_ok = TRUE;  /* assume yes */
      check_bitness (pi, NULL);
    }

    smartlist_add (py_programs, pi);

    /* Get the 'sys.path[]' list for this Python.
     */
    j = 0;
    while (1)
    {
      struct python_path *pp;
      int    exist = 0, is_dir = 0, is_zip = 0;
      char  *py_dir = NULL;

      snprintf (format, sizeof(format), "python_path%d_%d = %%d,%%d,%%d,%%s", i, j);
      rc1 = cache_getf (SECTION_PYTHON, format, &exist, &is_dir, &is_zip, &py_dir);
      if (rc1 != 4)
         break;

      pp = MALLOC (sizeof(*pp));
      pp->exist  = exist;
      pp->is_dir = is_dir;
      pp->is_zip = is_zip;
      _strlcpy (pp->dir, py_dir, sizeof(pp->dir));
      smartlist_add (pi->sys_path, pp);
      j++;
    }
    i++;    /* Get the next cached 'pythons_on_path' */
  }
  return (found > 0);
}

/*
 * Write all collected information for a single Python back to the file-cache.
 */
static void write_to_cache (const struct python_info *pi)
{
  int  i, bitness, max;
  char version [30];

  if (!opt.use_cache)
     return;

  snprintf (version, sizeof(version), "(%d.%d.%d)", pi->ver_major, pi->ver_minor, pi->ver_micro);

  switch (pi->bitness)
  {
    case bit_32:
         bitness = 32;
         break;
    case bit_64:
         bitness = 64;
         break;
    default:
         bitness = 0;
         break;
  }

  max = pi->modules ? smartlist_len (pi->modules) : 0;

  cache_putf (SECTION_PYTHON, "pythons_on_path%d = %s,%s,%s,%d,%d,%d,%d,%d,%s,%s",
              pi->py_index, pi->program, pi->exe_name, pi->dll_name, pi->variant,
              pi->is_default, max, pi->is_embeddable, bitness,
              version, pi->user_site_path);

  for (i = 0; i < max; i++)
  {
    const struct python_module *m = smartlist_get (pi->modules, i);
    const char  *meta_path = "-";

    if (m->meta_path[0] != '-')
    {
      if (m->is_zip || m->is_actual)
           meta_path = m->meta_path;
      else meta_path = py_filename (m->meta_path);
    }
    cache_putf (SECTION_PYTHON, "python_modules%d_%d = %s,%s,%d,%s,%s",
                pi->py_index, i, m->name, m->version, m->is_zip, m->location, meta_path);
  }

  max = smartlist_len (pi->sys_path);
  if (max == 0)
  {
    TRACE (2, "No cached sys_path[] for %s.\n", pi->exe_name);
    g_pi = (struct python_info*) pi;
    get_sys_path (pi);
  }

  max = smartlist_len (pi->sys_path);
  for (i = 0; i < max; i++)
  {
    const struct python_path *pp = smartlist_get (pi->sys_path, i);

    cache_putf (SECTION_PYTHON, "python_path%d_%d = %d,%d,%d,%s",
                pi->py_index, i, pp->exist, pp->is_dir, pp->is_zip, pp->dir);
  }
}

/**
 * Search all directories on `%PATH` for matches to `all_py_programs::program`.
 */
static void enum_pythons_on_path (void)
{
  char *dir, *tok_end, path_sep[2] = ";";
  char *path = getenv_expand ("PATH");

  for (dir = _strtok_r(path, path_sep, &tok_end); dir;
       dir = _strtok_r(NULL, path_sep, &tok_end))
  {
    slashify2 (dir, dir, DIR_SEP);
    if (!match_python_exe(dir))
       break;
  }
  FREE (path);
}

/**
 * Do some tests on a single Python.
 */
static void py_test_internal (struct python_info *pi)
{
  BOOL reinit = (g_pi == pi) ? FALSE : TRUE; /* Need to call py_init_embedding() again? */

  g_pi = pi;
  get_sys_path (pi);
  C_puts ("Python paths:\n");
  print_sys_path (pi, 0);

  if (reinit && pi->is_embeddable && !test_python_funcs(pi,reinit))
     C_puts ("Embedding failed.");

  C_printf ("~6List of modules for %s:~0\n", py_filename(pi->exe_name));
  py_print_modinfo ("*", TRUE);
  C_putc ('\n');
}

/**
 * Loop over \ref all_py_programs[] and do some tests on a Python matching \ref py_which.
 *
 * This can be the `default`, one specific Python in \ref python_variants or `all`.
 * This must be called after py_init() has been called.
 */
int py_test (void)
{
  struct python_info *pi;
  int    i = 0;

  for (pi = py_select_first(py_which);   /* if the 1st was okay, try the next too */
       pi;
       pi = py_select_next(py_which), i++)
  {
    C_printf ("~6Will try to test: ~3%s~0%s (%sembeddable): %s\n",
              py_variant_name(pi->variant),
              pi->is_default    ? " ~6(Default)~0," : "",
              pi->is_embeddable ? ""                : "not ",
              py_filename(pi->exe_name));
    py_test_internal (pi);
  }

  if (i == 0)
  {
    if (py_which == ALL_PYTHONS)
         WARN ("No Pythons were found on PATH.\n");
    else WARN ("%s was not found on PATH.\n", py_variant_name(py_which));
  }
  return (i);
}

/**
 * `popen_run()` callback to get the Python version.
 *
 * \param[in] output  the string from `popen_run()`.
 * \param[in] line    the line-number from `popen_run()`.
 */
static int report_py_version_cb (char *output, int line)
{
  const char *prefix = "sys.version_info";
  int   num;

  if (line == 1)  /* the 2nd line */
  {
    TRACE (1, "line: %d, output: '%s'\n", line, output);
    _strlcpy (tmp_user_site, output, sizeof(tmp_user_site));
    return (1);
  }

  /** `pypy.exe -c "import sys; print (sys.version_info)"` doesn't print the `prefix`.
   * But simply the result. Like: `(major=2, minor=7, micro=9, releaselevel='final', serial=42)`.
   */
  if (!strncmp(output, prefix, strlen(prefix)))
     output += strlen (prefix);

  num = sscanf (output, "(major=%d, minor=%d, micro=%d",
                &tmp_ver_major, &tmp_ver_minor, &tmp_ver_micro);
  TRACE (1, "Python ver: %d.%d.%d\n", tmp_ver_major, tmp_ver_minor, tmp_ver_micro);
  return  (num >= 2);
}

/**
 * Get the Python version and `user-site` path by spawning the program and
 * parsing the `popen()` output.
 */
static int get_python_version (const char *exe_name)
{
  tmp_ver_major = tmp_ver_minor = tmp_ver_micro = -1;
  tmp_user_site[0] = '\0';
  return (popen_run(report_py_version_cb, exe_name, "-c \"%s\"", PY_GET_VERSION()) >= 1);
}

/**
 * Called from `show_version()`:
 *   \li Print information for all found Pythons if `envtool -VV` was specified.
 *   \li Print the `sys_path[]` if `envtool -VVV` was specified.
 */
void py_searchpaths (void)
{
  struct python_info *pi;
  const char *ignored;
  char  fname [_MAX_PATH] = { '\0' };
  int   i, num = 0, max = smartlist_len (py_programs);
  char  slash  = (opt.show_unix_paths ? '/' : '\\');

  for (i = 0; i < max; i++)
  {
    char  version [12] = { '\0' };
    char *bitness = "?";

    pi = smartlist_get (py_programs, i);

    if (pi->ver_major > -1 && pi->ver_minor > -1 && pi->ver_micro > -1)
         snprintf (version, sizeof(version), "(%d.%d.%d)", pi->ver_major, pi->ver_minor, pi->ver_micro);
    else if (pi->exe_name)
         _strlcpy (version, "(ver: ?)", sizeof(version));

    if (pi->exe_name)
    {
      slashify2 (fname, pi->exe_name, slash);
      num++;
    }

    if (i == 0)
      C_putc ('\n');

    C_printf ("    %-*s %-*s -> ~%c%s~0",
              longest_py_program, pi->program,
              longest_py_version, version,
              fname[0] ? '6' : '5',
              fname[0] ? fname : "Not found");

    if (pi->is_embeddable && !check_bitness(pi,&bitness))
       C_printf (" (embeddable, but not %s-bit)", bitness);
    else if (pi->dll_name)
       C_printf (" (%sembeddable)", pi->is_embeddable ? "": "not ");

    if (pi->is_default)
       C_puts ("  ~3(1)~0");
    C_putc ('\n');

    if (pi->exe_name && opt.do_version >= 3)
    {
      print_home_path (pi, 12);
      print_user_site_path (pi, 12);
      g_pi = pi;
      get_sys_path (pi);
      print_sys_path (pi, 12);
    }
  }

  if (num > 0)
       C_puts ("    ~3(1)~0 Default Python (first found on PATH).\n");
  else C_puts ("    <None>.\n");

  /* Show the Python that were ignored.
   */
  for (i = 0, ignored = cfg_ignore_first("[Python]");
       ignored;
       ignored = cfg_ignore_next("[Python]"), i++)
  {
    if (i == 0)
       C_puts ("\n    Ignored:\n");
    slashify2 (fname, ignored, slash);
    C_printf ("      %s\n", fname);
  }
}

/**
 * Add the `REG_SZ` data in a `"HKLM\Software\Python\PythonCore\xx\InstallPath"` key
 * to the \ref py_programs smartlist.
 */
static void get_install_path (const struct python_info *pi, const char *key_name)
{
  HKEY  key = NULL;
  DWORD rc  = RegOpenKeyEx (HKEY_LOCAL_MACHINE, key_name, 0, reg_read_access(), &key);
  DWORD num = 0;

  while (rc == ERROR_SUCCESS)
  {
    char  value [512] = { '\0' };
    char  data [512]  = { '\0' };
    DWORD value_size  = sizeof(value);
    DWORD data_size   = sizeof(data);
    DWORD type        = REG_NONE;

    rc = RegEnumValue (key, num++, value, &value_size, NULL, &type, (LPBYTE)&data, &data_size);
    if (rc == ERROR_NO_MORE_ITEMS)
       break;

    if (type != REG_SZ)
       continue;

    TRACE (2, "   value: \"%s\", data: \"%s\"\n",
           value[0] ? value : "(Standard)", data[0] ? data : "(no data)");

    if (value[0] && data[0] && !stricmp(value,"ExecutablePath"))
    {
      struct python_info *pi2;
      char  *slash = strrchr (data, '\\');
      char  *end   = strchr (data,'\0') - 1;

      if (slash)
      {
        pi2 = CALLOC (sizeof(*pi2), 1);
        pi2->ver_major = pi->ver_major;
        pi2->ver_minor = pi->ver_minor;
        _strlcpy (pi2->exe_dir, data, end-data);
        _strlcpy (pi2->program, slash+1, slash-data);
        pi2->exe_name = STRDUP (data);
        pi2->sys_path = smartlist_new();
        smartlist_add (py_programs, pi2);
      }
    }
    else if (data[0] && !value[0])
    {
      struct python_info *pi2 = CALLOC (sizeof(*pi2), 1);
      char  *end = strchr (data,'\0') - 1;

      if (*end == '\\')
         *end = '\0';

      pi2->ver_major = pi->ver_major;
      pi2->ver_minor = pi->ver_minor;
      _strlcpy (pi2->exe_dir, data, sizeof(pi2->exe_dir));
      _strlcpy (pi2->program, "python.exe", sizeof(pi2->program));
      snprintf (data, sizeof(data), "%s\\%s", pi2->exe_dir, pi2->program);
      pi2->exe_name = STRDUP (data);
      pi2->sys_path = smartlist_new();
      smartlist_add (py_programs, pi2);
    }
  }
  if (key)
     RegCloseKey (key);
}

/**
 * Recursively walks the Registry branch under `HKLM\\ + <key_name>`. <br>
 * Look for `"InstallPath"` keys and gather the `REG_SZ / "InstallPath"` values.
 *
 * \param[in] key_name  On first call, this is always `"Software\\Python\\PythonCore"`.
 */
void enum_python_in_registry (const char *key_name)
{
  static struct python_info pi;   /* filled in sscanf() below */
  static int rec_level = 0;       /* recursion level */
  HKEY  key = NULL;
  DWORD rc, num = 0;

  rc = RegOpenKeyEx (HKEY_LOCAL_MACHINE, key_name, 0, reg_read_access(), &key);

  TRACE (2, "RegOpenKeyEx (HKLM\\%s)\n", key_name);

  while (rc == ERROR_SUCCESS)
  {
    char  sub_key [700];
    char  value [512];
    DWORD value_size = sizeof(value);
    char  bitness[3] = { '\0' };

    rc = RegEnumKeyEx (key, num++, value, &value_size, NULL, NULL, NULL, NULL);
    if (rc == ERROR_NO_MORE_ITEMS)
       break;

    snprintf (sub_key, sizeof(sub_key), "%s\\%s", key_name, value);
    TRACE (2, " rec_level %d, num %lu, value: '%s'\n"
              "                     sub_key: '%s'\n",
           rec_level, (unsigned long)num-1, value, sub_key);

    if (sscanf(value,"%d.%d-%2s", &pi.ver_major, &pi.ver_minor, bitness) >= 2)
    {
      if (bitness[0] == '6' && bitness[1] == '4' )
           pi.bitness = bit_64;
      else pi.bitness = bit_32;
      TRACE (2, " ver: %d.%d, bitness:s %d\n", pi.ver_major, pi.ver_major, pi.bitness);
    }
    else
    if (!stricmp(value,"InstallPath"))
       get_install_path (&pi, sub_key);

    rec_level++;
    enum_python_in_registry (sub_key);
    rec_level--;
  }

  if (key)
     RegCloseKey (key);
}

/**
 * Main initialiser function for this module:
 *  \li If `opt.use_cache == TRUE` get previous information from file-cache.
 *  \li If nothing found from cache:
 *    \li Find the details of all supported Pythons in `all_py_programs`.
 *    \li Walk the `%PATH` (and Registry; not yet) to find this information.
 *  \li Add each Python found to the `py_programs` smartlist as they are found.
 *
 * When the above is done, loop over all wanted Pythons
 * (e.g. if `py_which == ALL_PYTHONS`) and figure out the list of modules/packages
 * for each.
 *
 * \note The `sys.path[]` entries are not collected here. But in
 *       `py_search_internal()` when that information is needed.
 */
void py_init (void)
{
  size_t f_len = strlen (__FILE());
  int    i, max;

#if !defined(_DEBUG)
  if (exc_hnd == NULL)
  {
    exc_hnd = LoadLibrary ("exc-abort.dll");
    GetLastError();
    TRACE (2, "LoadLibrary (\"exc-abort.dll\"): hnd: %p\n", exc_hnd);
  }
#endif

  py_programs = smartlist_new();

  if (!get_pythons_from_cache())
     enum_pythons_on_path();

#if 0  /** \todo */
  enum_python_in_registry ("Software\\Python\\PythonCore");
#endif

  TRACE (1, "------------------------------------------------------------------\n"
         "py_which: %d/%s\n\n", py_which, py_variant_name(py_which));

  max = smartlist_len (py_programs);

  for (i = 0; i < max; i++)
  {
    struct python_info *pi = smartlist_get (py_programs, i);
    int    len, indent = 1 + (int)f_len;
    char  *num_mod;
    char   mod_buf [20];
    char   version [20];

    pi->py_index = i;

    len = (int) strlen (pi->program);
    if (len > longest_py_program)
       longest_py_program = len;

    snprintf (version, sizeof(version), "(%d.%d.%d)", pi->ver_major, pi->ver_minor, pi->ver_micro);
    len = (int) strlen (version);
    if (len > longest_py_version)
       longest_py_version = len;

    pi->modules = smartlist_new();

    if (get_modules_from_cache(pi) > 0)
    {
      len = smartlist_len (pi->modules);
      num_mod = _itoa (len, mod_buf, 10);
    }
    else if (opt.use_cache || pi->is_default || py_which == pi->variant || py_which == ALL_PYTHONS)
    {
      py_get_module_info (pi);
      len = smartlist_len (pi->modules);
      num_mod = _itoa (len, mod_buf, 10);
    }
    else
      num_mod = "<N/A>";

    TRACE (1, "%u: %-*s -> \"%s\".  ver: %s\n"
              "%*sDLL:         -> \"%s\"\n"
              "%*suser_site:   -> %s\n"
              "%*sVariant:     -> %s%s\n"
              "%*snum_mod:     -> %s\n",
           i, 2+longest_py_program, pi->program, pi->exe_name, version,
           indent+longest_py_program, "", pi->dll_name,
           indent+longest_py_program, "", pi->user_site_path ? pi->user_site_path : "<None>",
           indent+longest_py_program, "", py_variant_name(pi->variant), pi->is_default ? " (Default)" : "",
           indent+longest_py_program, "", num_mod);

    write_to_cache (pi);
  }

  TRACE (1, "py_init() finished\n"
         "------------------------------------------------------------------\n");
  global_indent = longest_py_version + longest_py_program;
}

/**
 * Make an `arg_vector` suitable for `PySys_SetArgvEx()`.
 *
 * \param[in] av   the `arg_vector` to initialise.
 * \param[in] argv the C-arguments to fill into `av`.
 * \param[in] wide TRUE if the `av->wide[]` array should be created.
 *                 FALSE if the `av->ascii[]` array should be created.
 */
static int make_arg_vector (arg_vector *av, const char **argv, BOOL wide)
{
  int i, num;

  if (wide && !Py_DecodeLocale)
     return (0);

  av->ascii = NULL;
  av->wide  = NULL;
  av->argc  = 0;

  for (i = 0; argv[i]; i++)
      av->argc++;
  num = av->argc + 1;

  if (wide)
       av->wide  = CALLOC (num, sizeof(wchar_t*));
  else av->ascii = CALLOC (num, sizeof(char*));

  for (i = 0; i < av->argc; i++)
  {
    if (wide)
         av->wide[i]  = (*Py_DecodeLocale) (argv[i], NULL);
    else av->ascii[i] = STRDUP (argv[i]);
  }

  TRACE (1, "av->argc: %d\n", av->argc);
  for (i = 0; i <= av->argc; i++)
  {
    if (wide)
         TRACE (1, "av->wide[%d]: %" WIDESTR_FMT "\n", i, av->wide[i]);
    else TRACE (1, "av->ascii[%d]: %s\n", i, av->ascii[i]);
  }
  return (i);
}

/**
 * Free the memory allocated for the `av` created by `make_arg_vector()`.
 *
 * \param[in] av  the `arg_vector` to free.
 */
static void free_arg_vector (arg_vector *av)
{
  int i;

  if (av->wide)
     ASSERT (PyMem_RawFree);

  for (i = 0; i < av->argc; i++)
  {
    if (av->wide)
         (*PyMem_RawFree) (av->wide[i]);
    else FREE (av->ascii[i]);
  }
  FREE (av->ascii);
  FREE (av->wide);
}

/**
 * \def PY_EXEC2_FMT
 *   The Python 2.x program to execute for a file.
 */
#define PY_EXEC2_FMT() "__file__=r'%s'; execfile(r'%s')"

/**
 * \def PY_EXEC3_FMT
 *   The Python 3.x program to execute for a file.
 */
#define PY_EXEC3_FMT() "__file__=r'%s'; f=open(r'%s','rt'); exec(f.read())"

/**
 * Executes a Python script for a single Python.
 *
 * \param[in] pi       The currently used Python-info variable.
 * \param[in] py_argv  The argument vector for the Python-script to run.
 * \param[in] capture  If `TRUE`, `sys.stdout` in the Python-script is
 *                     captured and stored in the return value `str`.
 *
 * \retval If `capture == TRUE`, an allocated string of the Python-script output. <br>
 *         If `capture == FALSE`, this function returns `NULL`.
 *
 * \note No expansion is done on `py_argv[0]`. The caller should expand it to a
 *       Fuly Qualified Pathname before use. Call `py_argv[0] = _fix_path (py_argv[0], buf)` first.
 */
static char *py_exec_internal (struct python_info *pi, const char **py_argv, BOOL capture)
{
  const char *prog0, *fmt;
  char       *str, *prog;
  size_t      size;
  arg_vector  av = { 0, NULL, NULL };   /* Fill in to shutup warnings from gcc */

  C_printf ("Executing ~6%s~0 using ~6%s~0: ", py_argv[0], pi->dll_name);

  if (!pi->is_embeddable)
  {
    WARN ("Failed; not embeddable.\n");
    return (NULL);
  }
  C_putc ('\n');

  /* Call `py_exit_embedding()` in case the previous Python used was a different variant.
   * Calling again `py_init_embedding()` will set the `PySys_SetArgvEx()` function pointer
   * and also call `setup_stdout_catcher()`.
   */
  py_exit_embedding (pi);

  if (!py_init_embedding(pi))
  {
    TRACE (1, "py_init_embedding() failed.\n");
    return (NULL);
  }

  if (pi->ver_major >= 3)
  {
    make_arg_vector (&av, py_argv, TRUE);
    (*PySys_SetArgvEx) (av.argc, av.wide, 1);
    fmt = PY_EXEC3_FMT();
  }
  else
  {
    make_arg_vector (&av, py_argv, FALSE);
    (*PySys_SetArgvEx) (av.argc, (wchar_t**)av.ascii, 1);
    fmt = PY_EXEC2_FMT();
  }

  if (capture)
       prog0 = "";
  else prog0 = "sys.stdout = old_stdout\n";   /* Undo what `setup_stdout_catcher()` did */

  size = strlen(prog0) + strlen(fmt) + 2 * strlen(py_argv[0]);
  prog = alloca (size);

  strcpy (prog, prog0);
  snprintf (prog+strlen(prog0), size, fmt, py_argv[0], py_argv[0]);

  str = call_python_func (pi, prog, __LINE__);

  /* If `capture == TRUE`, the 'str' should now contain the Python output of one of the above
   * 'exec()' or 'execfile()' calls.
   * Caller must call 'FREE(str)' on this value when done.
   *
   * Otherwise, if `capture == FALSE`, the `str` should now be NULL.
   */
  TRACE (2, "capture: %d, str:\n'%s'\n", capture, str ? str : "<none>");

  free_arg_vector (&av);
  return (str);
}

/**
 * Executes a Python script, optionally with arguments.
 *
 * If `py_wich == ALL_PYTHONS`, do it for all supported and found Pythons.
 *
 * \param[in] py_argv  The Python-script to run must be in `py_argv[0]`.<br>
 *                     The remaining command-line is in `py_argv[1..]`.
 *
 * \param[in] capture  If `TRUE`, `sys.stdout` in the Python-script is
 *                     captured and stored in the return value `str`.
 *
 * \retval str The captured string or `NULL`. If several `py_exec_internal()` are
 *             called, this string is from all calls merged together.
 *             Caller must call 'FREE(str)' on this value when done with it.
 */
char *py_execfile (const char **py_argv, BOOL capture)
{
  struct python_info *pi;
  char  *array [DIM(all_py_programs)+1];
  char  *ret, *str;
  int   i = 0;
  int   j = 0;

  memset (&array, '\0', sizeof(array));

  for (pi = py_select_first(py_which);   /* if the 1st was okay, try the next too */
       pi;
       pi = py_select_next(py_which), i++)
  {
    str = py_exec_internal (pi, py_argv, capture);
    if (str)
       array [j++] = str;
  }

  if (i == 0)
  {
    if (py_which == ALL_PYTHONS)
         WARN ("No Pythons were found on PATH.\n");
    else WARN ("%s was not found on PATH.\n", py_variant_name(py_which));
    return (NULL);
  }

  ret = str_join (array, "");
  TRACE (2, "str_join(): '%s'.\n", ret);

  for (i = 0; i < j; i++)
      FREE (array[i]);
  return (ret);
}

