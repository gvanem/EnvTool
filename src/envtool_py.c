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
#include "envtool.h"
#include "envtool_py.h"

/*
 * No need to include <Python.h> just for this:
 */
#define PyObject      void
#define PyThreadState void
#define Py_ssize_t    long

enum python_variants which_python = DEFAULT_PYTHON;

struct python_info {

  /* The basename of the specific Python interpreter.
   */
  const char *program;

  /* Which variant is this?
   */
  enum python_variants variant;

  /* Only CPython program can be embeddable from a C-program.
   */
  BOOL is_embeddable;

  /* The list of expected .DLLs for this specific Python.
   * Tested for existance in either %SystemDir% and/or directory
   * of 'exe_name'
   */
  const char *libraries [4];

  /* The FQFN of 'program'.
   */
  char exe_name [_MAX_PATH];

  /* The FQFN of the .dll that matches the first 'libraries[]' format above.
   * If this Python 'is_embeddable', use this 'dll_name' in 'LoadLibrary()'
   * during 'init_python_embedding()'.
   */
  char dll_name [_MAX_PATH];

  /* The 'sys.path[]' array of 'program'.
   * \todo: Use realloc()?
   */
  struct python_array sys_path [500];

  /* The version info.
   */
  int ver_major, ver_minor, ver_micro;

  /* This is the only default; i.e. the first 'program' in the $PATH.
   */
  BOOL is_default;

  /* It's position in the $PATH.
   */
  int path_pos;

  /* It's 'sys.prefix' used in Py_SetPythonHome().
   */
  void *home;

  PyObject *catcher;
  HANDLE    pipe_hnd, dll_hnd;
};

/*
 * List of all Pythons we care about. Ignore the console-less 'pythonw.exe'
 * programs.
 */
static struct python_info all_py_programs[] = {
    /* CPython */
    { "python.exe",  PY2or3_PYTHON, TRUE,  { "~\\libpython%d.%d.dll", "%s\\python%d%d.dll", NULL }, },
    { "python2.exe", PY2_PYTHON,    TRUE,  { "~\\libpython%d.%d.dll", "%s\\python%d%d.dll", NULL }, },
    { "python3.exe", PY3_PYTHON,    TRUE,  { "~\\libpython%d.%d.dll", "%s\\python%d%d.dll", NULL }, },

#if defined(__CYGWIN__)
    { "python2.7.exe", PY2_PYTHON,  TRUE,  { "~\\libpython%d.%d.dll", "%s\\python%d%d.dll", NULL }, },
    { "python3.5.exe", PY3_PYTHON,  TRUE,  { "~\\libpython%d.%d.dll", "%s\\python%d%d.dll", NULL }, },
#endif

    /* PyPy */
    { "pypy.exe",    PYPY_PYTHON,   FALSE, { "~\\libpypy-c.dll", NULL }, },

#if 0
    /* IronPython */
    { "ipy.exe",     IRON2_PYTHON,  FALSE, { "~\\IronPython.dll", NULL }, },
    { "ipy3.exe",    IRON3_PYTHON,  FALSE, { "~\\IronPython.dll", NULL }, },
    { "ipy64.exe",   IRON2_PYTHON,  FALSE, { "~\\IronPython.dll", NULL }, },
    { "ipy3_64.exe", IRON3_PYTHON,  FALSE, { "~\\IronPython.dll", NULL }, },
#endif

    /* JavaPython */
    { "jython.exe",  JYTHON_PYTHON, FALSE, { "~\\jpython.dll", NULL }, }
  };

static struct python_info *g_py;  /* The global Python instance data. */

#define PY_THREAD_SAVE  0
#define PY_GET_VERSION  "import sys; print (sys.version[:5])"
#define PY_PIPE_NAME    "\\\\.\\pipe\\EnvTool"

#define LOAD_FUNC(is_opt, f)  do {                                                 \
                                f = (func_##f) GetProcAddress (g_py->dll_hnd, #f); \
                                if (!f && !is_opt) {                               \
                                  WARN ("Failed to find \"%s()\" in %s.\n",        \
                                        #f, g_py->dll_name);                       \
                                  goto failed;                                     \
                                }                                                  \
                                DEBUGF (3, "Function %s(): %*s 0x%p\n",            \
                                           #f, 23-(int)strlen(#f), "", f);         \
                              } while (0)

#define DEF_FUNC(ret,f,args)  typedef ret (__cdecl *func_##f) args; \
                              static func_##f f

/*
 * We only need 1 func-ptr for each embeddable Python program function since
 * we can only embed 1 Python at a time.
 */
DEF_FUNC (void,             Py_InitializeEx,        (int init_sigs));
DEF_FUNC (void,             Py_Finalize,            (void));
DEF_FUNC (void,             Py_SetProgramName,      (char *name));
DEF_FUNC (void,             Py_SetPythonHome,       (void *home));
DEF_FUNC (int,              PyRun_SimpleString,     (const char *cmd));
DEF_FUNC (PyObject*,        PyImport_AddModule,     (const char *name));
DEF_FUNC (PyObject*,        PyObject_GetAttrString, (PyObject *o, char *attr));
DEF_FUNC (char *,           PyString_AsString,      (PyObject *o));    /* Python 2.x */
DEF_FUNC (char *,           PyBytes_AsString,       (PyObject *o));    /* Python 3.x */
DEF_FUNC (Py_ssize_t,       PyString_Size,          (PyObject *o));    /* Python 2.x */
DEF_FUNC (Py_ssize_t,       PyBytes_Size,           (PyObject *o));    /* Python 3.x */
DEF_FUNC (void,             PyObject_Free,          (PyObject *obj));
DEF_FUNC (void,             Py_DecRef,              (PyObject *obj));
DEF_FUNC (PyObject*,        PyObject_CallMethod,    (PyObject *o, char *method, char *fmt, ...));

#if PY_THREAD_SAVE
  DEF_FUNC (void,           PyEval_InitThreads,     (void));
  DEF_FUNC (PyThreadState*, PyEval_SaveThread,      (void));
  DEF_FUNC (void,           PyEval_RestoreThread,   (PyThreadState *));
#endif

static const char *python_variant_name (enum python_variants v)
{
  return (v == DEFAULT_PYTHON ? "Default"     :
          v == PY2_PYTHON     ? "Python2"     :
          v == PY3_PYTHON     ? "Python3"     :
          v == PY2or3_PYTHON  ? "Python2or3"  :
          v == IRON2_PYTHON   ? "IronPython2" :
          v == IRON3_PYTHON   ? "IronPython3" :
          v == PYPY_PYTHON    ? "PyPy"        :
          v == JYTHON_PYTHON  ? "JPython"     :
          v == ALL_PYTHONS    ? "All"         : "??");
}

static int compare_strings (const void *_s1, const void *_s2)
{
  const char *s1 = *(const char**) _s1;
  const char *s2 = *(const char**) _s2;
  return stricmp (s1, s2);
}

const char **python_get_variants (void)
{
  static char *result [DIM(all_py_programs)+1];
  const struct python_info *py;
  int   i, j;

  for (i = j = 0, py = all_py_programs; i < DIM(all_py_programs); i++, py = all_py_programs+i)
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
      case PY2or3_PYTHON:
           result [j++] = "py";
           break;
      default:
           FATAL ("What?");
           break;
    }
  }

  DEBUGF (3, "j: %d\n", j);
  for (i = 0; i < DIM(result); i++)
     DEBUGF (3, "python_get_variants(); result[%d] = %s\n", i, result[i]);

  qsort (&result[0], j, sizeof(char*), compare_strings);

  /* Make a unique result list.
   */
  for (i = 0; i < j; i++)
    if (i > 0 && result[i] && !strcmp(result[i],result[i-1]))
       memmove (&result[i-1], &result[i], (DIM(result) - i) * sizeof(const char*));

  DEBUGF (3, "\n");
  for (i = 0; i < DIM(result); i++)
     DEBUGF (3, "python_get_variants(); result[%d] = %s\n", i, result[i]);

  result [j] = NULL;
  return (const char**) result;
}

/*
 * Select a Python that is found on PATH and that we've found the DLL for
 * and that is of a suitable variant.
 */
static struct python_info *select_python (enum python_variants which)
{
  struct python_info *py;
  int    i;

  for (i = 0, py = all_py_programs; i < DIM(all_py_programs); i++, py = all_py_programs+i)
  {
    if (!py->exe_name[0] || !py->dll_name[0])
       continue;

    if ((which == DEFAULT_PYTHON && py->is_default) || (which == py->variant))
    {
      DEBUGF (1, "select_python (%d); \"%s\" -> \"%s\"\n", which, python_variant_name(py->variant), py->exe_name);
      return (py);
    }
  }
  DEBUGF (1, "select_python (%d); \"%s\" not possible.\n", which, python_variant_name(which));
  return (NULL);
}

int get_python_info (const char **exe, const char **dll, int *major, int *minor, int *micro)
{
  struct python_info *py = select_python (DEFAULT_PYTHON);

  if (!py)
     return (0);

  if (exe)
     *exe = py->exe_name;
  if (dll)
     *dll = py->dll_name;
  if (major)
     *major = py->ver_major;
  if (minor)
     *minor = py->ver_minor;
  if (micro)
     *micro = py->ver_micro;
  return (1);
}

static void free_sys_path (struct python_info *py)
{
  struct python_array *pa;
  int    i;

  for (i = 0, pa = &py->sys_path[0]; pa->dir; pa = &py->sys_path[++i])
      FREE (pa->dir);
}

/*
 * This should be the same as 'sys.prefix'.
 * The allocated string (ASCII or Unicode) is freed in 'exit_python()'.
 *
 * It seems CygWin64's Python fail to find the site.py file w/o setting
 * the PYTONPATH explicitly!
 */
static void get_python_home (struct python_info *py)
{
#if defined(__CYGWIN__) && 0
  char home[_MAX_PATH];

  snprintf (home, sizeof(home), "/usr/lib/python%d.%d", py->ver_major, py->ver_minor);
  py->home = (void*) strdup (home);

  snprintf (home, sizeof(home), "PYTHONPATH=%s", (char*)py->home);
  putenv (home);

#elif defined(__CYGWIN__)
  py->home = (void*) strdup ("/usr/lib");

#else
  char *dir = dirname (py->exe_name);

  if (py->ver_major >= 3)
  {
    wchar_t buf [_MAX_PATH];

    buf[0] = L'\0';
    MultiByteToWideChar (CP_ACP, 0, dir, -1, buf, DIM(buf));
    py->home = (void*) _wcsdup (buf);
  }
  else
  {
    /* Reallocate again because free() is used in exit_python()!
     */
    py->home = (void*) strdup (dir);
  }
  FREE (dir);
#endif
}

static const char *get_prog_name_ascii (void)
{
  static char prog_name [_MAX_PATH] = "?";
  static char cyg_name [_MAX_PATH];

  GetModuleFileNameA (NULL, prog_name, DIM(prog_name));

#if defined(__CYGWIN__)
  if (cygwin_conv_path(CCP_WIN_A_TO_POSIX, prog_name, cyg_name, sizeof(cyg_name)) == 0)
     return (cyg_name);
#endif
  ARGSUSED (cyg_name);
  return (prog_name);
}

static const wchar_t *get_prog_name_wchar (void)
{
  static wchar_t prog_name [_MAX_PATH] = L"?";
  static wchar_t cyg_name [_MAX_PATH];

  GetModuleFileNameW (NULL, prog_name, DIM(prog_name));

#if defined(__CYGWIN__)
  if (cygwin_conv_path(CCP_WIN_W_TO_POSIX, prog_name, cyg_name, DIM(cyg_name)) == 0)
     return (cyg_name);
#endif
  ARGSUSED (cyg_name);
  return (prog_name);
}

static void exit_python_embedding (void)
{
  if (g_py->pipe_hnd && g_py->pipe_hnd != INVALID_HANDLE_VALUE)
     CloseHandle (g_py->pipe_hnd);

  if (g_py->dll_hnd && g_py->dll_hnd != INVALID_HANDLE_VALUE)
  {
    if (Py_Finalize)
    {
      DEBUGF (4, "Calling Py_Finalize().\n");
      (*Py_Finalize)();
    }
    CloseHandle (g_py->dll_hnd);
  }

  g_py->pipe_hnd = g_py->dll_hnd = INVALID_HANDLE_VALUE;
  g_py->home = NULL;
  g_py = NULL;
}

void exit_python (void)
{
  struct python_info *py;
  int    i;

  if (g_py)
  {
    if (!g_py->is_embeddable)
       free_sys_path (g_py);
    exit_python_embedding();
  }

  for (i = 0, py = all_py_programs; i < DIM(all_py_programs); py = &all_py_programs[++i])
  {
    if (py->home)
       free (py->home);  /* Do not use FREE() since it could have been allocated using _wcsdup(). */
  }
}

/*
 * Setup a class-instance for catching all output written
 * using 'sys.stdout'. I.e. 'print' and 'os.write(1, ...)'.
 * This instance must reside at the global '__main__' level.
 *
 * Thus the Python printed strings are retrieved in the C-world by
 * 'catcher.value'. I.e:
 *   obj = (*PyObject_GetAttrString) (py_catcher, "value");
 *
 * Ref:
 *   http://stackoverflow.com/questions/4307187/how-to-catch-python-stdout-in-c-code
 */
static PyObject *setup_stdout_catcher (void)
{
  static char code[] = "import sys\n"               \
                       "class catch_stdout:\n"      \
                       "  def __init__(self):\n"    \
                       "    self.value = ''\n"      \
                       "  def write(self, txt):\n"  \
                       "    self.value += txt\n"    \
                       "  def reset(self):\n"       \
                       "    self.value = ''\n"      \
                       "old_stdout = sys.stdout\n"  \
                       "sys.stdout = catcher = catch_stdout()\n";

  PyObject *mod = (*PyImport_AddModule) ("__main__");          /* create main module */
  int       rc  = (*PyRun_SimpleString) (code);                /* invoke code to redirect */
  PyObject *obj = (*PyObject_GetAttrString) (mod, "catcher");  /* get our catcher created above */

  DEBUGF (4, "mod: %p, rc: %d, obj: %p\n", mod, rc, obj);
  return (obj);
}

static int init_python_embedding (void)
{
  char *exe, *dll;

  g_py = select_python (which_python);
  if (!g_py)
     return (0);

  if (!g_py->is_embeddable)
  {
    WARN ("%s is not embeddable.\n", g_py->exe_name);
    return (0);
  }

  exe = g_py->exe_name;
  dll = g_py->dll_name;
  if (!exe || !dll)
  {
    WARN ("Failed to find any Python DLLs.\n");
    return (0);
  }

  g_py->dll_hnd = LoadLibrary (dll);
  if (!g_py->dll_hnd || g_py->dll_hnd == INVALID_HANDLE_VALUE)
  {
    WARN ("Failed to load %s; %s\n", dll, win_strerror(GetLastError()));
    return (0);
  }

  DEBUGF (2, "Full DLL name: \"%s\". Handle: 0x%p\n", g_py->dll_name, g_py->dll_hnd);

  LOAD_FUNC (0, Py_InitializeEx);
  LOAD_FUNC (0, Py_Finalize);
  LOAD_FUNC (0, Py_SetProgramName);
  LOAD_FUNC (0, Py_SetPythonHome);
  LOAD_FUNC (0, PyRun_SimpleString);
#if PY_THREAD_SAVE
  LOAD_FUNC (0, PyEval_InitThreads);
  LOAD_FUNC (0, PyEval_SaveThread);
  LOAD_FUNC (0, PyEval_RestoreThread);
#endif
  LOAD_FUNC (0, PyObject_GetAttrString);
  LOAD_FUNC (0, PyImport_AddModule);
  LOAD_FUNC (1, PyString_AsString);
  LOAD_FUNC (1, PyBytes_AsString);
  LOAD_FUNC (1, PyString_Size);
  LOAD_FUNC (1, PyBytes_Size);
  LOAD_FUNC (0, PyObject_CallMethod);
  LOAD_FUNC (0, PyObject_Free);
  LOAD_FUNC (0, Py_DecRef);

  if (g_py->ver_major >= 3)
  {
    const wchar_t *name = get_prog_name_wchar();

    PyString_AsString = PyBytes_AsString;
    PyString_Size     = PyBytes_Size;

    DEBUGF (2, "Py_SetProgramName (\"%S\")\n", name);
    (*Py_SetProgramName) ((char*)name);

    DEBUGF (2, "Py_SetPythonHome (\"%S\")\n", (wchar_t*)g_py->home);
    (*Py_SetPythonHome) (g_py->home);
  }
  else
  {
    const char *name = get_prog_name_ascii();

    DEBUGF (2, "Py_SetProgramName (\"%s\")\n", name);
    (*Py_SetProgramName) ((char*)name);

    DEBUGF (2, "Py_SetPythonHome (\"%s\")\n", (char*)g_py->home);
    (*Py_SetPythonHome) (g_py->home);
  }

  (*Py_InitializeEx) (0);
  DEBUGF (3, "Py_InitializeEx (0) passed\n");

#if PY_THREAD_SAVE
  (*PyEval_InitThreads)();
#endif

  g_py->catcher = setup_stdout_catcher();
  if (g_py->catcher)
     return (1);

  /* Fall through */

failed:
  exit_python_embedding();
  return (0);
}

/*
 * Call the Python code in 'py_prog'. The returned memory (!= NULL)
 * MUST be freed using FREE().
 */
static char *call_python_func (const char *py_prog)
{
  PyObject  *obj;
  Py_ssize_t size;
  char      *str = NULL;
  int        rc;

#if PY_THREAD_SAVE
  PyThreadState *ts = (*PyEval_SaveThread)();

  DEBUGF (4, "PyEval_SaveThread(): %p\n", ts);
#endif

  DEBUGF (3, "py_prog:\n"
             "----------------------\n%s\n"
             "----------------------\n", py_prog);

  rc  = (*PyRun_SimpleString) (py_prog);
  obj = (*PyObject_GetAttrString) (g_py->catcher, "value");

  DEBUGF (4, "rc: %d, obj: %p\n", rc, obj);

  if (rc == 0 && obj)
  {
    size = (*PyString_Size) (obj);
    if (size > 0)
       str = STRDUP ((*PyString_AsString)(obj));

    /* Reset the 'g_py->catcher' buffer value to prepare for next call
     * to this 'call_python_func()'.
     */
    (*PyObject_CallMethod) (g_py->catcher, "reset", NULL);
    DEBUGF (4, "PyString_Size(): %ld\n", size);
  }

#if PY_THREAD_SAVE
  (*PyEval_RestoreThread) (ts);
#endif

  return (str);
}

void test_python_funcs (void)
{
  const char *prog = "import sys, os\n"
                     "print (sys.version[0:5] + \"\\n\")\n"
                     "for i in range(10):\n"
                     "  print (\"  Hello world\")\n";
  char *str;

  if (!init_python_embedding())
     return;

  assert (g_py->is_embeddable);

  str = call_python_func (prog);
  printf ("Captured output of Python:\n** %s **\n", str);
  FREE (str);

  printf ("The rest should not be captured:\n");

  /* This should return no output.
   */
  str = call_python_func ("sys.stdout = old_stdout\n");
  FREE (str);

  str = call_python_func (prog);
  printf ("Captured output of Python now:\n** %s **\n", str);
  FREE (str);
}

/*
 * Add the 'dir' to 'g_py->sys_path[]' at index 'i'.
 * This assumes the python program in 'PY_PRINT_SYS_PATH' only returns
 * correct 'sys.path[]' components.
 *
 * Note: the 'dir' is allocated memory from 'call_python_func()' which
 *       is freed at end of 'do_check_python()'.
 */
static int add_sys_path (char *dir, int index)
{
  struct python_array *pa = g_py->sys_path + index;
  struct stat st;
  int    j;

  if (index >= DIM(g_py->sys_path))
  {
    WARN ("Too many paths in 'sys.path[]'. Max %d\n", index);
    return (0);
  }

  memset (&st, '\0', sizeof(st));
  pa->dir    = dir;
  pa->exist  = (stat(dir, &st) == 0);
  pa->is_dir = pa->exist && _S_ISDIR(st.st_mode);
  pa->is_zip = pa->exist && _S_ISREG(st.st_mode) && check_if_zip (dir);

  for (j = 0; j < index; j++)
      if (!stricmp(dir,g_py->sys_path[j].dir))
         pa->num_dup++;

  return (1);
}

/**
 * Parse the output line from the 'PY_ZIP_LIST' program below.
 * Each line on the form:
 *   81053 20130327.164158 stem/control.py
 *   ^     ^
 *   size  time: YYYYMMDD.HHMMSS
 */
static int report_zip_file (const char *zip_file, char *output)
{
  static char *py_home = NULL;
  static char *sys_prefix = "$PYTHONHOME";
  static BOOL  do_warn = TRUE;
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

  if (!py_home && do_warn)
  {
    py_home = getenv ("PYTHONHOME");
    if (!py_home)
       py_home = g_py->home;
    if (!FILE_EXISTS(py_home))
       WARN ("%s points to non-existing directory: \"%s\".\n", sys_prefix, py_home);
    do_warn = FALSE;
  }

  len = snprintf (report, sizeof(report), "%s  (", file_within_zip);

  /* Figure out if and where 'py_home' and 'zip_file' overlaps.
   */
  p = py_home ? path_ltrim (zip_file, py_home) : zip_file;

  if (IS_SLASH(*p))  /* if 'py_home' doesn't end in a slash. */
     p++;

  DEBUGF (1, "p: '%s', py_home: '%s', zip_file: '%s'\n", p, py_home, zip_file);

  if (p != zip_file && !strnicmp(zip_file,py_home,strlen(py_home)))
       snprintf (report+len, sizeof(report)-len, "%s\\%s)", sys_prefix, p);
  else snprintf (report+len, sizeof(report)-len, "%s)", zip_file);

  /* zipinfo always reports 'file_within_zip' with '/' slashes. But simply slashify the
   * complete 'report' to use either '\\' or '/'.
   */
  _strlcpy (report, slashify(report, opt.show_unix_paths ? '/' : '\\'), sizeof(report));

  /* \todo: incase '--pe-check' is specified and 'report' file is a .pyd-file,
   *        we should save the .pyd to a %TMP-file and examine it in report_file().
   */
  report_file (report, mtime, fsize, FALSE, HKEY_PYTHON_EGG);
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
 */

/*
 * This goes into a buffer used in call_python_func().
 */
#define PY_ZIP_LIST                                                                   \
          "import os, fnmatch, zipfile\n"                                             \
          "def print_zline (f, debug):\n"                                             \
          "  base = os.path.basename (f.filename)\n"                                  \
          "  if debug >= 3:\n"                                                        \
          "     os.write (2, 'egg-file: %%s, base: %%s\\n' %% (f.filename, base))\n"  \
          "  if fnmatch.fnmatch (f.filename, '%s'):\n"   /* opt.file_spec */          \
          "    date = \"%%4d%%02d%%02d\"  %% (f.date_time[0:3])\n"                    \
          "    time = \"%%02d%%02d%%02d\" %% (f.date_time[3:6])\n"                    \
          "    str = \"%%d %%s.%%s %%s\"  %% (f.file_size, date, time, f.filename)\n" \
          "    if debug:\n"                                                           \
          "       os.write (2,'str: \"%%s\"\\n' %% str)\n"                            \
          "    print (str)\n"                                                         \
          "\n"                                                                        \
          "zf = zipfile.ZipFile (r\"%s\", 'r')\n"        /* zfile */                  \
          "for f in zf.infolist():\n"                                                 \
          "  print_zline (f, %d)\n"                      /* opt.debug */

static int process_zip (const char *zfile)
{
  char  cmd [_MAX_PATH + 1000];
  char *line, *str;
  int   len, found = 0;

  if (sizeof(cmd) < sizeof(PY_ZIP_LIST) + _MAX_PATH + 100)
     FATAL ("cmd[] buffer too small.\n");

  len = snprintf (cmd, sizeof(cmd), PY_ZIP_LIST, opt.file_spec, zfile, opt.debug);
  str = call_python_func (cmd);
  DEBUGF (2, "cmd-len: %d, Python output: \"%s\"\n", len, str);

  if (str)
  {
    for (found = 0, line = strtok(str,"\n"); line; line = strtok(NULL,"\n"), found++)
    {
      DEBUGF (2, "line: \"%s\", found: %d\n", line, found);
      if (!report_zip_file(zfile, line))
         break;
    }
    FREE (str);
  }

  if (found == 0)
     DEBUGF (1, "No matches in %s for %s.\n", zfile, opt.file_spec);

  return (found);
}

/*
 * Run python, figure out the 'sys.path[]' array and search along that
 * for matches. If a 'sys.path[]' component contains a ZIP/EGG-file, use
 * 'process_zip()' to list files inside it for a match.
 *
 * Note:
 *   not all .egg-files are ZIP-files. 'check_if_zip()' is used to test
 *   that and set 'py->is_zip' accordingly.
 */

/* This is for the 'call_python_func()' variant which is used when
 * 'py->is_embeddable' is TRUE.
 */
#define PY_PRINT_SYS_PATH   "import sys\n" \
                            "for (i,p) in enumerate(sys.path):\n" \
                            "  print ('%s\\n' % p)\n"

/* For the cmd-line versions (Python 2+3) of the above Python program
 * when 'py->is_embeddable' is FALSE.
 */
#define PY_PRINT_SYS_PATH2  "import os, sys; " \
                            "[os.write(1,'%s\\n' % p) for (i,p) in enumerate(sys.path)]"

#define PY_PRINT_SYS_PATH3  "import sys; " \
                            "[print(p) for (i,p) in enumerate(sys.path)]"

static int build_sys_path (char *str, int line)
{
  char *l = strtok (str, "\n");
  int   index;

  ASSERT (line == -1);

  for (index = 0; l; index++)
  {
    DEBUGF (2, "l (index: %d, line: %2d): \"%s\"\n", index, line, l);
    if (!add_sys_path(l, index))
       break;
    l = strtok (NULL, "\n");
  }
  return (0);
}

static int build_sys_path2 (char *str, int index)
{
  DEBUGF (2, "str (index: %d): \"%s\"\n", index, str);
  ASSERT (index >= 0);

  if (!add_sys_path(STRDUP(str), index))
     return (-1);
  return (0);
}

static void print_sys_path (const struct python_info *py)
{
  char cmd [_MAX_PATH + max(sizeof(PY_PRINT_SYS_PATH2),sizeof(PY_PRINT_SYS_PATH3)) + 10];

  if (py->ver_major >= 3)
       snprintf (cmd, sizeof(cmd), "%s -c \"%s\"", py->exe_name, PY_PRINT_SYS_PATH3);
  else snprintf (cmd, sizeof(cmd), "%s -c \"%s\"", py->exe_name, PY_PRINT_SYS_PATH2);
  popen_run (cmd, build_sys_path2);
}

int do_check_python (void)
{
  struct python_array *pa;
  char                *str = NULL;
  int                  found = 0;

  g_py = select_python (which_python);
  if (!g_py)
  {
    WARN ("%s was not found on PATH.\n", python_variant_name(which_python));
    return (0);
  }

  if (g_py->is_embeddable)
  {
    if (!init_python_embedding())
       return (0);

    str = call_python_func (PY_PRINT_SYS_PATH);
    if (!str)
       return (0);
    build_sys_path (str, -1);
  }
  else
    print_sys_path (g_py);

  for (found = 0, pa = g_py->sys_path; pa && pa->dir; pa++)
  {
    /* Don't warn on missing .zip files in 'sys.path[]' (unless in debug-mode)
     */
    if (!opt.debug && !pa->exist && !stricmp(get_file_ext(pa->dir), "zip"))
       pa->exist = pa->is_dir = TRUE;

    if (pa->is_zip)
         found += process_zip (pa->dir);
    else found += process_dir (pa->dir, pa->num_dup, pa->exist, pa->is_dir,
                               TRUE, "sys.path[]", NULL);
  }

  if (g_py->is_embeddable)
       FREE (str);
  else free_sys_path (g_py);

  return (found);
}

#if !defined(__CYGWIN__)

#define PY_PIPE_WRITE_PROG                                                                   \
        "import time, win32pipe, win32file\n"                                                \
        "handle = win32file.CreateFile (r'%s',\n"                                            \
        "                               win32file.GENERIC_READ | win32file.GENERIC_WRITE,\n" \
        "                               0, None,\n"                                          \
        "                               win32file.OPEN_EXISTING,\n"                          \
        "                               0, None)\n"                                          \
        "for (i,p) in enumerate(sys.path):\n"                                                \
        "  time.sleep (1)\n"                                                                 \
        "  win32file.WriteFile (handle, '%%s\\n' %% p)\n"

static int fgets_loop (FILE *f, popen_callback callback)
{
  char  buf[256];
  int   i = 0;
  int   j = 0;
  int   ch;

  DEBUGF (2, "_fileno(f): %d\n", _fileno(f));

#if 1
  while ((ch = fgetc(f)) != -1)
#else
  while (fgets(buf,sizeof(buf),f))
#endif
  {
#if 1
    DEBUGF (2, " fgetc(): '%c'\n", ch);
    j++;
#else
    int rc;

    strip_nl (buf);
    DEBUGF (2, " fgets(): '%s'\n", buf);
    if (!buf[0])
       continue;
    rc = (*callback) (buf, i++);
    if (rc < 0)
       break;
    j += rc;
#endif
  }
  ARGSUSED (i);
  ARGSUSED (buf);
  return (j);
}
#endif

/*
 * Doesn't work as single-threaded :-(
 * Maybe run the Python pipe-test in a separate thread?
 * Enable this test using 'envtool.exe --python --test'.
 */
int test_python_pipe (void)
{
  int rc = 0;

#if !defined(__CYGWIN__)
  int   fd = -1;
  char  py_cmd [500];
  FILE *f  = NULL;

  if (!init_python_embedding())
     return (0);

  /* Under Windows, named pipes MUST have the form
   * "\\<server>\pipe\<pipename>". <server> may be "." for localhost.
   */
  g_py->pipe_hnd = CreateNamedPipe (PY_PIPE_NAME,
                                    PIPE_ACCESS_DUPLEX,  /* read/write pipe */
                                    PIPE_TYPE_BYTE | PIPE_NOWAIT,
                                    1, 4*1024, 4*1024, 0, NULL);
  if (g_py->pipe_hnd == INVALID_HANDLE_VALUE)
  {
    WARN ("CreateNamedPipe (\"%s\") failed; %s\n", PY_PIPE_NAME,
          win_strerror(GetLastError()));
    return (0);
  }

  fd = _open_osfhandle ((intptr_t)g_py->pipe_hnd, _O_BINARY /* _O_TEXT */);
  if (fd < 0)
  {
    DEBUGF (1, "_open_osfhandle() failed: %s.\n", strerror(errno));
    return (0);
  }

  f = fdopen (fd, "r");
  if (!f)
  {
    DEBUGF (1, "fdopen() failed: %s.\n", strerror(errno));
    return (0);
  }

  DEBUGF (3, "pipe_hnd: 0x%p, fd: %d.\n", (void*)g_py->pipe_hnd, fd);

  snprintf (py_cmd, sizeof(py_cmd), PY_PIPE_WRITE_PROG, PY_PIPE_NAME);
  DEBUGF (2, "py_cmd:\n----------------------\n%s\n----------------------\n",
          py_cmd);

  rc = (*PyRun_SimpleString) (py_cmd);
  if (rc == 0)
     rc = fgets_loop (f, add_sys_path);
  fclose (f);
#endif  /* __CYGWIN__ */

  return (rc);
}

static int report_py_version_cb (char *output, int line)
{
  int num = sscanf (output, "%d.%d.%d", &g_py->ver_major, &g_py->ver_minor, &g_py->ver_micro);

  DEBUGF (1, "Python ver: %d.%d.%d\n", g_py->ver_major, g_py->ver_minor, g_py->ver_micro);
  ARGSUSED (line);
  return  (num >= 2);
}

/*
 * \todo: find the newest unique 'py->program' on PATH.
 */
static char *get_python_exe_name (struct python_info *py)
{
  const char *prog = searchpath (py->program, "PATH");

  if (!prog || !FILE_EXISTS(prog))
     return (NULL);

  _fix_path (prog, py->exe_name);
  py->path_pos = searchpath_pos();
  DEBUGF (1, "\n");
  DEBUGF (1, "python_exe_name: %s, path_pos: %d\n", py->exe_name, py->path_pos);
  return (py->exe_name);
}

/*
 * \todo:
 *   If multiple DLLs with same name but different time-stamps are found
 *   (in 'dirname(py->exe_name)' and 'sys_dir'), report a warning.
 *   Check PE-version and/or MD5 finger-print?
 */
static int get_python_dll_name (struct python_info *py)
{
  const char *lib_fmt, **libs = py->libraries;
  char       *path = dirname (py->exe_name);
  char        dll1 [_MAX_PATH] = { '\0' };
  char        dll2 [_MAX_PATH] = { '\0' };
  char       *use_this = NULL;
  const char *newest = NULL;
  struct stat st1, st2, *use_st = NULL;
  BOOL       _st1, _st2, equal;
  size_t      i, num = DIM (py->libraries);

  _st1 = _st2 = FALSE;

  for (i = 0, lib_fmt = libs[0]; lib_fmt && i < num; lib_fmt = libs[++i])
  {
    if (!strncmp(lib_fmt, "%s\\",3))
    {
      snprintf (dll1, sizeof(dll1), lib_fmt, path, py->ver_major, py->ver_minor);
      snprintf (dll2, sizeof(dll2), lib_fmt, sys_dir, py->ver_major, py->ver_minor);
    }
    else if (!strncmp(lib_fmt,"~\\",2))
    {
      strcpy (dll1, path);
      snprintf (dll1+strlen(dll1), sizeof(dll1)-strlen(dll1), lib_fmt+1, py->ver_major, py->ver_minor);
      dll2[0] = '\0';
    }
    else
    {
      snprintf (dll1, sizeof(dll1), lib_fmt, py->ver_major, py->ver_minor);
      dll2[0] = '\0';
    }

    DEBUGF (1, "checking for:\n  dll1: \"%s\"\n  dll2: \"%s\"\n", dll1, dll2);

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
      newest = dll1;  /* The one in 'path' */
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
    _fix_path (newest, py->dll_name);
    DEBUGF (1, "Found newest DLL: \"%s\", \"%s\"\n", newest, get_time_str(use_st->st_mtime));
  }

  FREE (path);
  return (newest != NULL);
}

/*
 * This must be called after 'init_python()' has been called.
 */
int test_pythons (void)
{
  struct python_info *py;
  int    i, found = 0;

  for (i = 0, py = all_py_programs; i < DIM(all_py_programs); py = &all_py_programs[++i])
  {
    BOOL test_this = ( (py->variant == which_python) || (which_python == ALL_PYTHONS) ||
                       (which_python == DEFAULT_PYTHON && py->is_default) ) &&
                     py->exe_name[0];

    DEBUGF (0, "Will%s try to test: %s%s: %s\n",
            test_this ? "" : " not",
            python_variant_name(py->variant), py->is_default ? " (Default) " : "",
            py->exe_name[0] ? py->exe_name : "Not found");

    if (test_this)
    {
      g_py = py;
      print_sys_path (py);
      free_sys_path (py);
      found++;
    }
  }
  g_py = NULL;
  return (found);
}

static int get_python_version (struct python_info *py)
{
  char cmd [_MAX_PATH + sizeof(PY_GET_VERSION) + 10];

  snprintf (cmd, sizeof(cmd), "%s -c \"%s\"", py->exe_name, PY_GET_VERSION);
  return (popen_run(cmd, report_py_version_cb) >= 1);
}

static size_t longest_py_program = 0;

void searchpath_pythons (void)
{
  const struct python_info *py;
  size_t       i;
  BOOL         has_default = FALSE;
  char         version [12];

  for (i = 0, py = all_py_programs; i < DIM(all_py_programs); py = &all_py_programs[++i])
  {
    char fname [_MAX_PATH];

    if (py->ver_major > -1 && py->ver_minor > -1 && py->ver_micro > -1)
         snprintf (version, sizeof(version), "(%d.%d.%d)", py->ver_major, py->ver_minor, py->ver_micro);
    else if (py->exe_name[0])
         _strlcpy (version, "(ver: ?)", sizeof(version));
    else version[0] = '\0';

    if (py->is_default)
       has_default = TRUE;

    _strlcpy (fname, slashify(py->exe_name, opt.show_unix_paths ? '/' : '\\'), sizeof(fname));

    C_printf ("   %s %s%*s %-7s -> ~%c%s~0\n",
              py->is_default ? "~3(1)~0" : "   ", py->program,
              (int)(longest_py_program - strlen(py->program)), "", version,
              fname[0] ? '6' : '5',
              fname[0] ? fname : "Not found");

    if (py->dll_name[0])
    {
      _strlcpy (fname, slashify(py->exe_name, opt.show_unix_paths ? '/' : '\\'), sizeof(fname));
      C_printf ("%*s%s (%sembeddable)\n",
                (int)(longest_py_program + 19), "",
                fname, py->is_embeddable ? "": "not ");
     }
  }
  if (has_default)
     C_puts ("   ~3(1)~0 Default Python (found first on PATH).\n");
}

int init_python (void)
{
  struct python_info *py, *default_py;
  size_t i, len;
  int    pos, num = 0;

  DEBUGF (1, "which_python: %d/%s\n", which_python, python_variant_name(which_python));

  for (i = 0, py = all_py_programs; i < DIM(all_py_programs); py = &all_py_programs[++i])
  {
    len = strlen (py->program);
    if (len > longest_py_program)
       longest_py_program = len;
  }

  for (i = 0, py = all_py_programs; i < DIM(all_py_programs); py = &all_py_programs[++i])
  {
    memset (&py->sys_path, 0, sizeof(py->sys_path));
    py->ver_major = py->ver_minor = py->ver_micro = py->path_pos = -1;
    py->catcher  = NULL;
    py->pipe_hnd = INVALID_HANDLE_VALUE;
    py->dll_hnd  = INVALID_HANDLE_VALUE;

    py->exe_name[0] = '\0';
    py->dll_name[0] = '\0';

    g_py = py;

    if (get_python_exe_name(py) && get_python_version(py) && get_python_dll_name(py))
    {
      get_python_home (g_py);
      num++;
    }
  }

  DEBUGF (1, "\n");

  pos = INT_MAX;

  /* Figure out which 'py->program' in list is found first on PATH.
   * That one is the default.
   */
  for (i = 0, py = all_py_programs, default_py = NULL; i < DIM(all_py_programs); py = &all_py_programs[++i])
      if (py->path_pos >= 0 && py->path_pos < pos)
      {
        pos = py->path_pos;
        default_py = py;
      }

  if (default_py)
     default_py->is_default = TRUE;

  for (i = 0, py = all_py_programs; i < DIM(all_py_programs); py = &all_py_programs[++i])
  {
    DEBUGF (1, "%d: \"%s\" %*s -> \"%s\".\n"
               "                    DLL:     %-8s -> \"%s\"\n"
               "                    Variant: %-8s -> %s %s\n",
            (int)i, py->program, (int)(longest_py_program - strlen(py->program)), "", py->exe_name,
            "", py->dll_name,
            "", python_variant_name(py->variant), py->is_default ? "(Default)" : "");
  }
  g_py = NULL;
  return (num);
}
