/*
 * The Python functions for the envtool program.
 *
 * By Gisle Vanem <gvanem@yahoo.no> August 2011.
 *
 */

#include <stddef.h>
#include <fcntl.h>
#include <errno.h>
#include <assert.h>

#include "color.h"
#include "dirlist.h"
#include "envtool.h"
#include "envtool_py.h"
#include "smartlist.h"

/*
 * No need to include <Python.h> just for this:
 */
#define PyObject      void
#define PyThreadState void
#define Py_ssize_t    long

struct python_path {
       char dir [_MAX_PATH];  /* Fully qualified directory of this entry */
       int  exist;            /* does it exist? */
       int  is_dir;           /* and is it a dir; _S_ISDIR() */
       int  is_zip;           /* or is it a zip; an .EGG or .zip-file. */
     };

enum python_variants py_which = DEFAULT_PYTHON;

struct python_info {

  /* The basename of the specific Python interpreter.
   */
  const char *program;

  /* Which variant is this?
   */
  enum python_variants variant;

  /* Only a CPython program can be embeddable from a C-program.
   */
  BOOL is_embeddable;

  /* The list of expected .DLLs for this specific Python.
   * Tested for existance in either %SystemDir% and/or directory
   * of 'exe_name[]'
   */
  const char *libraries [4];

  /* The FQFN of 'program'.
   */
  char *exe_name;

  /* The FQFN of the .dll that matches the first 'libraries[]' format above.
   * If this Python 'is_embeddable', use this 'dll_name' in 'LoadLibrary()'
   * during 'py_init_embedding()'.
   */
  char *dll_name;

  /* The directory and basename of above 'exe_name'.
   */
  char dir [_MAX_PATH];
  char prog [20];

  /* The 'sys.path[]' array of above 'exe_name'.
   */
  smartlist_t *sys_path;

  /* The version info.
   */
  int ver_major, ver_minor, ver_micro;

  /* Bitness of 'exe_name'; 0, 32 or 64.
   */
  int bitness;

  /* Embedding requires bitness of CPython is the same as this program.
   */
  BOOL bitness_ok;

  /* This is the only default; i.e. the first 'program' in the $PATH.
   */
  BOOL is_default;

  /* Is this a CygWin Python?
   */
  BOOL is_cygwin;

  /* It's 'sys.prefix' used in Py_SetPythonHome().
   */
  char    *home_a;
  wchar_t *home_w;  /* for Python 3.x */

  /* The program-names used in Py_SetProgramName().
   */
  char    *prog_a;
  wchar_t *prog_w;  /* for Python 3.x */

  /* Warn once if the above is not set
   */
  BOOL     do_warn;

  /* Only if 'is_embeddable == TRUE':
   *   the stdout catcher object and
   *   the handle from LoadLibrary().
   */
  PyObject *catcher;
  HANDLE    dll_hnd;
};

/*
 * List of all Pythons we care about. Ignore the console-less 'pythonw.exe'
 * programs.
 */
static struct python_info all_py_programs[] = {

    /* PyPy */
    { "pypy.exe",      PYPY_PYTHON,   FALSE, { "~\\libpypy-c.dll", NULL }, },

    /* CPython */
    { "python.exe",    PY3_PYTHON,    TRUE,  { "~\\libpython%d.%d.dll", "%s\\python%d%d.dll", NULL }, },
    { "python.exe",    PY2_PYTHON,    TRUE,  { "~\\libpython%d.%d.dll", "%s\\python%d%d.dll", NULL }, },

    /* IronPython */
    { "ipy.exe",       IRON2_PYTHON,  FALSE, { "~\\IronPython.dll", NULL }, },
    { "ipy64.exe",     IRON2_PYTHON,  FALSE, { "~\\IronPython.dll", NULL }, },

    /* JavaPython */
    { "jython.exe",    JYTHON_PYTHON, FALSE, { "~\\jpython.dll", NULL }, }
  };

/* The global Python instance data.
 */
static struct python_info *g_py;
static int    our_bitness = (int) 8*sizeof(void*);

static int tmp_ver_major, tmp_ver_minor, tmp_ver_micro;
static int get_python_version (const char *exe_name);

/* The list Pythons from the PATH and from 'HKLM\Software\Python\PythonCore\xx\InstallPath'
 * locations. This is an array of 'struct python_info'.
 */
static smartlist_t *py_programs;

#define LOAD_FUNC(is_opt, f)  do {                                               \
                                f = (func_##f) GetProcAddress (py->dll_hnd, #f); \
                                if (!f && !is_opt) {                             \
                                  WARN ("Failed to find \"%s()\" in %s.\n",      \
                                        #f, py->dll_name);                       \
                                  goto failed;                                   \
                                }                                                \
                                DEBUGF (3, "Function %s(): %*s 0x%p\n",          \
                                           #f, 23-(int)strlen(#f), "", f);       \
                              } while (0)

#define DEF_FUNC(ret,f,args)  typedef ret (__cdecl *func_##f) args; \
                              static func_##f f

/*
 * We only need 1 set of func-ptr for each embeddable Python program function since
 * we can only embed 1 Python at a time.
 */
DEF_FUNC (void,        Py_InitializeEx,        (int init_sigs));
DEF_FUNC (void,        Py_Finalize,            (void));
DEF_FUNC (void,        Py_SetProgramName,      (char *name));
DEF_FUNC (void,        Py_SetPythonHome,       (void *home));
DEF_FUNC (int,         PyRun_SimpleString,     (const char *cmd));
DEF_FUNC (PyObject*,   PyImport_AddModule,     (const char *name));
DEF_FUNC (PyObject*,   PyObject_GetAttrString, (PyObject *o, char *attr));
DEF_FUNC (char *,      PyString_AsString,      (PyObject *o));    /* Python 2.x */
DEF_FUNC (char *,      PyBytes_AsString,       (PyObject *o));    /* Python 3.x */
DEF_FUNC (Py_ssize_t,  PyString_Size,          (PyObject *o));    /* Python 2.x */
DEF_FUNC (Py_ssize_t,  PyBytes_Size,           (PyObject *o));    /* Python 3.x */
DEF_FUNC (void,        PyObject_Free,          (PyObject *o));
DEF_FUNC (void,        Py_DecRef,              (PyObject *o));
DEF_FUNC (PyObject*,   PyObject_CallMethod,    (PyObject *o, char *method, char *fmt, ...));
DEF_FUNC (void,        initposix,              (void));  /* In Cygwin's 'libpython2.x.dll' */
DEF_FUNC (void,        PyInit_posix,           (void));  /* In Cygwin's 'libpython3.x.dll' */
DEF_FUNC (const char*, Anaconda_GetVersion,    (void));  /* In Anaconda's 'python3x.dll' */

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

const char *py_variant_name (enum python_variants v)
{
  switch (v)
  {
    case UNKNOWN_PYTHON:
         return ("Unknown");
    case DEFAULT_PYTHON:
         return ("Default");
    case ALL_PYTHONS:
         return ("All");
    default:
         return list_lookup_name (v, full_names, DIM(full_names));
  }
}

static int compare_strings (const void *_s1, const void *_s2)
{
  const char *s1 = *(const char**) _s1;
  const char *s2 = *(const char**) _s2;
  return stricmp (s1, s2);
}

const char **py_get_variants (void)
{
  static char *result [DIM(all_py_programs)+1];
  const struct python_info *py;
  int   i, j;

  if (result[0])  /* Already done this */
     return (const char**) result;

  for (i = j = 0, py = all_py_programs; i < DIM(all_py_programs); py++, i++)
  {
    switch (py->variant)
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

  DEBUGF (3, "j: %d\n", j);
  for (i = 0; i < DIM(result); i++)
     DEBUGF (3, "py_get_variants(); result[%d] = %s\n", i, result[i]);

  qsort (&result[0], j, sizeof(char*), compare_strings);

  /* Make a unique result list.
   */
  for (i = 0; i < j; i++)
    if (i > 0 && result[i] && !strcmp(result[i],result[i-1]))
    {
      memmove (&result[i-1], &result[i], (DIM(result) - i) * sizeof(const char*));
      j--;
    }

  DEBUGF (3, "\n");
  for (i = 0; i < j; i++)
     DEBUGF (3, "py_get_variants(); result[%d] = %s\n", i, result[i]);

  j = i;
  assert (j < DIM(result));

  result [j] = NULL;
  return (const char**) result;
}

/*
 * Select a Python that is found on PATH and that we've found the DLL for
 * and that is of a suitable variant.
 * Cannot select ALL_PYTHONS here.
 */
struct python_info *py_select (enum python_variants which)
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
      DEBUGF (1, "py_select (%d); \"%s\" -> \"%s\"\n", which, py_variant_name(pi->variant), pi->exe_name);
      return (pi);
    }
  }
  DEBUGF (1, "py_select (%d); \"%s\" not possible.\n", which, py_variant_name(which));
  return (NULL);
}

int py_get_info (const char **exe, const char **dll, struct ver_info *ver)
{
  const struct python_info *py;

  if (py_which == ALL_PYTHONS)          /* Not possible here */
       py = py_select (DEFAULT_PYTHON);
  else py = py_select (py_which);

  if (!py)
     return (0);

  if (exe)
     *exe = py->exe_name;

  if (dll)
     *dll = py->dll_name;

  if (ver)
  {
    ver->val_1 = py->ver_major;
    ver->val_2 = py->ver_minor;
    ver->val_3 = py->ver_micro;
    ver->val_4 = 0;
  }
  return (1);
}

static void fix_python_variant2 (struct python_info *py, enum python_variants v)
{
  if (v == PY2_PYTHON || v == PY3_PYTHON)
  {
    if (py->ver_major == 3)
         py->variant = PY3_PYTHON;
    else py->variant = PY2_PYTHON;
  }
  else
    py->variant = v;
}

static void free_sys_path (int i, struct python_info *pi)
{
  int max = smartlist_len (pi->sys_path);

  DEBUGF (3, "%d: %s, max: %d.\n", i, pi == g_py ? "is g_py " : "not g_py", max);

  for (i = 0; i < max; i++)
  {
    struct python_path *pp = smartlist_get (pi->sys_path, i);

    FREE (pp);
  }
}

static int longest_py_program = 0;  /* set in py_init() */
static int longest_py_version = 0;  /* set in py_init() */

static void print_sys_path (struct python_info *pi, size_t indent)
{
  int i, max = smartlist_len (pi->sys_path);

  for (i = 0; i < max; i++)
  {
    struct python_path *pp = smartlist_get (pi->sys_path, i);
    const char *dir = slashify (_fix_drive(pp->dir), opt.show_unix_paths ? '/' : '\\');

    if (indent)
         C_printf ("%*s%s\n", indent+longest_py_program, "", dir);
    else C_printf ("~6%3d: ~0%s\n", i, dir);
  }
}

static char *get_prog_name_ascii (const struct python_info *py)
{
  char  prog [_MAX_PATH];
  char *p = NULL;

  if (GetModuleFileNameA(NULL, prog, DIM(prog)))
     p = prog;

#if defined(__CYGWIN__)
  {
    char cyg_name [_MAX_PATH];

    if (cygwin_conv_path(CCP_WIN_A_TO_POSIX, prog, cyg_name, sizeof(cyg_name)) == 0)
       p =  cyg_name;
  }
#else
  if (py->is_cygwin)
     p = make_cyg_path (prog, prog);
#endif

  return (p ? STRDUP(p) : NULL);
}

static wchar_t *get_prog_name_wchar (const struct python_info *py)
{
  wchar_t  prog [_MAX_PATH];
  wchar_t *p = NULL;

  if (GetModuleFileNameW(NULL, prog, DIM(prog)))
     p = prog;

#if defined(__CYGWIN__)
  {
    wchar_t cyg_name [_MAX_PATH];

    if (cygwin_conv_path(CCP_WIN_W_TO_POSIX, prog, cyg_name, DIM(cyg_name)) == 0)
       p = cyg_name;
  }
#else
  if (py->is_cygwin)
     p = make_cyg_pathw (prog, prog);
#endif

  return (p ? WCSDUP(p) : NULL);
}

/*
 * Setup the 'py->prog_a' or 'py->prog_w'.
 * The allocated string (ASCII or Unicode) is freed in 'py_exit()'.
 */
static void set_python_prog (struct python_info *py)
{
  if (py->ver_major >= 3)
  {
    py->prog_w = get_prog_name_wchar (py);
    py->prog_a = NULL;
  }
  else
  {
    py->prog_a = get_prog_name_ascii (py);
    py->prog_w = NULL;
  }
}

/*
 * This should be the same as 'sys.prefix'.
 * The allocated string (ASCII or Unicode) is freed in 'py_exit()'.
 */
static void set_python_home (struct python_info *py)
{
#if defined(__CYGWIN__)
  py->home_a = STRDUP ("/usr/lib");
  py->home_w = NULL;
#else
  char *dir = py->dir;

  if (py->ver_major >= 3)
  {
    wchar_t buf [_MAX_PATH];

    buf[0] = L'\0';
    MultiByteToWideChar (CP_ACP, 0, dir, -1, buf, DIM(buf));
    if (py->is_cygwin)
    {
      py->home_w = WCSDUP (L"/usr");
      py->home_a = STRDUP ("/usr");
    }
    else
    {
      py->home_w = WCSDUP (buf);
      py->home_a = STRDUP (dir);
    }
  }
  else
  {
    /* Reallocate again because FREE() is used in py_exit()!
     */
    if (py->is_cygwin)
         py->home_a = STRDUP ("/usr");
    else py->home_a = STRDUP (dir);
    py->home_w = NULL;
  }
#endif
}

static void py_exit_embedding (struct python_info *py)
{
  if (py->dll_hnd && py->dll_hnd != INVALID_HANDLE_VALUE)
  {
    DEBUGF (4, "Calling Py_Finalize().\n");
    (*Py_Finalize)();
    CloseHandle (py->dll_hnd);
  }
  py->dll_hnd = INVALID_HANDLE_VALUE;
}

void py_exit (void)
{
  struct python_info *py;
  int    i, len;

  if (py_programs)
  {
    len = smartlist_len (py_programs);
    for (i = 0; i < len; i++)
    {
      py = smartlist_get (py_programs, i);
      FREE (py->prog_a);
      FREE (py->prog_w);
      FREE (py->home_a);
      FREE (py->home_w);
      FREE (py->dll_name);
      FREE (py->exe_name);
      if (py->is_embeddable)
         py_exit_embedding (py);

      if (py->sys_path)
      {
        free_sys_path (i, py);
        smartlist_free (py->sys_path);
      }
      FREE (py);
    }
    smartlist_free (py_programs);
  }
  g_py = NULL;
}

/*
 * Setup a class-instance for catching all output written
 * using 'sys.stdout'. I.e. 'print(...)' and 'os.write(1, ...)'.
 * This instance must reside at the global '__main__' level.
 *
 * Thus the Python printed strings are retrieved in the C-world by
 * 'catcher.value'. I.e:
 *   obj = (*PyObject_GetAttrString) (py_catcher, "value");
 *
 * \todo:
 *   Use 'StringIO()' class instead?
 *
 * Ref:
 *   http://stackoverflow.com/questions/4307187/how-to-catch-python-stdout-in-c-code
 */
static PyObject *setup_stdout_catcher (void)
{
  static char code[] = "import sys\n"                               \
                       "PY3 = (sys.version_info[0] == 3)\n"         \
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

  PyObject *mod = (*PyImport_AddModule) ("__main__");          /* create main module */
  int       rc  = (*PyRun_SimpleString) (code);                /* invoke code to redirect */
  PyObject *obj = (*PyObject_GetAttrString) (mod, "catcher");  /* get our catcher created above */

  DEBUGF (5, "code: '%s'\n", code);
  DEBUGF (4, "mod: %p, rc: %d, obj: %p\n", mod, rc, obj);
  return (obj);
}

/*
 * Do NOT call this unless 'py->is_embeddable == TRUE'.
 */
static int py_init_embedding (struct python_info *py)
{
  char *exe, *dll;

#if 0
  g_py = py_select (py_which);
  if (!g_py)
     return (0);
#endif

  exe = py->exe_name;
  dll = py->dll_name;
  if (!exe || !dll)
  {
    WARN ("Failed to find any Python DLLs.\n");
    return (0);
  }

  py->dll_hnd = LoadLibrary (dll);
  if (!py->dll_hnd || py->dll_hnd == INVALID_HANDLE_VALUE)
  {
    WARN ("Failed to load %s; %s\n", dll, win_strerror(GetLastError()));
    py->is_embeddable = FALSE;  /* Do not do this again */
    return (0);
  }

  DEBUGF (2, "Full DLL name: \"%s\". Handle: 0x%p\n", py->dll_name, py->dll_hnd);

  LOAD_FUNC (0, Py_InitializeEx);
  LOAD_FUNC (0, Py_Finalize);
  LOAD_FUNC (0, Py_SetProgramName);
  LOAD_FUNC (0, Py_SetPythonHome);
  LOAD_FUNC (0, PyRun_SimpleString);
  LOAD_FUNC (0, PyObject_GetAttrString);
  LOAD_FUNC (0, PyImport_AddModule);
  LOAD_FUNC (1, PyString_AsString);
  LOAD_FUNC (1, PyBytes_AsString);
  LOAD_FUNC (1, PyString_Size);
  LOAD_FUNC (1, PyBytes_Size);
  LOAD_FUNC (0, PyObject_CallMethod);
  LOAD_FUNC (0, PyObject_Free);
  LOAD_FUNC (0, Py_DecRef);
  LOAD_FUNC (1, initposix);
  LOAD_FUNC (1, PyInit_posix);
  LOAD_FUNC (1, Anaconda_GetVersion);

  if (initposix || PyInit_posix)
     py->is_cygwin = TRUE;

  if (py->ver_major >= 3)
  {
    PyString_AsString = PyBytes_AsString;
    PyString_Size     = PyBytes_Size;

    DEBUGF (2, "Py_SetProgramName (\"%" WIDESTR_FMT "\")\n", py->prog_w);
    (*Py_SetProgramName) ((char*)py->prog_w);

    DEBUGF (2, "Py_SetPythonHome (\"%" WIDESTR_FMT "\")\n", py->home_w);
    (*Py_SetPythonHome) (py->home_w);
  }
  else
  {
    DEBUGF (2, "Py_SetProgramName (\"%s\")\n", py->prog_a);
    (*Py_SetProgramName) (py->prog_a);

    DEBUGF (2, "Py_SetPythonHome (\"%s\")\n", py->home_a);
    (*Py_SetPythonHome) (py->home_a);
  }

  (*Py_InitializeEx) (0);
  DEBUGF (3, "Py_InitializeEx (0) passed\n");

  py->catcher = setup_stdout_catcher();
  if (py->catcher)
     return (1);

  /* Fall through */

failed:
  py_exit_embedding (py);
  return (0);
}

/*
 * Call the Python code in 'py_prog'. The returned memory (!= NULL)
 * MUST be freed using FREE().
 */
static char *call_python_func (struct python_info *py, const char *py_prog)
{
  PyObject *obj;
  char     *str = NULL;
  int       rc;

  DEBUGF (3, "py_prog:\n"
             "----------------------\n%s\n"
             "----------------------\n", py_prog);

  rc  = (*PyRun_SimpleString) (py_prog);
  obj = (*PyObject_GetAttrString) (py->catcher, "value");

  DEBUGF (4, "rc: %d, obj: %p\n", rc, obj);

  if (rc == 0 && obj)
  {
    Py_ssize_t size = (*PyString_Size) (obj);

    if (size > 0)
       str = STRDUP ((*PyString_AsString)(obj));

    /* Reset the 'py->catcher' buffer value to prepare for next call
     * to this 'call_python_func()'.
     */
    (*PyObject_CallMethod) (py->catcher, "reset", NULL);
    DEBUGF (4, "PyString_Size(): %ld\n", size);
  }
  return (str);
}

static BOOL test_python_funcs (struct python_info *py)
{
  const char *prog = "import sys, os\n"
                     "print(sys.version_info)\n"
                     "for i in range(5):\n"
                     "  print(\"  Hello world\")\n";
  const char *name = py_variant_name (py->variant);
  char *str;

  if (!py_init_embedding(py))
     return (FALSE);

  /* The 'str' should now contain the Python output of above 'prog'.
   */
  str = call_python_func (py, prog);
  C_printf ("~3Captured output of %s:~0\n** %s **\n", name, str);
  FREE (str);

  /* Restore 'sys.stdout' to it's old value. Thus this should return no output.
   */
  C_printf ("~3The rest should not be captured:~0\n");
  str = call_python_func (py, "sys.stdout = old_stdout\n");
  FREE (str);

  str = call_python_func (py, prog);
  C_printf ("~3Captured output of %s now:~0\n** %s **\n", name, str);
  FREE (str);
  return (TRUE);
}

/*
 * Create a %TEMP%-file and write a .py-script to it.
 * The file-name 'tmp' is allocated in misc.c. Caller should call 'unlink(tmp)' and
 * MUST call 'FREE(tmp)' on this ret-value.
 */
static char *fprintf_py (const char *fmt, ...)
{
  char   *tmp = create_temp_file();
  FILE   *fil;
  va_list args;

  if (!tmp)
     return (NULL);

  fil = fopen (tmp, "w+t");
  if (!fil)
  {
    FREE (tmp);
    return (NULL);
  }
  va_start (args, fmt);
  vfprintf (fil, fmt, args);
  va_end (args);
  fclose (fil);
  return (tmp);
}

/**
 * Parse the output line from the 'PY_ZIP_LIST' program below.
 * Each line on the form:
 *   81053 20130327.164158 stem/control.py
 *   ^     ^
 *   size  time: YYYYMMDD.HHMMSS
 */
static int report_zip_file (struct python_info *py, const char *zip_file, char *output)
{
  static char *sys_prefix = "$PYTHONHOME";
  struct tm    tm;
  const  char *space, *p, *file_within_zip;
  char   report [1024];
  int    num, len;
  time_t mtime;
  long   fsize;

  space = strrchr (output, ' ');
  if (!space)
  {
    WARN (" (1) Unexpected zipinfo line: %s\n", output);
    return (0);
  }

  file_within_zip = space + 1;
  p = space - 1;
  while (p > output && *p != ' ')
      --p;

  if (p <= output)
  {
    WARN (" (2) Unexpected zipinfo line: %s\n", output);
    return (0);
  }

  p++;
  memset (&tm, 0, sizeof(tm));
  num = sscanf (output, "%ld %4u%2u%2u.%2u%2u%2u",
                &fsize, &tm.tm_year, &tm.tm_mon, &tm.tm_mday,
                        &tm.tm_hour, &tm.tm_min, &tm.tm_sec);
  if (num != 7)
  {
    WARN (" (3) Unexpected zipinfo line: %s\n", output);
    return (0);
  }

  tm.tm_year -= 1900;
  tm.tm_mon  -= 1;

  mtime = mktime (&tm);
  if (mtime == -1)
  {
    WARN (" (4) Unexpected timestamp: \"%.*s\".\n", (int)(space-p), p);
    DEBUGF (1, "parsed tm: %04u%02u%02u.%02u%02u%02u. num: %d\n",
            tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
            tm.tm_hour, tm.tm_min, tm.tm_sec, num);
    return (0);
  }

  if (py->do_warn)
  {
    DEBUGF (1, "py->home_a: %s\n", py->home_a);
    if (py->home_a && !FILE_EXISTS(py->home_a))
       WARN ("%s points to non-existing directory: \"%s\".\n", sys_prefix, py->home_a);
    py->do_warn = FALSE;
  }

  len = snprintf (report, sizeof(report), "%s  (", file_within_zip);

  /* Figure out if and where 'py_home_a' and 'zip_file' overlaps.
   */
  p = py->home_a ? path_ltrim (zip_file, py->home_a) : zip_file;

  if (IS_SLASH(*p))  /* if 'py_home_a' doesn't end in a slash. */
     p++;

  DEBUGF (1, "p: '%s', py->home_a: '%s', zip_file: '%s'\n", p, py->home_a, zip_file);

  if (p != zip_file && !strnicmp(zip_file,py->home_a,strlen(py->home_a)))
       snprintf (report+len, sizeof(report)-len, "%s\\%s)", sys_prefix, p);
  else snprintf (report+len, sizeof(report)-len, "%s)", zip_file);

  /* zipinfo always reports 'file_within_zip' with '/' slashes. But simply slashify the
   * complete 'report' to use either '\\' or '/'.
   */
  _strlcpy (report, slashify(report, opt.show_unix_paths ? '/' : '\\'), sizeof(report));

  /* \todo: incase '--pe-check' is specified and 'report' file is a .pyd-file,
   *        we should save the .pyd to a %TMP-file and examine it in report_file().
   */
  report_file (report, mtime, fsize, FALSE, FALSE, HKEY_PYTHON_EGG);
  return (1);
}

static char *zip_output = NULL;

static int get_zip_output (char *str, int index)
{
  DEBUGF (2, "str (index: %d): \"%s\"\n", index, str);
  ASSERT (index >= 0);
  zip_output = REALLOC (zip_output, 4*strlen(str));
  if (index == 0)
     zip_output[0] = '\0';
  strcat (zip_output, str);
  return (1);
}

/*
 * List a ZIP/EGG-file (file) for a matching file_spec.
 *
 * Note:
 *   'fnmatch.fnmatch ("EGG-INFO/dependency_links.txt", "egg*.txt")' will return True.
 *   We are not interested in the dir-part. Hence get the basename of 'f.filename'
 *   first. Thus:
 *     "EGG-INFO/requires.txt" -> False
 *     "egg-timer.txt"         -> True
 *
 * This goes into a buffer used by 'call_python_func()'.
 */
#define PY_ZIP_LIST                                                                   \
          "import os, sys, fnmatch, zipfile\n"                                        \
          "PY3 = (sys.version_info[0] == 3)\n"                                        \
          "\n"                                                                        \
          "def trace (s):\n"  /* trace to stderr (2) */                               \
          "  if PY3:\n"                                                               \
          "    os.write (2, bytes(s,\"UTF-8\"))\n"                                    \
          "  else:\n"                                                                 \
          "    os.write (2, s)\n"                                                     \
          "\n"                                                                        \
          "def print_zline (f, debug):\n"                                             \
          "  base = os.path.basename (f.filename)\n"                                  \
          "  if debug >= 3:\n"                                                        \
          "    trace ('egg-file: %%s, base: %%s\\n' %% (f.filename, base))\n"         \
          "  if fnmatch.fnmatch(base, '%s'):\n"     /* opt.file_spec */               \
          "    date = \"%%4d%%02d%%02d\"  %% (f.date_time[0:3])\n"                    \
          "    time = \"%%02d%%02d%%02d\" %% (f.date_time[3:6])\n"                    \
          "    str = \"%%d %%s.%%s %%s\"  %% (f.file_size, date, time, f.filename)\n" \
          "    if debug > 0:\n"                                                       \
          "      trace ('str: \"%%s\"\\n' %% str)\n"                                  \
          "    print (str)\n"                                                         \
          "\n"                                                                        \
          "zf = zipfile.ZipFile (r\"%s\", 'r')\n"   /* zfile */                       \
          "for f in zf.infolist():\n"                                                 \
          "  print_zline (f, %d)\n"                 /* opt.debug */

static int process_zip (struct python_info *py, const char *zfile)
{
  char  cmd [_MAX_PATH + 1000];
  char *line, *tmp, *str = NULL;
  int   len, found = 0;

  if (sizeof(cmd) < sizeof(PY_ZIP_LIST) + _MAX_PATH + 100)
     FATAL ("cmd[] buffer too small.\n");

  if (py->is_embeddable)
  {
    len = snprintf (cmd, sizeof(cmd), PY_ZIP_LIST, opt.file_spec, zfile, opt.debug);
    str = call_python_func (py, cmd);
    DEBUGF (2, "cmd-len: %d, Python output: \"%s\"\n", len, str);
  }
  else if ((tmp = fprintf_py(PY_ZIP_LIST, opt.file_spec, zfile, opt.debug)) != NULL)
  {
    popen_runf (get_zip_output, "%s %s", py->exe_name, tmp);
    str = zip_output;
    zip_output = NULL;
    if (opt.debug == 0)
       unlink (tmp);
    FREE (tmp);
  }

  if (str)
  {
    for (found = 0, line = strtok(str,"\n"); line; line = strtok(NULL,"\n"), found++)
    {
      DEBUGF (2, "line: \"%s\", found: %d\n", line, found);
      if (!report_zip_file(py, zfile, line))
         break;
    }
    FREE (str);
  }

  if (found == 0)
     DEBUGF (1, "No matches in %s for %s.\n", zfile, opt.file_spec);

  return (found);
}

/*
 * Allocate a 'python_path' node and add it to 'pi->sys_path[]' smartlist.
 */
static void add_sys_path (struct python_info *pi, const char *dir)
{
  struct python_path *pp = CALLOC (1, sizeof(*pp));
  struct stat st;

  _strlcpy (pp->dir, dir, sizeof(pp->dir));
  memset (&st, '\0', sizeof(st));
  pp->exist  = (stat(dir, &st) == 0);
  pp->is_dir = pp->exist && _S_ISDIR(st.st_mode);
  pp->is_zip = pp->exist && _S_ISREG(st.st_mode) && check_if_zip (dir);

  smartlist_add (g_py->sys_path, pp);
}

/*
 * Build up the 'g_py->sys_path[]' array.
 * If 'index == -1' we are called from 'call_python_func()'.
 * Otherwise we are the 'popen_runf()' callback.
 */
static int build_sys_path (char *str, int index)
{
  if (index == -1)
  {
    char *l = strtok (str, "\n");

    for (index = 0; l; index++)
    {
      DEBUGF (2, "index: %d: \"%s\"\n", index, l);
      add_sys_path (g_py, l);
      l = strtok (NULL, "\n");
    }
  }
  else
  {
    DEBUGF (2, "index: %d: \"%s\"\n", index, str);
    add_sys_path (g_py, str);
  }
  return (1);
}

/*
 * Run python with this on the cmd-line to get the version tripplet.
 */
#define PY_GET_VERSION  "import sys; print (sys.version_info)"

/*
 * Run python, figure out the 'sys.path[]' array and search along that
 * for matches. If a 'sys.path[]' component contains a ZIP/EGG-file, use
 * 'process_zip()' to list files inside it for a match.
 *
 * Note:
 *   not all .EGG-files are ZIP-files. 'check_if_zip()' is used to test
 *   that and set 'py->is_zip' accordingly.
 *
 * This is used if 'py->is_embeddable == TRUE' in the parameter to
 * 'call_python_func()'.
 */
#define PY_PRINT_SYS_PATH   "import sys\n" \
                            "for (i,p) in enumerate(sys.path):\n" \
                            "  print('%s\\n' % p)\n"

/* And these are used (in Python2 or 3) when 'py->is_embeddable == FALSE'.
 */
#define PY_PRINT_SYS_PATH2  "import os, sys; " \
                            "[os.write(1,'%s\\n' % p) for (i,p) in enumerate(sys.path)]"

#define PY_PRINT_SYS_PATH3  "import sys; " \
                            "[print(p) for (i,p) in enumerate(sys.path)]"

/*
 * \todo:
 *   CygWin's Python doesn't like the ";" and "\" in %PYTHONPATH.
 *   Try to detect Cygwin and please it before calling 'popen_runf()'.
 *   Do something like 'cygwin_create_path (CCP_WIN_A_TO_POSIX, dir)'
 *   does in CygWin.
 */
static int get_sys_path (struct python_info *pi)
{
  ASSERT (pi == g_py);
  return popen_runf (build_sys_path, "%s -c \"%s\"",
                     pi->exe_name, pi->ver_major >= 3 ?
                     PY_PRINT_SYS_PATH3 : PY_PRINT_SYS_PATH2);
}

/*
 * \todo:
 *   If multiple DLLs with same name but different time-stamps are found
 *   (in 'pi->dir' and 'sys_dir'), report a warning.
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
  size_t      i, num = DIM (pi->libraries);

  _st1 = _st2 = FALSE;

  for (i = 0, lib_fmt = libs[0]; lib_fmt && i < num; lib_fmt = libs[++i])
  {
    if (!strncmp(lib_fmt,"%s\\",3))
    {
      snprintf (dll1, sizeof(dll1), lib_fmt, pi->dir, pi->ver_major, pi->ver_minor);
      snprintf (dll2, sizeof(dll2), lib_fmt, sys_dir, pi->ver_major, pi->ver_minor);
    }
    else if (!strncmp(lib_fmt,"~\\",2))
    {
      strcpy (dll1, pi->dir);
      snprintf (dll1+strlen(dll1), sizeof(dll1)-strlen(dll1), lib_fmt+1, pi->ver_major, pi->ver_minor);
      dll2[0] = '\0';
    }
    else
    {
      snprintf (dll1, sizeof(dll1), lib_fmt, pi->ver_major, pi->ver_minor);
      dll2[0] = '\0';
    }

    DEBUGF (1, "checking for:\n"
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
      newest = dll1;    /* The one in 'exe_dir' */
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
    DEBUGF (1, "Found newest DLL: \"%s\", \"%s\"\n", newest, get_time_str(use_st->st_mtime));
  }
  return (newest != NULL);
}

int py_search (void)
{
  char *str = NULL;
  int   i, len, found;

  g_py = py_select (py_which);
  if (!g_py)
  {
    WARN ("%s was not found on PATH.\n", py_variant_name(py_which));
    return (0);
  }

  if (g_py->is_embeddable)
  {
    if (!py_init_embedding(g_py))
       return (0);

    str = call_python_func (g_py, PY_PRINT_SYS_PATH);
    if (!str)
       return (0);
    build_sys_path (str, -1);
    FREE (str);
  }
  else
    get_sys_path (g_py);

  found = 0;
  len = smartlist_len (g_py->sys_path);

  for (i = 0; i < len; i++)
  {
    struct python_path *pp = smartlist_get (g_py->sys_path, i);

    /* Don't warn on missing .zip files in 'sys.path[]' (unless in debug-mode)
     */
    if (!opt.debug && !pp->exist && !stricmp(get_file_ext(pp->dir), "zip"))
       pp->exist = pp->is_dir = TRUE;

    if (pp->is_zip)
         found += process_zip (g_py, pp->dir);
    else found += process_dir (pp->dir, 0, pp->exist, pp->is_dir,
                               TRUE, "sys.path[]", NULL, FALSE);
  }
  return (found);
}

static int match_python_exe (const char *dir)
{
  struct od2x_options opts;
  struct dirent2     *de;
  struct python_info *py, *py2;
  const char *base;
  DIR2 *dp;
  int   i, rc = 1;
  static int found = 0;

  memset (&opts, '\0', sizeof(opts));
  opts.pattern = "*.exe";

  dp = opendir2x (dir, &opts);
  if (!dp)
     return (rc);

  while ((de = readdir2(dp)) != NULL)
  {
    if ((de->d_attrib & FILE_ATTRIBUTE_DIRECTORY) ||
        (de->d_attrib & FILE_ATTRIBUTE_DEVICE))
      continue;

    base = basename (de->d_name);
    for (i = 0, py = all_py_programs; i < DIM(all_py_programs); py++, i++)
    {
      if (stricmp(base,py->program))
         continue;

      found++;
      DEBUGF (1, "de->d_name: %s matches: '%s', variant: %d\n",
              de->d_name, py->program, py->variant);

      py2 = CALLOC (sizeof(*py2), 1);

      _strlcpy (py2->dir, de->d_name, base - de->d_name);
      _strlcpy (py2->prog, base, sizeof(py2->prog));
      py2->program = py->program;
      py2->exe_name = _fix_path (de->d_name, NULL);
      py2->dll_hnd  = INVALID_HANDLE_VALUE;
      py2->do_warn  = TRUE;

      /* First Python found is defined as default.
       */
      if (found == 1)
         py2->is_default = TRUE;

      py2->sys_path = smartlist_new();

      tmp_ver_major = tmp_ver_minor = tmp_ver_micro = -1;

      if (get_python_version(de->d_name) >= 1)
      {
        py2->ver_major = tmp_ver_major;
        py2->ver_minor = tmp_ver_minor;
        py2->ver_micro = tmp_ver_micro;
        if (get_dll_name(py2, py->libraries))
        {
         /* Only CPythons are embeddable here.
          * For now, assume the bitness is okay (LoadLibrary() will succeed).
          */
          py2->is_embeddable = (py->variant == PY2_PYTHON || py->variant == PY3_PYTHON);
          if (py2->is_embeddable)
             py2->bitness_ok = TRUE;

          fix_python_variant2 (py2, py->variant);
          set_python_home (py2);
          set_python_prog (py2);
        }
      }
      smartlist_add (py_programs, py2);

      /* If we found all Pythons we can handle, there is no point searching
       * further along the PATH. Stop by returning 0.
       */
      if (found >= DIM(all_py_programs))
         rc = 0;
      break;
    }
  }
  closedir2 (dp);
  return (rc);
}

/*
 * Search all directories on PATH for matches to 'all_py_programs::program'.
 */
static int get_python_exe_names (void)
{
  char *path = getenv_expand ("PATH");
  char *dir, dir_sep[2] = ";";
  int   max, i, found;

#ifdef __CYGWIN__
  dir_sep[0] = ':';
#endif

  dir = strtok (path, dir_sep);
  DEBUGF (1, "\n");

  for (found = 0; dir; dir = strtok(NULL,dir_sep))
  {
     if (!match_python_exe(dir))
        break;
     found++;
  }

  max = smartlist_len (py_programs);
  for (i = 0; i < max; i++)
  {
    const struct python_info *pi = smartlist_get (py_programs, i);

    DEBUGF (1, "%d: %s\\%s, %d.%d.%d\n",
            i, pi->dir, pi->prog, pi->ver_major, pi->ver_minor, pi->ver_micro);
  }
  FREE (path);
  return (found);
}

/*
 * Loop over 'all_py_programs[]' and do some tests on a Python matching 'py_which'.
 * This can be the 'default', one specific Python or 'all'.
 * This must be called after 'py_init()' has been called.
 */
int py_test (void)
{
  struct python_info *pi;
  int    i, found = 0, max = smartlist_len (py_programs);

  for (i = 0; i < max; i++)
  {
    pi = smartlist_get (py_programs, i);
    enum python_variants which = py_which;
    BOOL test_it = ( which == ALL_PYTHONS || pi->variant == which ||
                    (which == DEFAULT_PYTHON && pi->is_default) ) &&
                   pi->exe_name;

    if (i > 0)
    {
      const struct python_info *pi2 = smartlist_get (py_programs, i-1);

      /* This should never become TRUE.
       */
      if (pi2->variant == pi->variant && !stricmp(pi2->prog,pi->prog))
         test_it = FALSE;
    }
    if (which == ALL_PYTHONS)
       py_which = pi->variant;

    C_printf ("~6Will%s try to test: ~3%s~0%s (%sembeddable): %s\n",
              test_it ? "" : " ~5not~6",
              py_variant_name(pi->variant),
              pi->is_default     ? " ~6(Default)~0," : "",
              !pi->is_embeddable ? "not "            : "",
              pi->exe_name       ? pi->exe_name      : "~5Not found~0");

    if (test_it)
    {
      g_py = pi;
      get_sys_path (pi);
      print_sys_path (pi, 0);
      if (pi->is_embeddable && !test_python_funcs(pi))
         C_puts ("Embedding failed.");
      found++;
      C_putc ('\n');
    }
    py_which = which;
  }
  return (found);
}

/*
 * 'popen_runf()' callback to get the Python version.
 */
static int report_py_version_cb (char *output, int line)
{
  const char *prefix = "sys.version_info";  /* 'pypy.exe -c "import sys; print(sys.version_info)"' doesn't print this */
  int         num;

  if (!strncmp(output,prefix,strlen(prefix)))
     output += strlen (prefix);

  num = sscanf (output, "(major=%d, minor=%d, micro=%d",
                &tmp_ver_major, &tmp_ver_minor, &tmp_ver_micro);

  DEBUGF (1, "Python ver: %d.%d.%d\n", tmp_ver_major, tmp_ver_minor, tmp_ver_micro);
  ARGSUSED (line);
  return  (num >= 2);
}

/*
 * Get the Python version.
 */
static int get_python_version (const char *exe_name)
{
  tmp_ver_major = tmp_ver_minor = tmp_ver_micro = -1;
  return (popen_runf (report_py_version_cb, "%s -c \"%s\"", exe_name, PY_GET_VERSION) >= 1);
}

void py_searchpaths (void)
{
  struct python_info *pi;
  int    i, num = 0, max = smartlist_len (py_programs);

  for (i = 0; i < max; i++)
  {
    char fname [_MAX_PATH] = { '\0' };
    char version [12] = { '\0' };

    pi = smartlist_get (py_programs, i);

    if (pi->ver_major > -1 && pi->ver_minor > -1 && pi->ver_micro > -1)
         snprintf (version, sizeof(version), "(%d.%d.%d)", pi->ver_major, pi->ver_minor, pi->ver_micro);
    else if (pi->exe_name)
         _strlcpy (version, "(ver: ?)", sizeof(version));

    if (pi->exe_name)
    {
      _strlcpy (fname, slashify(pi->exe_name, opt.show_unix_paths ? '/' : '\\'), sizeof(fname));
      num++;
    }

    C_printf ("   %s %-*s %-*s -> ~%c%s~0",
              pi->is_default ? "~3(1)~0" : "   ",
              1+longest_py_program, pi->program,
              2+longest_py_version, version,
              fname[0] ? '6' : '5',
              fname[0] ? fname : "Not found");

    if (pi->is_embeddable && !pi->bitness_ok)
       C_printf (" (embeddable, but not %d bits)", our_bitness);
    else if (pi->dll_name)
       C_printf (" (%sembeddable)", pi->is_embeddable ? "": "not ");
    C_printf (", %d bits\n", pi->bitness);

    if (pi->exe_name && opt.do_version >= 3)
    {
      g_py = pi;
      get_sys_path (g_py);
      print_sys_path (g_py, 23);
    }
  }

  if (num > 0)
     C_puts ("   ~3(1)~0 Default Python (first found on PATH).\n");
}

/*
 * Add the REG_SZ data in a 'HKLM\Software\Python\PythonCore\xx\InstallPath' key
 * to the 'py_programs' smartlist.
 */
static void get_install_path (const char *key_name, const struct python_info *pi)
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

    DEBUGF (2, "   value: \"%s\", data: \"%s\"\n",
            value[0] ? value : "(Standard)", data[0] ? data : "(no data)");

    if (value[0] && data[0] && !stricmp(value,"ExecutablePath"))
    {
      struct python_info *pi2 = MALLOC (sizeof(*pi2));
      char  *slash = strrchr (data, '\\');
      char  *end = strchr (data,'\0') - 1;

      pi2->ver_major = pi->ver_major;
      pi2->ver_minor = pi->ver_minor;
      pi2->bitness   = pi->bitness;
      _strlcpy (pi2->dir, data, end-data);
      _strlcpy (pi2->prog, slash+1, slash-data);
      pi2->exe_name = STRDUP (data);
      smartlist_add (py_programs, pi2);
    }
    else if (data[0] && !value[0])
    {
      struct python_info *pi2 = MALLOC (sizeof(*pi2));
      char  *end = strchr (data,'\0') - 1;

      if (*end == '\\')
         *end = '\0';

      pi2->ver_major = pi->ver_major;
      pi2->ver_minor = pi->ver_minor;
      pi2->bitness   = pi->bitness;
      _strlcpy (pi2->dir, data, sizeof(pi2->dir));
      _strlcpy (pi2->prog, "python.exe", sizeof(pi2->prog));
      snprintf (data, sizeof(data), "%s\\%s", pi2->dir, pi2->prog);
      pi2->exe_name = STRDUP (data);
      smartlist_add (py_programs, pi2);
    }
  }
  if (key)
     RegCloseKey (key);
}

/*
 * Recursively walks the Registry branch under "HKLM\Software\Python\PythonCore".
 * Look for "InstallPath" keys and gather the REG_SZ "InstallPath" values.
 */
static void enum_python_install_paths (const char *key_name)
{
  static struct python_info pi;   /* filled in sscanf() below */
  static int rec_level = 0;       /* recursion level */
  HKEY  key = NULL;
  DWORD rc, num = 0;

  rc = RegOpenKeyEx (HKEY_LOCAL_MACHINE, key_name, 0, reg_read_access(), &key);

  DEBUGF (2, "RegOpenKeyEx (HKLM\\%s)\n", key_name);

  while (rc == ERROR_SUCCESS)
  {
    char  sub_key [512];
    char  value [512];
    DWORD value_size = sizeof(value);
    char  bitness[3] = { '\0' };

    rc = RegEnumKeyEx (key, num++, value, &value_size, NULL, NULL, NULL, NULL);
    if (rc == ERROR_NO_MORE_ITEMS)
       break;

    snprintf (sub_key, sizeof(sub_key), "%s\\%s", key_name, value);
    DEBUGF (2, " rec_level %d, num %lu, value: '%s'\n"
               "                     sub_key: '%s'\n", rec_level, num-1, value, sub_key);

    if (sscanf(value,"%d.%d-%2s", &pi.ver_major, &pi.ver_minor, bitness) >= 2)
    {
      if (bitness[0])
           pi.bitness = atoi (bitness);
      else pi.bitness = 32;
      DEBUGF (2, " ver %d.%d, bitness %d\n", pi.ver_major, pi.ver_major, pi.bitness);
    }
    else if (!stricmp(value,"InstallPath"))
            get_install_path (sub_key, &pi);

    rec_level++;
    enum_python_install_paths (sub_key);
    rec_level--;
  }

  if (key)
     RegCloseKey (key);
}

int py_init (void)
{
  static HANDLE ex_hnd = NULL;
  int    i, max;

  if (ex_hnd == NULL)
  {
    ex_hnd = LoadLibrary ("exc-abort.dll");
    DEBUGF (2, "LoadLibrary (\"exc-abort.dll\"): hnd: %p\n", ex_hnd);
  }

  py_programs = smartlist_new();

  get_python_exe_names();

/* enum_python_install_paths ("Software\\Python\\PythonCore");
 */

  /* \todo: compare 'exe_name' against 'py_programs' smartlist */

  DEBUGF (1, "py_which: %d/%s\n\n", py_which, py_variant_name(py_which));

  max = smartlist_len (py_programs);
  for (i = 0; i < max; i++)
  {
    const struct python_info *pi = smartlist_get (py_programs, i);
    int   len, indent = 1 + strlen (__FILE());
    char  version [20];

    snprintf (version, sizeof(version), "(%d.%d.%d)", pi->ver_major, pi->ver_minor, pi->ver_micro);
    len = (int) strlen (version);
    if (len > longest_py_version)
       longest_py_version = len;

    len = (int) strlen (pi->program);
    if (len > longest_py_program)
       longest_py_program = len;

    DEBUGF (1, "%u: %-*s -> \"%s\".  ver: %s\n"
               "%*sDLL:         -> \"%s\"\n"
               "%*sVariant:     -> %s%s\n"
               "%*sBitness:     -> %d\n",
            i, 2+longest_py_program, pi->program, pi->exe_name, version,
            indent+longest_py_program, "", pi->dll_name,
            indent+longest_py_program, "", py_variant_name(pi->variant),
            pi->is_default ? " (Default)" : "",
            indent+longest_py_program, "", pi->bitness);
  }
  return (max);
}
