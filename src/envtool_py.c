/*
 * The Python functions for the envtool program.
 *
 * By Gisle Vanem <gvanem@yahoo.no> August 2011.
 *
 */

#include <stddef.h>
#include <fcntl.h>
#include <errno.h>

#include "color.h"
#include "envtool.h"
#include "envtool_py.h"

enum python_variants which_python = DEFAULT_PYTHON;

struct python_info {

  /* The basename of the specific Python interpreter.
   */
  const char *program;

  /* The list of formats for the expected .DLLs for this specific Python.
   */
  const char *libraries[4];

  /* The FQFN of 'program'.
   */
  char exe_name [_MAX_PATH];

  /* it's FQFN .dll that matches the first above 'libraries[]' format.
   * This dll_name is used in LoadLibrary() during init_python_embedding().
   */
  char dll_name [_MAX_PATH];

  /* The 'sys.path[]' array of 'program'.
   * \todo: Use realloc()?
   */
  struct python_array py_array [500];

  /* The version info.
   */
  int ver_major, ver_minor, ver_micro;
};

static struct python_info all_py_programs[] = {
   /* CPython */
   { "python.exe",  { "python%d%d.dll", "libpython%d.%d.dll", NULL }, },
   { "python2.exe", { "python%d%d.dll", "libpython%d.%d.dll", NULL }, },
   { "python3.exe", { "python%d%d.dll", "libpython%d.%d.dll", NULL }, },

   /* IronPython */
   { "ipy.exe",     { "IronPython.dll", NULL }, },
   { "ipy3.exe",    { "IronPython.dll", NULL }, },
   { "ipy64.exe",   { "IronPython.dll", NULL }, },
   { "ipy3_64.exe", { "IronPython.dll", NULL }, },

   /* PyPy */
   { "pypy.exe",    { "libpypy-c.dll", NULL }, },

   /* JavaPython */
   { "jython.exe",  { "jpython.dll", NULL }, }
 };

#define PY_THREAD_SAVE  0
#define PY_GET_VERSION  "import sys; print (sys.version[:5])"
#define PY_PIPE_NAME    "\\\\.\\pipe\\EnvTool"

/* No need to include <Python.h> just for this:
 */
#define PyObject      void
#define PyThreadState void
#define Py_ssize_t    long

static struct python_array py_array [500];

static int       py_major = -1;
static int       py_minor = -1;
static int       py_micro = -1;

static char      py_exe_name [_MAX_PATH];  /* python.exe on PATH (or curr dir). */
static char      py_dll_name [_MAX_PATH];  /* {lib}python*.dll in directory of 'py_exe_name'. */

static PyObject *py_catcher  = NULL;
static HANDLE    py_pipe_hnd = INVALID_HANDLE_VALUE;
static HANDLE    py_dll_hnd  = INVALID_HANDLE_VALUE;

#define LOAD_FUNC(f)         do {                                              \
                               f = (func_##f) GetProcAddress (py_dll_hnd, #f); \
                               if (!f) {                                       \
                                 WARN ("Failed to find \"%s()\" in %s.\n",     \
                                       #f, py_dll_name);                       \
                                 goto failed;                                  \
                               }                                               \
                               DEBUGF (3, "Function %s(): %*s 0x%p\n",         \
                                          #f, 23-strlen(#f), "", f);           \
                             } while (0)

#define DEF_FUNC(ret,f,args) typedef ret (__cdecl *func_##f) args; \
                             static func_##f f

DEF_FUNC (void,             Py_InitializeEx,        (int init_sigs));
DEF_FUNC (void,             Py_Finalize,            (void));
DEF_FUNC (void,             Py_SetProgramName,      (char *name));
DEF_FUNC (int,              PyRun_SimpleString,     (const char *cmd));
DEF_FUNC (PyObject*,        PyImport_AddModule,     (const char *name));
DEF_FUNC (PyObject*,        PyObject_GetAttrString, (PyObject *o, char *attr));
DEF_FUNC (char *,           PyString_AsString,      (PyObject *o));
DEF_FUNC (Py_ssize_t,       PyString_Size,          (PyObject *o));
DEF_FUNC (void,             PyObject_Free,          (PyObject *obj));
DEF_FUNC (void,             Py_DecRef,              (PyObject *obj));
DEF_FUNC (PyObject*,        PyObject_CallMethod,    (PyObject *o, char *method, char *fmt, ...));

#if PY_THREAD_SAVE
  DEF_FUNC (void,           PyEval_InitThreads,     (void));
  DEF_FUNC (PyThreadState*, PyEval_SaveThread,      (void));
  DEF_FUNC (void,           PyEval_RestoreThread,   (PyThreadState *));
#endif

static int report_py_version (char *output, int line)
{
  int num = sscanf (output, "%d.%d.%d", &py_major, &py_minor, &py_micro);

  DEBUGF (2, "num: %d, line: %d, major: %d, minor: %d, micro: %d\n",
          num, line, py_major, py_minor, py_micro);
  return  (num >= 2);
}

/*
 * \todo: find the newest python*.exe on PATH.
 * \todo: find all supported Python interpreters on PATH.
 */
static char *get_python_exe_name (void)
{
  const char *p;

  if (py_exe_name[0])
     return (py_exe_name);

  p = getenv ("PYTHON");
  if (!p)
       p = searchpath ("python.exe", "PATH");
  else p = slashify (p, '\\');

  _strlcpy (py_exe_name, p ? p : "python.exe", sizeof(py_exe_name));

  if (!FILE_EXISTS(py_exe_name))
  {
    WARN ("Failed to find \"%s\".\n", py_exe_name);
    return (NULL);
  }

  DEBUGF (2, "Using Python program: \"%s\".\n", py_exe_name);
  return (py_exe_name);
}

/*
 * Find the FQFN of the Python DLL.
 *
 * Note: The DLL-name of the official Python is named 'pythonXY.dll'.
 *       Python as distributed with GCC/GDB is named 'libpythonX.Y.dll'.
 *       This latter never seems to be in System-directory.
 *
 * Try the offical name first.
 *
 * \todo: Rewrite and use scan_for_libs() below.
 */
static char *get_python_dll_name (void)
{
  char *dir;
  char dll1 [_MAX_PATH];
  char dll2 [_MAX_PATH];
  char dll3 [_MAX_PATH];
  char sdir  [_MAX_PATH];

  if (!py_exe_name[0])
     return (NULL);

  if (!get_python_version(NULL, &py_major, &py_minor, &py_micro))
     return (NULL);

  dir = dirname (py_exe_name);
  DEBUGF (2, "dir of py_exe_name \"%s\"\n", dir);
  if (!dir)
     return (NULL);

  if (GetSystemDirectory(sdir, sizeof(sdir)))
       snprintf (dll1, sizeof(dll1), "%s\\python%d%d.dll", sdir, py_major, py_minor);  /* pri 1 */
  else strcpy (dll1, "??");

  snprintf (dll2, sizeof(dll2), "%s\\python%d%d.dll", dir, py_major, py_minor);        /* pri 2 */
  snprintf (dll3, sizeof(dll3), "%s\\libpython%d.%d.dll", dir, py_major, py_minor);    /* pri 3 */

  DEBUGF (2, "checking for:\n  \"%s\"\n  \"%s\"\n  \"%s\"\n", dll1, dll2, dll3);

  if (FILE_EXISTS(dll1))
  {
    FREE (dir);
    DEBUGF (2, "Found \"%s\"\n", dll1);
    return strcpy (py_dll_name, dll1);
  }
  if (FILE_EXISTS(dll2))
  {
    FREE (dir);
    DEBUGF (2, "Found \"%s\"\n", dll2);
    return strcpy (py_dll_name, dll2);
  }
  if (FILE_EXISTS(dll3))
  {
    FREE (dir);
    DEBUGF (2, "Found \"%s\"\n", dll3);
    return strcpy (py_dll_name, dll3);
  }

  DEBUGF (2, "No Python .dll found where python.exe is (\"%s\")\n", dir);
  FREE (dir);
  return (NULL);
}

int get_python_version (const char **py_exe, int *major, int *minor, int *micro)
{
  char  cmd [_MAX_PATH + sizeof(PY_GET_VERSION) + 10];
  char *exe = get_python_exe_name();

  if (!exe)
     return (0);

  snprintf (cmd, sizeof(cmd), "%s -c \"%s\"", exe, PY_GET_VERSION);

  if ((py_major > -1 && py_minor > -1) ||
      popen_run(cmd, report_py_version) >= 1)
  {
    if (major)
       *major  = py_major;
    if (minor)
       *minor  = py_minor;
    if (micro)
       *micro  = py_micro;
    if (py_exe)
       *py_exe = py_exe_name;
    return (1);
  }
  return (0);
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

int init_python_embedding (void)
{
  char  prog_name [_MAX_PATH] = "?";
  char *exe, *dll;

  if (py_dll_hnd != INVALID_HANDLE_VALUE)
     return (1);

  exe = get_python_exe_name();
  dll = get_python_dll_name();
  if (!exe || !dll)
  {
    WARN ("Failed to find any Python DLLs.\n");
    return (0);
  }

  py_dll_hnd = LoadLibrary (dll);
  if (!py_dll_hnd || py_dll_hnd == INVALID_HANDLE_VALUE)
  {
    WARN ("Failed to load %s; %s\n", dll, win_strerror(GetLastError()));
    return (0);
  }

  DEBUGF (2, "Full DLL name: \"%s\". Handle: 0x%p\n", py_dll_name, py_dll_hnd);

  LOAD_FUNC (Py_InitializeEx);
  LOAD_FUNC (Py_Finalize);
  LOAD_FUNC (Py_SetProgramName);
  LOAD_FUNC (PyRun_SimpleString);
#if PY_THREAD_SAVE
  LOAD_FUNC (PyEval_InitThreads);
  LOAD_FUNC (PyEval_SaveThread);
  LOAD_FUNC (PyEval_RestoreThread);
#endif
  LOAD_FUNC (PyObject_GetAttrString);
  LOAD_FUNC (PyImport_AddModule);
  LOAD_FUNC (PyString_AsString);
  LOAD_FUNC (PyString_Size);
  LOAD_FUNC (PyObject_CallMethod);
  LOAD_FUNC (PyObject_Free);
  LOAD_FUNC (Py_DecRef);

  GetModuleFileName (NULL, prog_name, sizeof(prog_name));

  (*Py_InitializeEx) (0);
  (*Py_SetProgramName) (prog_name);

#if PY_THREAD_SAVE
  (*PyEval_InitThreads)();
#endif

  py_catcher = setup_stdout_catcher();
  if (py_catcher)
     return (1);

  /* Fall through */

failed:
  exit_python_embedding();
  return (0);
}

void exit_python_embedding (void)
{
  if (py_pipe_hnd && py_pipe_hnd != INVALID_HANDLE_VALUE)
     CloseHandle (py_pipe_hnd);

  if (py_dll_hnd && py_dll_hnd != INVALID_HANDLE_VALUE)
  {
    if (Py_Finalize)
    {
      DEBUGF (4, "Calling Py_Finalize().\n");
      (*Py_Finalize)();
    }
    CloseHandle (py_dll_hnd);
  }
  py_pipe_hnd = py_dll_hnd = INVALID_HANDLE_VALUE;
}

/*
 * Call the Python code in 'py_prog'. The returned memory (!= NULL)
 * MUST be freed using FREE().
 */
char *call_python_func (const char *py_prog)
{
  PyObject  *obj;
  Py_ssize_t size;
  char      *str = NULL;
  int        rc;

#if PY_THREAD_SAVE
  PyThreadState *ts = (*PyEval_SaveThread)();
  DEBUGF (4, "PyEval_SaveThread(): %p\n", thread_state);
#endif

  DEBUGF (3, "py_prog:\n"
             "----------------------\n%s\n"
             "----------------------\n", py_prog);

  rc  = (*PyRun_SimpleString) (py_prog);
  obj = (*PyObject_GetAttrString) (py_catcher, "value");

  DEBUGF (4, "rc: %d, obj: %p\n", rc, obj);

  if (rc == 0 && obj)
  {
    size = (*PyString_Size) (obj);
    if (size > 0)
       str = STRDUP ((*PyString_AsString)(obj));

    /* Reset the 'py_catcher' buffer value to prepare for next call
     * to this 'call_python_func()'.
     */
    (*PyObject_CallMethod) (py_catcher, "reset", NULL);
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

#if 0
  test_all_pythons();

#else

  if (!init_python_embedding())
     return;

  str = call_python_func (prog);
  printf ("Captured output of Python:\n** %s **\n", str);
  if (str)
     FREE (str);

  printf ("The rest should not be captured:\n");

  /* This should return no output.
   */
  ASSERT (call_python_func ("sys.stdout = old_stdout\n") == NULL);
  str = call_python_func (prog);
  printf ("Captured output of Python now:\n** %s **\n", str);
  if (str)
     FREE (str);
#endif
}

/*
 * Add the 'dir' to 'py_array[]' at index 'i'.
 * This assumes the python program in 'PY_PRINT_SYS_PATH' only returns
 * correct 'sys.path[]' components.
 *
 * Note: the 'dir' is allocated memory from 'call_python_func()' which
 *       is freed at end of 'do_check_python()'.
 */
static int add_to_py_array (char *dir, int index)
{
  struct python_array *py = py_array + index;
  struct stat st;
  int    j;

  if (index >= DIM(py_array))
  {
    WARN ("Too many paths in 'sys.path[]'. Max %d\n", index);
    return (0);
  }

  memset (&st, '\0', sizeof(st));
  py->dir    = dir;
  py->exist  = (stat(dir, &st) == 0);
  py->is_dir = py->exist && _S_ISDIR(st.st_mode);
  py->is_zip = py->exist && _S_ISREG(st.st_mode) && check_if_zip (dir);

  for (j = 0; j < index; j++)
      if (!stricmp(dir,py_array[j].dir))
         py->num_dup++;

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
  struct tm   tm;
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

  if (!py_home)
  {
    static BOOL do_warn = TRUE;

    py_home = getenv ("PYTHONHOME");
    if (do_warn && !FILE_EXISTS(py_home))
       WARN ("%%PYTHONHOME points to non-existing directory: \"%s\".\n", py_home);
    do_warn = FALSE;
  }

  len = snprintf (report, sizeof(report), "%s  (", file_within_zip);

  /* Figure out if and where in %PYTHONHOME 'zip_file' is equal.
   */
  p = py_home ? path_ltrim (zip_file, py_home) : zip_file;
  if (p > zip_file)
       snprintf (report+len, sizeof(report)-len, "%%PYTHONHOME%%\\%s)", p);
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
          "  if fnmatch.fnmatch (base, '%s'):\n"   /* opt.file_spec */                \
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
#define PY_PRINT_SYS_PATH  "import sys\n" \
                           "for (i,p) in enumerate(sys.path):\n" \
                            "  print ('%s\\n' % p)\n"

int do_check_python (void)
{
  struct python_array *py;
  int    index, found = 0;
  char  *l, *str = NULL;

  if (!init_python_embedding())
     return (0);

  memset (&py_array, 0, sizeof(py_array));

  str = call_python_func (PY_PRINT_SYS_PATH);
  if (!str)
     goto quit;

  l = strtok (str, "\n");

  for (index = 0; l; index++)
  {
    DEBUGF (2, "l (index: %2d): \"%s\"\n", index, l);
    if (!add_to_py_array(l, index))
       break;
    l = strtok (NULL, "\n");
  }

  for (found = 0, py = py_array; py && py->dir; py++)
  {
    /* Don't warn on missing .zip files in 'sys.path[]' (unless in debug-mode)
     */
    if (!opt.debug && !py->exist && !stricmp(get_file_ext(py->dir), "zip"))
       py->exist = py->is_dir = TRUE;

    if (py->is_zip)
         found += process_zip (py->dir);
    else found += process_dir (py->dir, py->num_dup, py->exist, py->is_dir,
                               TRUE, "sys.path[]", NULL);
  }

quit:
  FREE (str);
  return (found);
}

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
  return (j);
}

/*
 * Doesn't work as single-threaded :-(
 * Maybe run the Python pipe-test in a separate thread?
 * Enable this test using 'envtool.exe --python --test'.
 */
int test_python_pipe (void)
{
  int   rc = 0;

#if !defined(__CYGWIN__)
  int   fd = -1;
  char  py_cmd [500];
  FILE *f  = NULL;

  if (!init_python_embedding())
     return (0);

  /* Under Windows, named pipes MUST have the form
   * "\\<server>\pipe\<pipename>". <server> may be "." for localhost.
   */
  py_pipe_hnd = CreateNamedPipe (PY_PIPE_NAME,
                                 PIPE_ACCESS_DUPLEX,  /* read/write pipe */
                                 PIPE_TYPE_BYTE | PIPE_NOWAIT,
                                 1, 4*1024, 4*1024, 0, NULL);
  if (py_pipe_hnd == INVALID_HANDLE_VALUE)
  {
    WARN ("CreateNamedPipe (\"%s\") failed; %s\n", PY_PIPE_NAME,
          win_strerror(GetLastError()));
    return (0);
  }

  fd = _open_osfhandle ((intptr_t)py_pipe_hnd, _O_BINARY /* _O_TEXT */);
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

  DEBUGF (3, "py_pipe_hnd: 0x%p, fd: %d.\n", (void*)py_pipe_hnd, fd);

  snprintf (py_cmd, sizeof(py_cmd), PY_PIPE_WRITE_PROG, PY_PIPE_NAME);
  DEBUGF (2, "py_cmd:\n----------------------\n%s\n----------------------\n",
          py_cmd);

  rc = (*PyRun_SimpleString) (py_cmd);
  if (rc == 0)
     rc = fgets_loop (f, add_to_py_array);
  fclose (f);
#endif  /* __CYGWIN__ */

  return (rc);
}


static int this_py_major = -1;
static int this_py_minor = -1;
static int this_py_micro = -1;

static int report_this_py_version_cb (char *output, int line)
{
  int num = sscanf (output, "%d.%d.%d", &this_py_major, &this_py_minor, &this_py_micro);
  return  (num >= 2);
}

/*
 * \todo: find the newest 'py_exe' on PATH.
 */
static char *get_this_python_exe_name (const char *py_exe)
{
  const char *p = searchpath (py_exe, "PATH");

  if (!p || !FILE_EXISTS(p))
     return (NULL);

  /* Returns allocated memory
   */
  return _fixpath (p, NULL);
}

/*
 * \todo:
 *  If multiple DLLs with same name are found (in 'dirname(exe)' and/or
 *  SystemDir/PATH), report a warning. Check PE-version and/or MD5 finger-
 *  print?
 */
static const char *scan_for_libs (const char *exe, int major, int minor,
                                  const char **libs, size_t num)
{
  char  *path = dirname (exe);
  const  char *lib, *rc = NULL;
  static char dll [_MAX_PATH];
  size_t i;

  for (i = 0, lib = libs[0]; lib && i < num; lib = libs[++i])
  {
    if (strchr(lib,'%'))
    {
      char lib2 [_MAX_PATH];
      snprintf (lib2, sizeof(lib2), lib, major, minor);
      lib = lib2;
    }
    snprintf (dll, sizeof(dll), "%s\\%s", path, lib);
    if (FILE_EXISTS(dll))
    {
      rc = dll;
      break;
    }
    DEBUGF (2, "dll: %s, rc: %s.\n", dll, rc);
  }
  FREE (path);
  return (rc);
}

static int print_sys_path_cb (char *output, int line)
{
  printf ("line %d: %s\n", line, output);
  return (0);
}

static void print_sys_path (const struct python_info *py)
{
  #define PY_PRINT_SYS_PATH2  "import os, sys; " \
                              "[os.write(1,'%s\\n' % p) for (i,p) in enumerate(sys.path)]"

  char  cmd [_MAX_PATH + sizeof(PY_PRINT_SYS_PATH2) + 10];

  snprintf (cmd, sizeof(cmd), "%s -c \"%s\"", py->exe_name, PY_PRINT_SYS_PATH2);
  popen_run (cmd, print_sys_path_cb);
}

int test_all_pythons (void)
{
  struct python_info *py;
  int    i, found = 0;

  for (i = 0, py = all_py_programs; i < DIM(all_py_programs); i++, py = all_py_programs+i)
  {
    char  cmd [_MAX_PATH + sizeof(PY_GET_VERSION) + 10];
    char *dll, *exe = get_this_python_exe_name (py->program);
    int   save, popen_rc;

    py->ver_major = py->ver_minor = py->ver_micro = -1;
    if (!exe)
    {
      DEBUGF (0, "%d: \"%s\" not found\n", i, py->program);
      continue;
    }

    _strlcpy (py->exe_name, exe, sizeof(py->exe_name));
    FREE (exe);

    _strlcpy (py->dll_name, "<unknown>", sizeof(py->dll_name));

    save = opt.debug;
    opt.debug = 0;
    snprintf (cmd, sizeof(cmd), "%s -c \"%s\"", py->exe_name, PY_GET_VERSION);
    popen_rc = popen_run (cmd, report_this_py_version_cb);
    opt.debug = save;

    if (popen_rc >= 1)
    {
      py->ver_major = this_py_major;
      py->ver_minor = this_py_minor;
      py->ver_micro = this_py_micro;

      dll = scan_for_libs (py->exe_name, py->ver_major, py->ver_minor,
                           py->libraries, DIM(py->libraries));
      if (dll)
        _strlcpy (py->dll_name, dll, sizeof(py->dll_name));

      print_sys_path (py);
      found++;
    }

    DEBUGF (0, "%d: \"%s\" found -> \"%s\".\n"
               "    DLL: %s\n"
               "    ver. %d.%d.%d\n\n",
            i, py->program, py->exe_name, py->dll_name,
            py->ver_major, py->ver_minor, py->ver_micro);
  }
  return (found);
}
