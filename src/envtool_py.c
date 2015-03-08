/*
 * The Python functions for the envtool program.
 *
 * By Gisle Vanem <gvanem@yahoo.no> August 2011.
 *
 */

#include <fcntl.h>

#include "envtool.h"
#include "envtool_py.h"

#define PY_THREAD_SAVE  0
#define PY_GET_VERSION  "import sys; print (sys.version[:5])"
#define PY_PIPE_NAME    "\\\\.\\pipe\\envtool"

#define PyObject      void
#define PyThreadState void
#define Py_ssize_t    long

static struct python_array py_array [500];

static int       py_major = -1;
static int       py_minor = -1;
static int       py_micro = -1;

static PyObject *py_catcher  = NULL;
static HANDLE    py_pipe_hnd = INVALID_HANDLE_VALUE;
static HANDLE    py_dll_hnd  = INVALID_HANDLE_VALUE;
static char      py_dll_name [_MAX_PATH];  /* pythonXX.dll on %PATH. */

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

DEF_FUNC (void,           Py_InitializeEx,        (int init_sigs));
DEF_FUNC (void,           Py_Finalize,            (void));
DEF_FUNC (void,           Py_SetProgramName,      (char *name));
DEF_FUNC (int,            PyRun_SimpleString,     (const char *cmd));
DEF_FUNC (PyObject*,      PyImport_AddModule,     (const char *name));
DEF_FUNC (PyObject*,      PyObject_GetAttrString, (PyObject *o, char *attr));
DEF_FUNC (char *,         PyString_AsString,      (PyObject *o));
DEF_FUNC (Py_ssize_t,     PyString_Size,          (PyObject *o));
DEF_FUNC (void,           PyObject_Free,          (PyObject *obj));
DEF_FUNC (void,           Py_DecRef,              (PyObject *obj));
DEF_FUNC (PyObject*,      PyObject_CallMethod,    (PyObject *o, char *method, char *fmt, ...));

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

int get_python_version (int *major, int *minor, int *micro)
{
  char cmd[100];

  snprintf (cmd, sizeof(cmd), "python.exe -c \"%s\"", PY_GET_VERSION);

  if ((py_major > -1 && py_minor > -1) ||
      popen_run(cmd, report_py_version) >= 1)
  {
    *major = py_major;
    *minor = py_minor;
    *micro = py_micro;
    return (1);
  }
  return (0);
}

/*
 * todo: find the newest Python*.DLL in PATH.
 */
char *get_python_dll_name (void)
{
  if (!get_python_version(&py_major, &py_minor, &py_micro))
     return (NULL);

  snprintf (py_dll_name, sizeof(py_dll_name), "python%d%d.dll", py_major, py_minor);
  DEBUGF (2, "py_dll_name: \"%s\"\n", py_dll_name);
  return (py_dll_name);
}

/*
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
                       "catcher = catch_stdout()\n" \
                       "sys.stdout = catcher\n";

  PyObject *mod = (*PyImport_AddModule) ("__main__");          /* create main module */
  int       rc  = (*PyRun_SimpleString) (code);                /* invoke code to redirect */
  PyObject *obj = (*PyObject_GetAttrString) (mod, "catcher");  /* get our catcher created above */

  DEBUGF (4, "mod: %p, rc: %d, obj: %p\n", mod, rc, obj);
  return (obj);
}

int init_python_embedding (void)
{
  char  full_name [_MAX_PATH] = "?";
  char  prog_name [_MAX_PATH] = "?";
  char *dll;

  if (py_dll_hnd != INVALID_HANDLE_VALUE)
     return (1);

  dll = get_python_dll_name();
  if (!dll)
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

  if (GetModuleFileName(py_dll_hnd, full_name, sizeof(full_name)))
     _strlcpy (dll, full_name, sizeof(full_name));  /* Copy full name into py_dll_name[] */

  DEBUGF (2, "Full DLL name: \"%s\". Handle: 0x%p\n", full_name, py_dll_hnd);

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
    DEBUGF (4, "Calling Py_Finalize().\n");
    if (Py_Finalize)
      (*Py_Finalize)();
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

  DEBUGF (3, "py_prog:\n----------------------\n%s\n----------------------\n", py_prog);

  rc  = (*PyRun_SimpleString) (py_prog);
  obj = (*PyObject_GetAttrString) (py_catcher, "value");

  DEBUGF (4, "rc: %d, obj: %p\n", rc, obj);

  if (rc == 0 && obj)
  {
    size = (*PyString_Size) (obj);
    if (size > 0)
       str = STRDUP ((*PyString_AsString)(obj));
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
static int report_zip_file (const char *zip_file, char *output, int line)
{
  static char *py_home = NULL;
  struct tm   tm;
  const  char *space, *p, *file_within_zip;
  char   report [1024];
  int    num, len;
  time_t mtime;

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
  num = sscanf (p, "%4u%2u%2u.%2u%2u%2u", &tm.tm_year, &tm.tm_mon, &tm.tm_mday,
                                          &tm.tm_hour, &tm.tm_min, &tm.tm_sec);
  if (num != 6)
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
     py_home = getenv ("PYTHONHOME");

  len = snprintf (report, sizeof(report), "%s  (", file_within_zip);

  /* Figure out if and where in %PYTHONHOME 'zip_file' is equal.
   */
  p = py_home ? path_ltrim (zip_file, py_home) : zip_file;
  if (p > zip_file)
       snprintf (report+len, sizeof(report)-len, "%%PYTHONHOME\\%s)", p);
  else snprintf (report+len, sizeof(report)-len, "%s)", zip_file);

 /* zipinfo always reports 'file_within_zip' with '/' slashes. But simply slashify the
  * complete 'report' to use either '\\' or '/'.
  */
  _strlcpy (report, slashify(report, show_unix_paths ? '/' : '\\'), sizeof(report));

  report_file (report, mtime, FALSE, HKEY_PYTHON_EGG);
  ARGSUSED (line);
  return (1);
}

/*
 * List a ZIP/EGG-file (file) for a matching file_spec.
 */

/*
 * This goes into a buffer used in call_python_func().
 */
#define PY_ZIP_LIST                                                                   \
          "import os, sys, fnmatch, zipfile\n"                                        \
          "def print_zline (f, debug):\n"                                             \
          "  if debug >= 3:\n"                                                        \
          "     os.write (2, 'egg-file: %%s\\n' %% f.filename)\n"                     \
          "  if fnmatch.fnmatch (f.filename, '%s'):\n"   /* file_spec */              \
          "    date = \"%%4d%%02d%%02d\"  %% (f.date_time[0:3])\n"                    \
          "    time = \"%%02d%%02d%%02d\" %% (f.date_time[3:6])\n"                    \
          "    str = \"%%d %%s.%%s %%s\"  %% (f.file_size, date, time, f.filename)\n" \
          "    if debug:\n"                                                           \
          "       os.write (2,'str: \"%%s\"\\n' %% str)\n"                            \
          "    print (str)\n"                                                         \
          "\n"                                                                        \
          "zf = zipfile.ZipFile (r\"%s\", 'r')\n"        /* zfile */                  \
          "for f in zf.infolist():\n"                                                 \
          "  print_zline (f, %d)\n"                      /* debug */

static int process_zip (const char *zfile)
{
  char  cmd [_MAX_PATH + 1000];
  char *l, *str;
  int   found, len;

  if (sizeof(cmd) < sizeof(PY_ZIP_LIST) + _MAX_PATH + 100)
     FATAL ("cmd[] buffer at %s/%u too small.\n", __FILE__, __LINE__-6);

  len = snprintf (cmd, sizeof(cmd), PY_ZIP_LIST, file_spec, zfile, debug);
  str = call_python_func (cmd);
  DEBUGF (2, "len: %d, Python output: \"%s\"\n", len, str);

  for (found = 0, l = strtok(str,"\n"); l; l = strtok(NULL,"\n"), found++)
  {
    DEBUGF (2, "l: \"%s\", found: %d\n", l, found);
    if (!report_zip_file(zfile, l, found+1))
       break;
  }
  FREE (str);

  if (found == 0)
     DEBUGF (1, "No matches in %s for %s.\n", zfile, file_spec);

  ARGSUSED (len);
  return (found);
}

/*
 * Run python, figure out the 'sys.path[]' array and search along that
 * for matches. If a 'sys.path[]' component contains a ZIP/EGG-file, use
 * 'call_python_func()' to list files inside it for a match.
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
    DEBUGF (2, "l: \"%s\", index: %d\n", l, index);
    if (!add_to_py_array(l, index))
       break;
    l = strtok (NULL, "\n");
  }

  for (found = 0, py = py_array; py && py->dir; py++)
  {
    /* Don't warn on missing .zip files in 'sys.path[]' (unless in debug-mode)
     */
    if (!debug && !py->exist && !stricmp(get_file_ext(py->dir), "zip"))
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

  DEBUGF (2, "fileno(f): %d\n", _fileno(f));

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
 * Maybe run the Python pipe-test in a separate thread.
 */
int test_python_pipe (void)
{
  int   rc = 0;
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
  return (rc);
}









