/** \file    envtool_py.c
 *  \ingroup Envtool_PY
 *  \brief The Python functions for the envtool program.
 *
 * All functions require that `py_init()` was called first.
 * The output from `call_python_func()` is captured and can be parsed.
 * Look at `py_print_modinfo()` for an example.
 *
 * By Gisle Vanem <gvanem@yahoo.no>
 */

#include <stddef.h>
#include <fcntl.h>
#include <errno.h>

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
typedef intptr_t Py_ssize_t;

/** \typedef python_path
 *  The layout of each `sys.path[]` entry.
 */
typedef struct python_path {
        char dir [_MAX_PATH];  /**< Fully qualified directory of this entry. */
        bool exist;            /**< does it exist? */
        bool is_dir;           /**< and is it a dir; `_S_ISDIR()` */
        bool is_zip;           /**< or is it a zip; an .egg, .zip or a .whl file. */
      } python_path;

/**
 * \typedef python_module
 * The layout of each `pi->modules[]` entry.
 *
 * This information is obtained from parsing `PKG-INFO` or `METADATA` files.
 */
typedef struct python_module {
        char  name [30];               /**< the name of the module */
        char  version [40];            /**< this length should match the `p.version` in `PY_LIST_MODULES()` */
        char  location [_MAX_PATH];    /**< it's install location */
        char  dist_info [_MAX_PATH];   /**< filename for `METADATA` or `PKG-INFO` file. Equals "-" if none */
        char *summary;                 /**< A one line summary of it's purpose */
        char *homepage;                /**< the web-address of the project  */
        char *author;                  /**< the name of the author */
        char *author_email;            /**< the email-address of the author */
        char *installer;               /**< the installer used for this package (pip, conda) */
        char *requires;                /**< the package this module depends on. Currently only handles one */
        char *requires_py;             /**< the required Python versions. */
        bool  is_zip;                  /**< is it installed as a .egg/.whl? */
        bool  is_actual;               /**< Already called `get_actual_filename (&m->location)` */

       /**
        * \todo
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
  bool is_embeddable;

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
   * A smartlist of `python_path`.
   */
  smartlist_t *sys_path;

  /** The Python modules and packages array.
   *  A smartlist of `python_module`.
   */
  smartlist_t *modules;

  /** The user's `site-packages` path (if any).
   */
  char *user_site_path;

  /** Warn once if the above is not set.
   */
  bool do_warn_user_site;

  /** The Python version information.
   */
  int ver_major, ver_minor, ver_micro;

  /** Bitness of `dll_name`. `enum Bitness::bit_32` or `enum Bitness::bit_64`.
   * \anchor bitness
   */
  enum Bitness bitness;

  /** Embedding requires that the bitness of this CPython is the same as this calling program.
   */
  bool bitness_ok;

  /** This is the only default; i.e. the first Python `program` on `%PATH`.
   */
  bool is_default;

  /** Is this a CygWin Python?
   */
  bool is_cygwin;

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
  bool     do_warn_home;

  /** Holds a reference to the `sys.stdout` catcher object.
   *  Only set if `is_embeddable == true`:
   */
  PyObject *catcher;

  /** Holds the handle from `LoadLibrary()`.
   *  Only set if `is_embeddable == true`:
   */
  HANDLE dll_hnd;

  /** The index of this Python in the `py_programs` smartlist.
   *  I.e. in the cache-file "python_in_pathX = ...", `py_index == X`.
   */
  int py_index;

} python_info;

/**
 * List of all Pythons we care about. Ignore the console-less `pythonw.exe` programs.
 */
static python_info all_py_programs[] = {

    /* PyPy */
    { "pypy.exe",   PYPY_PYTHON, false, { "%s\\libpypy-c.dll", NULL }, },

    /* CPython */
    { "python.exe", PY3_PYTHON,  true,  { "~\\libpython%d.%d.dll", "%s\\python%d%d.dll", NULL }, },
    { "python.exe", PY2_PYTHON,  true,  { "~\\libpython%d.%d.dll", "%s\\python%d%d.dll", NULL }, },
  };

/**
 * The global Python instance pointer.
 * We need this mostly for `build_sys_path()` to work.
 */
static python_info *g_pi;

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
static int warn_on_py_fail = 1;

/**
 * Variables used in the generic callback helper function popen_append_out().
 */
static char  *popen_tmp = NULL;
static char  *popen_out = NULL;
static size_t popen_out_sz = 0;
static bool   popen_py_crash = false;

static int  get_python_version (const char *exe_name);
static bool py_add_module (python_info *pi, const python_module *m);

/**
 * The list of Pythons from the `"%PATH"` and from the
 * `"HKLM\Software\Python\PythonCore\xx\InstallPath"` locations.
 * This is an array of `python_info`.
 */
static smartlist_t *py_programs;

/**
 * \def LOAD_FUNC(pi, is_opt, f)
 *   A `GetProcAddress()` helper.
 *   \param pi      the `python_info` to work on.
 *   \param is_opt  `== true` if the function is optional.
 *   \param f       the name of the function (without any `"`).
 */
#define LOAD_FUNC(pi, is_opt, f)  do {                                              \
                                    f = GETPROCADDRESS (func_##f, pi->dll_hnd, #f); \
                                    if (!f && !is_opt) {                            \
                                      WARN ("Failed to find \"%s()\" in %s.\n",     \
                                            #f, pi->dll_name);                      \
                                      goto failed;                                  \
                                    }                                               \
                                    TRACE (3, "Function %s(): %*s 0x%p.\n",         \
                                           #f, 23-(int)strlen(#f), "", f);          \
                                  } while (0)

/**
 * \def LOAD_INT_PTR(pi, ptr)
 *   A `GetProcAddress()` helper for setting an int-pointer.
 *   \param pi   the `python_info` to work on.
 *   \param ptr  the int-pointer (and name) to import.
 */
#define LOAD_INT_PTR(pi, ptr)  do {                                                        \
                                 ptr = GETPROCADDRESS (int*, pi->dll_hnd, #ptr);           \
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
 * But ideally, these function-pointers should be put in `python_info` to be able
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
#define DEF_FUNC(ret, f, args)  typedef ret (__cdecl *func_##f) args; \
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

static int *Py_InspectFlag  = NULL;
static int *Py_IsolatedFlag = NULL;
static int *Py_FrozenFlag   = NULL;

static const search_list short_names[] = {
           { ALL_PYTHONS, "all"  },
           { PY2_PYTHON,  "py2"  },
           { PY3_PYTHON,  "py3"  },
           { PYPY_PYTHON, "pypy" },
         };

static const search_list full_names[] = {
           { ALL_PYTHONS, "All"     },
           { PY2_PYTHON,  "Python2" },
           { PY3_PYTHON,  "Python3" },
           { PYPY_PYTHON, "PyPy"    },
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
  static char       *result [DIM(all_py_programs)+2];
  const python_info *pi;
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
      case PYPY_PYTHON:
           result [j++] = "pypy";
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
    if (i > 0 && result[i] && !strcmp(result[i], result[i-1]))
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
static python_info *py_select (enum python_variants which)
{
  python_info *pi;
  int          i, max = smartlist_len (py_programs);

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
 * \param[in] ver A pointer to a `struct ver_info` filled with version information.
 *
 * \retval 0  On some error; none of the in-params gets filled.
 *         1  If all went okay.
 */
bool py_get_info (char **exe, struct ver_info *ver)
{
  const python_info *pi;

  if (exe)
     *exe = NULL;

  if (py_which == ALL_PYTHONS)          /* Not possible here */
       pi = py_select (DEFAULT_PYTHON);
  else pi = py_select (py_which);

  if (!pi)
     return (0);

  if (exe)
     *exe = STRDUP (pi->exe_name);

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
static void fix_python_variant (python_info *pi, enum python_variants v)
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
static const char *dir_name (const char *dir, bool is_dir)
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
static void print_sys_path (const python_info *pi, int indent)
{
  int i, max = smartlist_len (pi->sys_path);

  for (i = 0; i < max; i++)
  {
    const python_path *pp  = smartlist_get (pi->sys_path, i);
    const char        *dir = dir_name (pp->dir, pp->is_dir);

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

static void print_home_path (const python_info *pi, int indent)
{
  C_printf ("%*s", indent+global_indent, "~7Home-path:~0");
  if (pi->home_a)
     C_printf (" %s\n", dir_name(pi->home_a, true));
  else if (pi->home_w)
     C_printf (" %" WIDESTR_FMT "\n", pi->home_w);
  else
     C_puts (" ~5<none>~0\n");
}

static void print_user_site_path (const python_info *pi, int indent)
{
  C_printf ("%*s", indent+global_indent, "~7User-site:~0");
  if (pi->user_site_path)
       C_printf (" %s %s~0\n", dir_name(pi->user_site_path, true),
                 is_directory(pi->user_site_path) ? "~2OK" : "~5Does not exist");
  else C_puts (" ~5<none>~0\n");
}

/**
 * Get the name of the calling program. This allocated ASCII string is used in
 * `(*Py_SetProgramName)()` and `(*Py_SetPythonHome)()`.
 */
static char *get_prog_name_ascii (const python_info *pi)
{
  char  prog [_MAX_PATH];
  char *p = NULL;

  if (GetModuleFileNameA(NULL, prog, DIM(prog)))
     p = prog;

  if (pi->is_cygwin)
     p = make_cyg_path (prog, prog);
  return (p ? STRDUP(p) : NULL);
}

static wchar_t *get_prog_name_wchar (const python_info *pi)
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
static void set_python_prog (python_info *pi)
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
static void set_python_home (python_info *pi)
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
 * Set a flag and return the previous value.
 * Return -1 if the pointer to the flag is NULL.
 */
static int py_flag_common (int **ptr, int value)
{
  int rc = -1;

  if (!*ptr)
     return (-1);

  rc = **ptr;
  **ptr = value;
  return (rc);
}

/**
 * Set the imported `Py_InspectFlag`:
 * "Enter interactive mode after executing the script or the command, ..."
 *
 * We do not want that.
 */
static int py_set_inspect_flag (int v)
{
  return py_flag_common (&Py_InspectFlag, v);
}

/**
 * Set the imported `Py_IsolatedFlag`:
 * Will run Python in isolated mode. In isolated mode sys.path contains neither the
 * script's directory nor the user's site-packages directory.
 */
static int py_set_isolated_flag (int v)
{
  return py_flag_common (&Py_IsolatedFlag, v);
}

/**
 * Set the imported `Py_FrozenFlag`:
 * Suppress error messages when calculating the module search path in `Py_GetPath()`.
 * Private flag used by `_freeze_importlib` and `frozenmain` programs.
 */
static int py_set_frozen_flag (int v)
{
  return py_flag_common (&Py_FrozenFlag, v);
}

/**
 * Free the Python DLL handle allocated by `py_init_embedding()`.
 */
static void py_exit_embedding (python_info *pi)
{
  if (!pi->dll_hnd)
     return;

  py_set_inspect_flag (0);
  py_set_isolated_flag (0);
  Py_InspectFlag = Py_IsolatedFlag = Py_FrozenFlag = NULL;

  if (Py_Finalize)
  {
    /* Check if "AppVerifier (Network tests)" is active in our process
     */
    if (GetModuleHandle("vfnet.dll"))
       TRACE (1, "'vfnet.dll' present; calling Py_Finalize() is not safe.\n");
    else
    {
      TRACE (4, "Calling Py_Finalize().\n");
      (*Py_Finalize)();
    }
  }

  if (!IsDebuggerPresent())
     CloseHandle (pi->dll_hnd);
  pi->dll_hnd = NULL;
}

/**
 * Some smartlist_wipe() helpers:
 *
 * Free a `sys.path[]` element for a single `python_info` program.
 */
static void free_sys_path (void *e)
{
  python_path *pp = (python_path*) e;

  FREE (pp);
}

/**
 * Free a `modules[]` element for a single `python_info` program.
 */
static void free_module (void *_m)
{
  python_module *m = (python_module*) _m;

  FREE (m->summary);
  FREE (m->homepage);
  FREE (m->author);
  FREE (m->author_email);
  FREE (m->installer);
  FREE (m->requires);
  FREE (m->requires_py);
  FREE (m);
}

/**
 * A smartlist_wipe() helper:
 *
 * Free one element in \ref py_programs.
 */
static void free_py_program (void *_pi)
{
  python_info *pi = (python_info*) _pi;

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
  static char code[] = "import sys\n"                                 \
                       "PY3 = (sys.version_info[0] >= 3)\n"           \
                       "Empty = ['', b''][PY3]\n"                     \
                       "\n"                                           \
                       "class catch_stdout:\n"                        \
                       "  def __init__ (self):\n"                     \
                       "    self.value = Empty\n"                     \
                       "    self.exit_code = 0\n"                     \
                       "  def write (self, txt):\n"                   \
                       "    if PY3:\n"                                \
                       "      self.value += bytes (txt, \"UTF-8\")\n" \
                       "    else:\n"                                  \
                       "      self.value += txt\n"                    \
                       "  def reset (self):\n"                        \
                       "    self.value = Empty\n"                     \
                       "  def flush (self):\n"                        \
                       "    self.reset()\n"                           \
                       "  def exit (self, x):\n"                      \
                       "    self.exit_code = x\n"                     \
                       "\n"                                           \
                       "old_sys_exit = sys.exit\n"                    \
                       "old_stdout = sys.stdout\n"                    \
                       "sys.stdout = catcher = catch_stdout()\n"      \
                       "sys.exit   = catcher.exit\n";

  PyObject *mod = (*PyImport_AddModule) ("__main__");          /* create `__main__` module */
  int       rc  = (*PyRun_SimpleString) (code);                /* invoke code to redirect */
  PyObject *obj = (*PyObject_GetAttrString) (mod, "catcher");  /* get a reference to `catcher` created above */

  TRACE (5, "code: '%s'\n", code);
  TRACE (4, "mod: 0x%p, rc: %d, obj: 0x%p\n", mod, rc, obj);
  return (obj);
}

/**
 * Do NOT call this unless `py->is_embeddable == true`.
 */
static bool py_init_embedding (python_info *pi)
{
  char *exe = pi->exe_name;
  char *dll = pi->dll_name;

  if (!dll)
  {
    WARN ("Failed to find Python DLL for %s.\n", exe);
    return (false);
  }

  pi->dll_hnd = LoadLibrary (dll);
  if (!pi->dll_hnd)
  {
    WARN ("Failed to load %s; %s\n", dll, win_strerror(GetLastError()));
    pi->is_embeddable = false;  /* Do not do this again */
    return (false);
  }

  TRACE (2, "Full DLL name: \"%s\". Handle: 0x%p\n", pi->dll_name, pi->dll_hnd);

  LOAD_FUNC (pi, false, Py_InitializeEx);
  LOAD_FUNC (pi, false, Py_IsInitialized);
  LOAD_FUNC (pi, false, Py_Finalize);
  LOAD_FUNC (pi, false, PySys_SetArgvEx);
  LOAD_FUNC (pi, false, Py_FatalError);
  LOAD_FUNC (pi, false, Py_SetProgramName);
  LOAD_FUNC (pi, false, Py_SetPythonHome);
  LOAD_FUNC (pi, false, PyRun_SimpleString);
  LOAD_FUNC (pi, false, PyObject_GetAttrString);
  LOAD_FUNC (pi, false, PyImport_AddModule);
  LOAD_FUNC (pi, false, PyObject_CallMethod);
  LOAD_FUNC (pi, false, PyObject_Free);
  LOAD_FUNC (pi, false, Py_DecRef);
  LOAD_FUNC (pi, false, PyErr_Occurred);
  LOAD_FUNC (pi, false, PyErr_PrintEx);
  LOAD_FUNC (pi, false, PyErr_Clear);
  LOAD_FUNC (pi, true, PyString_AsString);      /* CPython 2.x */
  LOAD_FUNC (pi, true, PyBytes_AsString);       /* CPython 3.x */
  LOAD_FUNC (pi, true, PyString_Size);          /* CPython 2.x */
  LOAD_FUNC (pi, true, PyBytes_Size);           /* CPython 3.x */
  LOAD_FUNC (pi, true, Py_DecodeLocale);        /* CPython 3.x */
  LOAD_FUNC (pi, true, PyMem_RawFree);          /* CPython 3.x */
  LOAD_FUNC (pi, true, initposix);              /* In Cygwin's libpython2.x.dll */
  LOAD_FUNC (pi, true, PyInit_posix);           /* In Cygwin's libpython3.x.dll */
  LOAD_FUNC (pi, true, Anaconda_GetVersion);    /* In Anaconda's python3x.dll. */

  LOAD_INT_PTR (pi, Py_IsolatedFlag);
  LOAD_INT_PTR (pi, Py_FrozenFlag);
  LOAD_INT_PTR (pi, Py_InspectFlag);

  if (!Py_InspectFlag)
     WARN ("Failed to find `Py_InspectFlag' in \"%s\"\n", pi->exe_name);

  if (initposix || PyInit_posix)
     pi->is_cygwin = true;

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

//py_set_isolated_flag (1);

  (*Py_InitializeEx) (0);

  pi->catcher = setup_stdout_catcher();
  if (pi->catcher)
     return (true);   /* Success! */

  /* Fail, fall through */

failed:
  py_exit_embedding (pi);
  return (false);
}

/**
 * Dump a `py_prog` to `stdout` or `stderr` in case of errors.
 * Add leading line numbers while printing the `py_prog`.
 *
 * \param[in] out      the `FILE*` to print to; either `stdout` or `stderr`.
 * \param[in] py_prog  the Python program to dump.
 * \param[in] where    the line-number where the `py_prog` was executed.
 */
static void py_prog_dump (FILE *out, const char *py_prog, unsigned where)
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
static char *call_python_func (python_info *pi, const char *prog, unsigned line)
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
  TRACE (2, "rc: %d, err: 0x%p\n", rc, err);

  if (rc < 0)
  {
    (*PyErr_Clear)();
    if (warn_on_py_fail)
    {
      WARN ("Failed script (%s): ", pi->exe_name);
      py_prog_dump (stderr, prog, line);
    }
    return (NULL);
  }

  /* Get a reference to `catcher.value` where the catched `sys.stdout` string is stored.
   */
  obj = (*PyObject_GetAttrString) (pi->catcher, "value");

  TRACE (4, "rc: %d, obj: 0x%p\n", rc, obj);

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
  TRACE (3, "PyString_Size(): %ld, output:\n%s\n", (long)size, str);

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
           (long)dos_size, (long)(size+lines), lines, okay ? "OK" : "discrepancy in dos_size");
  }
  return (str);
}

/**
 * As above, but with the Python code given as a NULL-terminated array.
 * Each element should NOT have newlines. These are done here.
 */
static char *call_python_array (python_info *pi, const char **array, unsigned line)
{
  char       *p, *prog;
  const char *a;
  size_t      i = 0, size = 1;

  for (a = array[0]; a; a = array[++i])
  {
    size += strlen (a) + 1;
    ASSERT (strchr(a, '\n') == NULL);
  }

  prog = p = alloca (size);
  for (a = array[0]; a; a = array[++i])
  {
    strcpy (p, a);
    p += strlen (a);
    *p++ = '\n';
  }
  return call_python_func (pi, prog, line);
}

/**
 * Return a `file` with correct slashes and filename casing.
 */
static const char *py_filename (const char *file)
{
  static char buf [_MAX_PATH];
  char *p = _strlcpy (buf, file, sizeof(buf));

  if (get_actual_filename(&p, false))
  {
    _strlcpy (buf, p, sizeof(buf));
    FREE (p);
  }
  return (buf);
}

static char *fmem_search (const char *f_mem, size_t f_size, const char *search)
{
  size_t len, f_max = min (5000, f_size);
  char  *p, *ret = NULL;

  if (!search)
     ret = str_ndup (f_mem, f_max);
  else
  {
    ret = str_nstr (f_mem, search, f_max);
    len = strlen (search);
    if (ret && (ret + len < f_mem + f_max))
       ret = STRDUP (ret + len);
  }

  TRACE (1, "search: '%-12.12s', f_mem: 0x%p, f_max: %4zu, ret: 0x%p.\n", search ? search : "NULL", f_mem, f_max, ret);

  if (ret)
  {
    p = strchr (ret, '\r');
    if (p)
       *p = '\0';
    p = strchr (ret, '\n');
    if (p)
       *p = '\0';
  }
  return (ret);
}

static char *fcheck_open_mem (python_module *m, const char *file, size_t *f_size)
{
  if (file)
  {
    char  fqfn [_MAX_PATH];
    char *pkg_dir = dirname (m->dist_info);

    snprintf (fqfn, sizeof(fqfn), "%s%c%s", pkg_dir, DIR_SEP, file);
    FREE (pkg_dir);
    file = strdupa (fqfn);
  }
  else
    file = m->dist_info;

  *f_size = 0;
  if (FILE_EXISTS(file))
     return fopen_mem (file, f_size);
  return (NULL);
}

static void py_get_meta_details (python_module *m)
{
  if (!m->is_zip)
  {
    char  *f_mem;
    size_t f_size;

    f_mem = fcheck_open_mem (m, NULL, &f_size);
    if (f_mem)
    {
      m->summary      = fmem_search (f_mem, f_size, "Summary: ");
      m->homepage     = fmem_search (f_mem, f_size, "Home-page: ");
      m->author       = fmem_search (f_mem, f_size, "Author: ");
      m->author_email = fmem_search (f_mem, f_size, "Author-email: ");
      m->requires_py  = fmem_search (f_mem, f_size, "Requires-Python: ");

      if (!m->homepage)
         m->homepage = fmem_search (f_mem, f_size, "Project-URL: Source code, ");

      if (!m->homepage)
         m->homepage = fmem_search (f_mem, f_size, "Project-URL: homepage, ");

      /* Fix lines such as:
       *   Author-email: Hynek Schlawack <hs@ox.cx>
       *
       * Split into 'm->author' and 'm->author_email' fields.
       */
      if (m->author_email)
      {
        char *p = strchr (m->author_email, '<');
        char *q = strchr (m->author_email, '>');
        char *email, *author;

        if (p && q > p)
        {
          email = m->author_email;
          m->author_email = str_ndup (p+1, q - p);
          FREE (email);
        }
        if (m->author && p)
        {
          author = m->author;
          m->author = str_ndup (m->author, p - m->author);
          FREE (author);
        }
      }
      if (m->author)
      {
        char *p = strchr (m->author, '<');

        if (p && p[-1] == ' ')
           p[-1] = '\0';
      }
      FREE (f_mem);
    }

    f_mem = fcheck_open_mem (m, "INSTALLER", &f_size);
    if (f_mem)
    {
      m->installer = fmem_search (f_mem, f_size, NULL);
      FREE (f_mem);
    }
    f_mem = fcheck_open_mem (m, "requires.txt", &f_size);
    if (f_mem)
    {
      m->requires = fmem_search (f_mem, f_size, NULL);
      FREE (f_mem);
    }
  }

  if (!m->summary)
     m->summary = STRDUP ("<unknown>");

  if (!m->homepage)
     m->homepage = STRDUP ("<unknown>");

  if (!m->author)
     m->author = STRDUP ("<unknown>");

  if (!m->author_email)
     m->author_email = STRDUP ("?");

  if (!m->installer)
     m->installer = STRDUP ("<unknown>");

  if (!m->requires)
     m->requires = STRDUP ("<none>");

  if (!m->requires_py)
     m->requires_py = STRDUP ("<unknown>");
}

/**
 * A simple embedded test; capture the output from Python's
 * `print()` function.
 */
static bool test_python_funcs (python_info *pi, bool reinit)
{
  static const char *prog[] = { "import sys",
                                "print (sys.version_info)",
                                "for i in range(3):",
                                "  print (\"  Hello world\")",
                                NULL
                              };
  const char *name = py_variant_name (pi->variant);
  char *str;

  if (reinit && (!pi->bitness_ok || !py_init_embedding(pi)))
     return (false);

  /* The `str` should now contain the Python output of above `prog`.
   */
  str = call_python_array (pi, prog, __LINE__);
  C_printf ("~3Captured output of %s:~0\n** %s **\n", name, str);
  FREE (str);

  /* Restore `sys.stdout` to it's old value. Thus this should return no output.
   */
  C_printf ("~3The rest should not be captured:~0\n");
  str = call_python_func (pi, "sys.stdout = old_stdout\n", __LINE__);
  FREE (str);

  str = call_python_array (pi, prog, __LINE__);
  C_printf ("~3Captured output of %s now:~0\n** %s **\n", name, str);
  FREE (str);
  return (true);
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
static char *tmp_fputs (const python_info *pi, const char *buf)
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
  fprintf (fil,
           "#\n"
           "# Temp-file %s for command \"%s %s\".\n"
           "# Created %s.\n"
           "#\n%s\n",
           tmp, pi->exe_name, tmp, get_time_str(time(NULL)), buf);
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
  if (!strncmp(str, "Traceback (", 11) || !strncmp(str, "ImportError:", 13))
     popen_py_crash = true;

  if (!popen_out || popen_out_sz - strlen(popen_out) < 4*strlen(str))
  {
    popen_out_sz += 4 * strlen (str);
    popen_out = REALLOC (popen_out, popen_out_sz);
  }
  if (index == 0)
     popen_out[0] = '\0';

  TRACE (2, "index: %d, strlen(popen_out): %d, popen_out_sz: %d\n",
         index, (int)strlen(popen_out), (int)popen_out_sz);

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
  popen_py_crash = false;
  FREE (popen_out);
  FREE (popen_tmp);
}

/**
 * The `popen_run()` function for running a non-embedded Python program.
 */
static char *popen_run_py (const python_info *pi, const char *prog)
{
  char        report [1000];
  const char *py_exe = pi->exe_name;
  int         rc;

  popen_append_clear();
  popen_tmp = tmp_fputs (pi, prog);
  if (!popen_tmp)
  {
    WARN ("\"%s\": Failed to create a temp-file; errno: %d.\n", pi->exe_name, errno);
    popen_append_clear();
    return (NULL);
  }

  /** \todo: need to check 'pi->is_cygwin' first
   */
  rc = popen_run (popen_append_out, py_exe, "%s 2>&1", popen_tmp);
  if (rc < 0)
  {
    if (warn_on_py_fail)
    {
      snprintf (report, sizeof(report), "\"%s %s\"; errno: %d\n", py_exe, popen_tmp, errno);
      Beep (1000, 30);
      WARN ("Failed script (%s): ", py_exe);
      C_puts (report);
    }
    popen_append_clear();
    return (NULL);
  }

  if (popen_py_crash)
  {
    if (warn_on_py_fail)
    {
      snprintf (report, sizeof(report), "\"%s %s\":\n%s\n", py_exe, popen_tmp, popen_out);
      Beep (1000, 30);
      WARN ("Failed script (%s): ", py_exe);
      C_puts (report);
    }
    popen_append_clear();
    return (NULL);
  }
  return (popen_out);
}

const char *get_date_str (const python_module *m)
{
  struct stat st;
  int    rc;

  if (m->dist_info[0] == '-')
     return ("?");

  if (m->is_zip)
       rc = safe_stat (m->location, &st, NULL);
  else rc = safe_stat (m->dist_info, &st, NULL);

  if (rc != 0)
     return ("?");
  return get_time_str (st.st_mtime);
}

/**
 * Print the list of installed Python modules with
 * their location and version.
 *
 * \param[in] spec         Print only those module-names `m->name` that matches `spec`.
 * \param[in] get_details  Print more details, like dist-info information.
 *
 * \retval The number of modules printed.
 */
static int py_print_modinfo (const char *spec, bool get_details)
{
  bool match_all = !strcmp (spec, "*");
  int  found = 0, zips_found = 0;
  int  i, max = smartlist_len (g_pi->modules);

  for (i = 0; i < max; i++)
  {
    python_module *m = smartlist_get (g_pi->modules, i);

    if (fnmatch(spec, m->name, fnmatch_case(0)) != FNM_MATCH)
       continue;

#if 0
    if (opt.use_cache && m->dist_info[0] && !FILE_EXISTS(m->dist_info))
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

    C_printf ("~6%3d:~0 ", found);

    if (get_details)
    {
      const char *loc = py_filename (m->location);

      _strlcpy (m->location, loc, sizeof(m->location));
      m->is_actual = true;

      C_printf (" name:      %s\n", m->name);
      C_printf ("      version:   %s\n", m->version);
      C_printf ("      date:      %s\n", get_date_str(m));
      C_printf ("      location:  %s%s\n", m->location, m->is_zip ? " (ZIP)" : "");

      if (m->dist_info[0] != '-')
      {
        int raw = C_setraw (1);

        py_get_meta_details (m);
        C_printf ("      dist-info: %s\n", py_filename(m->dist_info));
        C_printf ("      summary:   %s\n", m->summary);
        C_printf ("      home-page: %s\n", m->homepage);
        C_printf ("      author:    %s <%s>\n", m->author, m->author_email);
        C_printf ("      installer: %s\n", m->installer);
        C_printf ("      requires:  %s\n", m->requires);
        C_printf ("      py-req:    %s\n", m->requires_py);
        C_setraw (raw);
      }
      C_putc ('\n');
    }
    else
    {
      C_printf ("%-30s %-10s%s\n", m->name, m->version, m->is_zip ? " (ZIP)" : "");
    }
  }

  if (match_all && !get_details)
     C_printf ("~6Found %d modules~0 (%d are ZIP/EGG files).\n", found, zips_found);
  return (found);
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
        "import os, sys, warnings, zipfile\n"                                          \
        "\n"                                                                           \
        "warnings.filterwarnings (\"ignore\", category=DeprecationWarning)\n"          \
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
        "    package_list.append ('%s;%s;%s;%s' % (p.key, ver, loc, meta))\n"          \
        "\n"                                                                           \
        "  for p in sorted (package_list):\n"                                          \
        "    print (p)\n"                                                              \
        "\n"                                                                           \
        "list_modules (\"*\")\n"

/**
 * A more detailed version of `PY_LIST_MODULES()` returning
 * modules and packages at runtime.
 */
static const char *py_list_modules2[] = {
      "from __future__ import print_function",
      "import os, sys, pip, imp",
      "",
      "types = { imp.PY_SOURCE:     'source file',      # = 1",
      "          imp.PY_COMPILED:   'object file',",
      "          imp.C_EXTENSION:   'shared library',   # = 3",
      "          imp.PKG_DIRECTORY: 'package directory',",
      "          imp.C_BUILTIN:     'built-in module',  # = 6",
      "          imp.PY_FROZEN:     'frozen module'",
      "        }",
      "mod_paths    = {}",
      "mod_builtins = {}",
      "prev_pathname = ''",
      "prev_modtype  = None",
      "",
      "def get_module_path (mod, mod_type, mod_path):",
      "  next_scope = mod.index ('.') + 1",
      "  last_scope = mod.rindex ('.') + 1",
      "",
      "  fname = mod_path + '\\\\' + mod[next_scope:].replace('.','\\\\\') + '.py'",
      "  if os.path.exists(fname):",
      "     return fname",
      "",
      "  init = mod_path + '\\\\' + mod[last_scope:] + '\\\\__init__.py'",
      "  if os.path.exists(init):",
      "     return init",
      "",
      "  try:",
      "    if mod_builtins[mod[last_scope:]] == 1:",
      "      return '__builtin__ ' + mod[last_scope:]",
      "  except KeyError:",
      "    pass",
      "",
      "  try:",
      "    return mod_paths [mod[last_scope:]] + ' !'",
      "  except KeyError:",
      "    pass",
      "",
      "  try:",
      "    _x, pathname, _y = imp.find_module (mod, mod_path)",
      "    return pathname",
      "  except ImportError:",
      "    return '<unknown>'",
      "  except RuntimeError:",
      "    mod_builtins [mod] = 1",
      "    return '__builtin__ ' + mod[last_scope:]",
      "",
      "for s in sorted(sys.modules):",
      "  print ('%s' % s, end='')",
      "  if '.' in s:",
      "    print (',%s' % get_module_path(s,prev_modtype,prev_pathname))",
      "    continue",
      "",
      "  try:",
      "    _, pathname, descr = imp.find_module (s)",
      "    t = types [descr[2]]",
      "    prev_modtype = t",
      "    print (',%s,' % t, end='')",
      "    if pathname and '\\\\' in pathname:",
      "      print ('%s' % pathname)",
      "      prev_pathname = pathname",
      "      prev_modtype  = t",
      "      mod_paths [s] = pathname",
      "    else:",
      "      mod_builtins [s] = 1",
      "  except ImportError:",
      "    print (',<unknown>')",
      NULL
    };

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
static void py_get_module_info (python_info *pi)
{
  char *line, *str = NULL;
  char *str2 = NULL;
  char *tok_end;
  int   save = warn_on_py_fail;

  if (opt.use_cache && smartlist_len(pi->modules) > 0)
     TRACE (1, "Calling %s() with a cache should not be neccesary.\n", __FUNCTION__);

  if (pi->is_embeddable && pi->bitness_ok)
  {
    if (!py_init_embedding(pi))
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
    warn_on_py_fail = 0;
    str = call_python_func (pi, PY_LIST_MODULES(), __LINE__);
//  str2 = call_python_array (pi, py_list_modules2, __LINE__);
    ARGSUSED (str2);
    set_error_mode (1);
  }
  else
  {
    warn_on_py_fail = 0;
    str = popen_run_py (pi, PY_LIST_MODULES());
  }

  warn_on_py_fail = save;

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
      char version [40+1]  = { "?" };
      char fname   [256+1] = { "?" };
      char meta    [256+1] = { "-" };
      int  num = sscanf (line, "%40[^;];%40[^;];%256[^;];%256s", module, version, fname, meta);

      if (num >= 3)
      {
        python_module m;

        memset (&m, '\0', sizeof(m));
        _strlcpy (m.name, module, sizeof(m.name));
        _strlcpy (m.version, version, sizeof(m.version));
        _strlcpy (m.location, fname, sizeof(m.location));
        _strlcpy (m.dist_info, "-", sizeof(m.dist_info));
        if (str_endswith(fname, " (ZIP)"))
        {
          m.location [strlen(m.location) - sizeof(" (ZIP)") + 1] = '\0';
          m.is_zip = true;
        }
        if (meta[0] != '-')
           _strlcpy (m.dist_info, meta, sizeof(m.dist_info));

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
static bool check_bitness (python_info *pi, char **needed_bits)
{
  our_bitness = (sizeof(void*) == 4) ? bit_32 : bit_64;

  if (pi->bitness == bit_unknown)
     pi->bitness_ok = (pi->dll_name && check_if_PE(pi->dll_name, &pi->bitness));

  if (pi->bitness_ok && pi->bitness != our_bitness)
     pi->bitness_ok = false;

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
  r.is_dir      = false;
  r.is_junction = false;
  r.is_cwd      = false;
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
#define PY_ZIP_LIST()                                                                \
        "import os, sys, fnmatch, zipfile\n"                                         \
        "class opt():\n"                                                             \
        "  debug          = %d\n"                                                    \
        "  case_sensitive = %d\n"                                                    \
        "  file_spec      = '%s'\n"                                                  \
        "  zip_file       = r'%s'\n"                                                 \
        "\n"                                                                         \
        "def trace (s):\n"                                                           \
        "  if opt.debug >= 3:\n"                                                     \
        "     sys.stderr.write (s)\n"                                                \
        "\n"                                                                         \
        "def print_zip (f):\n"                                                       \
        "  base = os.path.basename (f.filename)\n"                                   \
        "  trace ('egg-file: %%s, base: %%s\\n' %% (f.filename, base))\n"            \
        "  if opt.case_sensitive:\n"                                                 \
        "     match = fnmatch.fnmatchcase\n"                                         \
        "  else:\n"                                                                  \
        "     match = fnmatch.fnmatch\n"                                             \
        "  if match(f.filename, opt.file_spec) or match(base, opt.file_spec):\n"     \
        "     date = \"%%4d%%02d%%02d\"  %% (f.date_time[0:3])\n"                    \
        "     time = \"%%02d%%02d%%02d\" %% (f.date_time[3:6])\n"                    \
        "     str = \"%%d %%s.%%s %%s\"  %% (f.file_size, date, time, f.filename)\n" \
        "     trace ('str: \"%%s\"\\n' %% str)\n"                                    \
        "     print (str)\n"                                                         \
        "\n"                                                                         \
        "zf = zipfile.ZipFile (opt.zip_file, 'r')\n"                                 \
        "for f in zf.infolist():\n"                                                  \
        "    print_zip (f)\n"

static int process_zip (python_info *pi, const char *zip_file)
{
  char  prog [sizeof(PY_ZIP_LIST()) + _MAX_PATH + 100];
  char *line, *tok_end, *str = NULL;
  int   found = 0;
  int   len = snprintf (prog, sizeof(prog), PY_ZIP_LIST(),
                        opt.debug, opt.case_sensitive, opt.file_spec, zip_file);

  if (len < 0)
     FATAL ("`char prog[%zu]` buffer too small. Approx. %zu bytes needed.\n",
            sizeof(prog), sizeof(PY_ZIP_LIST()) + _MAX_PATH);

  if (pi->is_embeddable && pi->bitness_ok)
       str = call_python_func (pi, prog, __LINE__);
  else str = popen_run_py (pi, prog);

#if 0
  /**
   * \todo Add zip content to cache?
   */
  if (opt.use_cache)
  {
    snprintf (prog, sizeof(prog), PY_ZIP_LIST(), opt.debug, opt.case_sensitive, "*", zip_file);
    for (i,s) in enumerate(zip_content.split())
    {
      snprintf (format, sizeof(format), "python_zip%d_%d = %%s,%%s", py_index, i);
      cache_putf (SECTION_PYTHON, format, zip_file, s);
    }
  }
#endif

  if (str)
  {
    for (found = 0, line = _strtok_r(str, "\n", &tok_end);
         line;
         line = _strtok_r(NULL, "\n", &tok_end), found++)
    {
      TRACE (2, "line: \"%s\", found: %d\n", line, found);

      if (!strncmp(line, "str: ", 5))   /* if opt.debug >= 3; ignore these from stderr */
         continue;
      if (!report_zip_file(zip_file, line))
         break;
    }
  }

  if (str == popen_out)
       popen_append_clear();
  else FREE (str);

  if (found == 0)
     TRACE (1, "No matches in %s for %s.\n", zip_file, opt.file_spec);
  return (found);
}

/**
 * Check if the `module` is already in the `pi->modules` smartlist.
 */
static bool py_module_found (const python_info *pi, const char *module)
{
  int i, max = smartlist_len (pi->modules);

  for (i = 0; i < max; i++)
  {
    const python_module *m = smartlist_get (pi->modules, i);

    if (!stricmp(m->name, module))
       return (true);
  }
  return (false);
}

/**
 * If `m->name` is not already in the `pi->modules` smartlist, allocate and copy it.
 */
static bool py_add_module (python_info *pi, const python_module *m)
{
  if (!py_module_found(pi, m->name))
  {
    python_module *m2 = MALLOC (sizeof(*m2));

    memcpy (m2, m, sizeof(*m2));
    smartlist_add (pi->modules, m2);
    return (true);
  }
  TRACE (1, "module '%s' for '%s' already added!\n", m->name, pi->program);
  return (false);
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
static bool py_path_found (const python_info *pi, const char *dir)
{
  int i, max = smartlist_len (pi->sys_path);

  for (i = 0; i < max; i++)
  {
    const python_path *pp = smartlist_get (pi->sys_path, i);

    if (!stricmp(pp->dir, dir))
       return (true);
  }
  return (false);
}

/**
 * Allocate a `python_path` node and add it to `pi->sys_path[]` smartlist.
 * Not called if `opt.use_cache == true` and something found in file-cache.
 */
static void add_sys_path (const char *dir)
{
  if (!py_path_found(g_pi, dir))
  {
    python_path *pp = CALLOC (1, sizeof(*pp));
    struct stat  st;

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
 *   The code which is used if `py->is_embeddable == true`.
 *   Used in the parameter to `call_python_func()`.
 */
#define PY_PRINT_SYS_PATH_DIRECT()  "import sys\n" \
                                    "for (i,p) in enumerate(sys.path):\n" \
                                    "  print ('%s' % p)\n"

/**
 * \def PY_PRINT_SYS_PATH2_CMD
 *   Used in Python2 when `py->is_embeddable == false`.
 */
#define PY_PRINT_SYS_PATH2_CMD()  "import os, sys; " \
                                  "[os.write(1,'%s\\n' % p) for (i,p) in enumerate(sys.path)]"

/**
 * \def PY_PRINT_SYS_PATH3_CMD
 *   Used in Python3 when `py->is_embeddable == false`.
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
static void get_sys_path (const python_info *pi)
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
static int get_dll_name (python_info *pi, const char **libs)
{
  const char *lib_fmt;
  char        dll1 [_MAX_PATH] = { '\0' };
  char        dll2 [_MAX_PATH] = { '\0' };
  const char *use_this = NULL;
  const char *newest   = NULL;
  struct stat st1,  st2, *use_st = NULL;
  bool       _st1, _st2, equal;
  size_t      i, len, num = DIM (pi->libraries);

  _st1 = _st2 = false;

  memset (&st1, '\0', sizeof(st1));
  memset (&st2, '\0', sizeof(st2));

  for (i = 0, lib_fmt = libs[0]; lib_fmt && i < num; lib_fmt = libs[++i])
  {
    if (!strncmp(lib_fmt, "%s\\", 3))
    {
      snprintf (dll1, sizeof(dll1), lib_fmt, pi->exe_dir, pi->ver_major, pi->ver_minor);
      snprintf (dll2, sizeof(dll2), lib_fmt, sys_dir, pi->ver_major, pi->ver_minor);
    }
    else if (!strncmp(lib_fmt, "~\\", 2))
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
 * The above failed. Try to find the .DLL by running a
 * `print(sys.base_prefix)` command.
 */
static int get_dll_name_from_sys_prefix (python_info *pi)
{
  char *str;
  int   found = 0;
  int   save = warn_on_py_fail;

  warn_on_py_fail = 0;
  str = popen_run_py (pi, "import sys; print(sys.base_prefix, end=\"\")");
  warn_on_py_fail = save;

  if (str)
  {
    struct stat st;
    char        dll_spec [_MAX_PATH];

    snprintf (dll_spec, sizeof(dll_spec), "%s\\python3.dll", str);
    found = (stat(dll_spec, &st) == 0);
    TRACE (1, "found: %d: '%s'", found, dll_spec);
    if (found)
       pi->dll_name = STRDUP (dll_spec);
  }
  popen_append_clear();
  return (found);
}

/**
 * Check the Python path directory `pp->dir` and `opt.file_spec` against
 * a matching `*.dist-info` or a `*.egg-info` sub-directory.
 *
 * If at least one is found, these sub-directories shall be ignored.
 */
static bool check_if_dist_info_dirs (const python_path *pp)
{
  char dist_info_spec [_MAX_PATH];
  char egg_info_spec  [_MAX_PATH];
  WIN32_FIND_DATA ff;
  HANDLE          hnd;
  bool            is_dist_info, is_egg_info;

  snprintf (dist_info_spec, sizeof(dist_info_spec), "%s%c%s*.dist-info", pp->dir, DIR_SEP, opt.file_spec);
  snprintf (egg_info_spec, sizeof(dist_info_spec), "%s%c%s*.egg-info", pp->dir, DIR_SEP, opt.file_spec);

  hnd = FindFirstFile (dist_info_spec, &ff);
  FindClose (hnd);
  is_dist_info = (hnd != INVALID_HANDLE_VALUE);

  hnd = FindFirstFile (egg_info_spec, &ff);
  FindClose (hnd);
  is_egg_info = (hnd != INVALID_HANDLE_VALUE);

  if (is_dist_info || is_egg_info)
     TRACE (1, "%s -> is_dist_info: %d, is_egg_info: %d\n", pp->dir, is_dist_info, is_egg_info);
  return (is_dist_info || is_egg_info);
}

/**
 * Run a Python, figure out the `sys.path[]` array and search along that
 * for matches. If a `sys.path[]` component contains a ZIP/EGG-file, use
 * `process_zip()` to list files inside it for a match.
 *
 * \note not all .EGG-files are ZIP-files. `check_if_zip()` is used to test
 *       that and set `pp->is_zip` accordingly.
 */
static int py_search_internal (python_info *pi, bool reinit)
{
  char *str = NULL;
  int   i, max, found, found_zip;
  bool  use_cache = (opt.use_cache && smartlist_len(pi->modules) > 0);

  ASSERT (pi == g_pi);

  TRACE (1, "pi->variant: %d.\n", pi->variant);

  if (pi->is_embeddable && pi->bitness_ok)
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

  found = found_zip = 0;
  max = smartlist_len (pi->sys_path);

  for (i = 0; i < max; i++)
  {
    python_path *pp = smartlist_get (pi->sys_path, i);
    int          rc;

    /* Don't warn on missing .zip files in `sys.path[]` (unless in debug-mode)
     */
    if (opt.debug == 0 && !pp->exist && !stricmp(get_file_ext(pp->dir), "zip"))
       pp->exist = pp->is_dir = true;

    if (check_if_dist_info_dirs(pp))
       continue;

    if (pp->is_zip)
    {
      rc = process_zip (pi, pp->dir);
      found_zip += rc;
    }
    else
    {
      rc = process_dir (pp->dir, 0, pp->exist, false, pp->is_dir,
                        true, "sys.path[]", HKEY_PYTHON_PATH);
      found += rc;
    }
  }

  if (opt.verbose /* && found >= 1 */)
  {
    /* If there was a hit for `opt.file_spec` along the `sys.path[]`,
     * this could also be a hit for a matching module.
     */
    report_header_set ("Matches in \"%s\" modules:\n", pi->exe_name);
    found += py_print_modinfo (opt.file_spec, true);
  }

  return (found + found_zip);
}

/**
 * Find the next Python that can be selected.
 */
static python_info *py_select_next (enum python_variants which)
{
  python_info *pi = NULL;
  int    i, max;
  bool   okay = false;

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

  /* Prepare for next call to `py_select_next()`.
   */
  if (okay)
       py_index = i + 1;
  else py_index = -1;
  return (okay ? pi : NULL);
}

/**
 * Find the first Python that can be selected.
 */
static python_info *py_select_first (enum python_variants which)
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
  python_info *pi;
  int    i = 0, found = 0;

  for (pi = py_select_first(py_which);  /* if the 1st was okay, try the next too */
       pi;
       pi = py_select_next(py_which), i++)
  {
    bool reinit = (g_pi == pi) ? false : true; /* Need to call py_init_embedding() again? */

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
static python_info *add_python (const python_info *pi, const char *exe)
{
  python_info *pi2 = CALLOC (sizeof(*pi2), 1);
  const char  *base = basename (exe);
  const char **libs;

  _strlcpy (pi2->exe_dir, exe, base - exe);
  _strlcpy (pi2->program, pi->program, sizeof(pi2->program));
  pi2->exe_name = _fix_path (exe, NULL);
  pi2->do_warn_home      = true;
  pi2->do_warn_user_site = true;
  pi2->sys_path = smartlist_new();

  if (get_python_version(exe) >= 1)
  {
    pi2->ver_major = tmp_ver_major;
    pi2->ver_minor = tmp_ver_minor;
    pi2->ver_micro = tmp_ver_micro;
    pi2->user_site_path = tmp_user_site[0] ? STRDUP(tmp_user_site) : NULL;

    libs = (const char**) pi->libraries;
    if (get_dll_name(pi2, libs) || get_dll_name_from_sys_prefix(pi2))
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
    const python_info *all_pi;
    python_info       *pi;

    if ((de->d_attrib & FILE_ATTRIBUTE_DIRECTORY) ||
        (de->d_attrib & FILE_ATTRIBUTE_DEVICE))
      continue;

    base = basename (de->d_name);
    for (i = 0, all_pi = all_py_programs; i < DIM(all_py_programs); all_pi++, i++)
    {
      if (stricmp(base, all_pi->program) != 0)
         continue;

      if (cfg_ignore_lookup("[Python]", de->d_name))
         continue;

      found++;
      TRACE (1, "de->d_name: %s matches: '%s', variant: %d\n",
             de->d_name, all_pi->program, all_pi->variant);

      pi = add_python (all_pi, de->d_name);

      /* First Python found is default.
       */
      if (found == 1)
         pi->is_default = true;

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
 * The config-file handler for the `[Python]` section.
 * Currently only handles `"ignore = xx"` pairs.
 */
bool py_cfg_handler (const char *section, const char *key, const char *value)
{
  if (!stricmp(key, "ignore"))
      return cfg_ignore_handler (section, key, value);
  return (false);
}

/**
 * Build up the `pi->modules` list from the file-cache.
 */
static int get_modules_from_cache (python_info *pi)
{
  int i = 0;
  int dups = 0;

  if (!opt.use_cache)
     return (0);

  while (1)
  {
    python_module m;
    char   format [50];
    char  *mod_name, *mod_version, *location, *dist_info;
    int    rc, is_zip;

    snprintf (format, sizeof(format), "python_modules%d_%d = %%s,%%s,%%d,%%s,%%s", pi->py_index, i);
    rc = cache_getf (SECTION_PYTHON, format, &mod_name, &mod_version, &is_zip, &location, &dist_info);
    TRACE (3, "rc: %d.\n", rc);

    if (rc < 5)
       break;

    memset (&m, '\0', sizeof(m));
    m.is_zip    = is_zip;
    m.is_actual = true;
    _strlcpy (m.name, mod_name, sizeof(m.name));
    _strlcpy (m.version, mod_version, sizeof(m.version));
    _strlcpy (m.location, location, sizeof(m.location));
    _strlcpy (m.dist_info, dist_info, sizeof(m.dist_info));
    if (!py_add_module(pi, &m))
       dups++;
    i++;
  }
  return (i - dups);
}

/**
 * Try to get all Python information from the cache.
 */
static bool get_pythons_from_cache (void)
{
  python_info *pi;
  int    i, j, found;

  if (!opt.use_cache)
     return (false);

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
      pi->bitness_ok = true;  /* assume yes */
      check_bitness (pi, NULL);
    }

    smartlist_add (py_programs, pi);

    /* Get the 'sys.path[]' list for this Python.
     */
    j = 0;
    while (1)
    {
      python_path *pp;
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
static void write_to_cache (const python_info *pi)
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
    const python_module *m = smartlist_get (pi->modules, i);
    const char          *dist_info = "-";

    if (m->dist_info[0] != '-')
    {
      if (m->is_actual)
           dist_info = m->dist_info;
      else dist_info = py_filename (m->dist_info);
    }
    cache_putf (SECTION_PYTHON, "python_modules%d_%d = %s,%s,%d,%s,%s",
                pi->py_index, i, m->name, m->version, m->is_zip, m->location, dist_info);
  }

  max = smartlist_len (pi->sys_path);
  if (max == 0)
  {
    TRACE (2, "No cached `sys_path[]' for %s.\n", pi->exe_name);
    g_pi = (python_info*) pi;
    get_sys_path (pi);
  }

  max = smartlist_len (pi->sys_path);
  for (i = 0; i < max; i++)
  {
    const python_path *pp = smartlist_get (pi->sys_path, i);

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
static void py_test_internal (python_info *pi)
{
  bool reinit = (g_pi == pi);   /* Need to call py_init_embedding() again? */

  g_pi = pi;
  get_sys_path (pi);
  C_puts ("Python paths:\n");
  print_sys_path (pi, 0);

  if (reinit && pi->is_embeddable && pi->bitness_ok && !test_python_funcs(pi, reinit))
     C_puts ("Embedding failed.\n");
  else if (Anaconda_GetVersion)
     C_printf ("\nAnacondaGetVersion(): \"%s\"\n\n", (*Anaconda_GetVersion)());

  C_printf ("~6List of modules for %s:~0\n", py_filename(pi->exe_name));
  py_print_modinfo ("*", opt.verbose ? true : false);
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
  python_info *pi;
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
  python_info *pi;
  const char  *ignored;
  char         fname [_MAX_PATH] = { '\0' };
  int          i, num = 0, max = smartlist_len (py_programs);
  char         slash  = (opt.show_unix_paths ? '/' : '\\');

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

    if (pi->is_embeddable && !check_bitness(pi, &bitness))
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
static void get_install_path (const python_info *pi, const char *key_name)
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

    if (value[0] && data[0] && !stricmp(value, "ExecutablePath"))
    {
      char *slash = strrchr (data, '\\');
      char *end   = strchr (data,'\0') - 1;

      if (slash)
      {
        python_info *pi2 = CALLOC (sizeof(*pi2), 1);

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
      python_info *pi2 = CALLOC (sizeof(*pi2), 1);
      char        *end = strchr (data,'\0') - 1;

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
  static python_info pi;     /* filled in by `sscanf()` below */
  static int rec_level = 0;  /* recursion level */
  HKEY   key = NULL;
  DWORD  rc, num = 0;

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

    if (sscanf(value, "%d.%d-%2s", &pi.ver_major, &pi.ver_minor, bitness) >= 2)
    {
      if (bitness[0] == '6' && bitness[1] == '4' )
           pi.bitness = bit_64;
      else pi.bitness = bit_32;
      TRACE (2, " ver: %d.%d, bitness:s %d\n", pi.ver_major, pi.ver_major, pi.bitness);
    }
    else if (!stricmp(value, "InstallPath"))
    {
      get_install_path (&pi, sub_key);
    }

    rec_level++;
    enum_python_in_registry (sub_key);
    rec_level--;
  }

  if (key)
     RegCloseKey (key);
}

/**
 * Main initialiser function for this module:
 *  \li Clear the `PYTHONINSPECT` env-var (causes a .py-scripts to hang).
 *  \li Set the `PYTHON_COLORS=0` env-var to aoid colours in error-messages.
 *  \li If `opt.use_cache == true` get previous information from file-cache.
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

  SetEnvironmentVariable ("PYTHONINSPECT", NULL);
  SetEnvironmentVariable ("PYTHON_COLORS", "0");

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
    python_info *pi = smartlist_get (py_programs, i);
    int          len, indent = 1 + (int)f_len;
    char        *num_mod;
    char         mod_buf [20];
    char         version [20];

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
              "%*snum_mod:     -> %s\n"
              "%*sis_default:  -> %d\n",
           i, 2+longest_py_program, pi->program, pi->exe_name, version,
           indent+longest_py_program, "", pi->dll_name,
           indent+longest_py_program, "", pi->user_site_path ? pi->user_site_path : "<None>",
           indent+longest_py_program, "", py_variant_name(pi->variant), pi->is_default ? " (Default)" : "",
           indent+longest_py_program, "", num_mod,
           indent+longest_py_program, "", pi->is_default);

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
 * \param[in] wide true if the `av->wide[]` array should be created.
 *                 false if the `av->ascii[]` array should be created.
 */
static int make_arg_vector (arg_vector *av, const char **argv, bool wide)
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
 * \param[in] pi        The currently used Python-info variable.
 * \param[in] py_argv   The argument vector for the Python-script to run.
 * \param[in] capture   If `true`, `sys.stdout` in the Python-script is
 *                      captured and stored in the return value `str`.
 * \param[in] as_import If `true`, excute the program like a
 *                      `python -c "import ..."` statement would do.
 *
 * \retval If `capture == true`, an allocated string of the Python-script output. <br>
 *         If `capture == false`, this function returns `NULL`.
 *
 * \note No expansion is done on `py_argv[0]`. The caller should expand it to a
 *       Fuly Qualified Pathname before use. Call `py_argv[0] = _fix_path (py_argv[0], buf)` first.
 */
static char *py_exec_internal (python_info *pi, const char **py_argv, bool capture, bool as_import)
{
  const char *prog0, *fmt = NULL;
  char       *str, *prog;
  size_t      size;
  arg_vector  av = { 0, NULL, NULL };

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

  if (!as_import)
  {
    if (pi->ver_major >= 3)
    {
      make_arg_vector (&av, py_argv, true);
      (*PySys_SetArgvEx) (av.argc, av.wide, 1);
      fmt = PY_EXEC3_FMT();
    }
    else
    {
      make_arg_vector (&av, py_argv, false);
      (*PySys_SetArgvEx) (av.argc, (wchar_t**)av.ascii, 1);
      fmt = PY_EXEC2_FMT();
    }
  }

  if (capture)
       prog0 = "";
  else prog0 = "sys.stdout = old_stdout\n";   /* Undo what `setup_stdout_catcher()` did */

  if (as_import)
  {
    prog = alloca (1000);
    snprintf (prog, 1000, "sys.path.append (\"%s\")\n%s%s", current_dir, prog0, py_argv[0]);
  }
  else
  {
    size = strlen(prog0) + strlen(fmt) + 2 * strlen(py_argv[0]);
    prog = alloca (size + 1);

    strcpy (prog, prog0);
    snprintf (prog + strlen(prog0), size, fmt, py_argv[0], py_argv[0]);
  }

  str = call_python_func (pi, prog, __LINE__);

  /* If `capture == true`, the 'str' should now contain the Python output of one of the above
   * 'exec()' or 'execfile()' calls.
   * Caller must call 'FREE(str)' on this value when done.
   *
   * Otherwise, if `capture == false`, the `str` should now be NULL.
   */
  TRACE (2, "capture: %d, str:\n'%s'\n", capture, str ? str : "<none>");

  free_arg_vector (&av);
  return (str);
}

/**
 * Executes a Python script, optionally with arguments.
 *
 * If `py_which == ALL_PYTHONS`, do it for all supported and found Pythons.
 *
 * \param[in] py_argv  The Python-script to run must be in `py_argv[0]`.<br>
 *                     The remaining command-line is in `py_argv[1..]`.
 *
 * \param[in] capture  If `true`, `sys.stdout` in the Python-script is
 *                     captured and stored in the return value `str`.
 *
 * \retval str The captured string or `NULL`. If several `py_exec_internal()` are
 *             called, this string is from all calls merged together.
 *             Caller must call 'FREE(str)' on this value when done with it.
 */
char *py_execfile (const char **py_argv, bool capture, bool as_import)
{
  python_info *pi;
  char        *array [DIM(all_py_programs)+1];
  char        *ret, *str;
  int         i = 0;
  int         j = 0;

  memset (&array, '\0', sizeof(array));

  for (pi = py_select_first(py_which);   /* if the 1st was okay, try the next too */
       pi;
       pi = py_select_next(py_which), i++)
  {
    str = py_exec_internal (pi, py_argv, capture, as_import);
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

