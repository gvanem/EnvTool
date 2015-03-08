#define MS_NO_COREDLL
#undef snprintf
#undef vsnprintf

#include <Python.h>
#include <windows.h>

#define USE_PIPE 0

#if (USE_PIPE)
  #define PIPE_NAME       "\\\\.\\pipe\\envtool"
#endif

#if 0
  #define PYTHON_CMD_FMT  "import sys; try: f=open(r'%s','w'): except IOError: sys.exit(10);" \
                          "[f.write('%%s\\n' %% p) for (i,p) in enumerate(sys.path)]"
#else
  #define PYTHON_CMD_FMT  "import sys; f=open(r'%s','w'); " \
                          "[f.write('%%s\\n' %% p) for (i,p) in enumerate(sys.path)]; f.close()"
#endif

typedef int (*Py_Main_t) (int argc, const char **argv);

#if (USE_PIPE == 0)

#define PY_COMMANDS  "import sys\n"                          \
                     "f = open(r'%s','w')\n"                 \
                     "for i in range (1, len(sys.path)):\n"  \
                     "  f.write ('%%s\\n' %% sys.path[i])\n" \
                     "f.close()\n"

static char *create_temp (void)
{
  char *tmp = _tempnam (NULL, "envtool-tmp");

  if (tmp)
  {
    char *t = STRDUP (tmp);
    DEBUGF (2, " %s() tmp: '%s'\n", __FUNCTION__, tmp);
    free (tmp);
    return (t);     /* Caller must free() */
  }
  DEBUGF (2, " %s() _tempname() failed: %s\n", __FUNCTION__, strerror(errno));
  return (NULL);
}
#endif

static int fgets_loop (FILE *f, popen_callback callback)
{
  char  buf[256];
  int   i = 0;
  int   j = 0;

  DEBUGF (2, "fileno(f): %d\n", _fileno(f));

  while (fgets(buf,sizeof(buf),f))
  {
    int rc;

    strip_nl (buf);
    DEBUGF (2, " fgets(): '%s'\n", buf);
    if (!buf[0])
       continue;
    rc = (*callback) (NULL, buf, i++);
    if (rc < 0)
       break;
    j += rc;
  }
  return (j);
}

int do_check_python (void)
{
  int       rc, argc, found = 0;
  const char *argv[4];
  char     *py_in_file  = NULL;
  char     *py_out_file = NULL;
  char      py_cmd [200];
  FILE     *f = NULL;
  Py_Main_t py_main;
  HANDLE    module = INVALID_HANDLE_VALUE;

#if (USE_PIPE)
  HANDLE pipe = INVALID_HANDLE_VALUE;
  int    fd   = -1;
#else
  FILE  *f2 = NULL;
#endif

  struct python_array *py;

  memset (&py_array, 0, sizeof(py_array));

  module = LoadLibrary ("python27.dll");         /* todo: find the newest .DLL in PATH */
  if (!module || module == INVALID_HANDLE_VALUE)
  {
    WARN ("Failed to find python27.dll; %s\n", win_strerror(GetLastError()));
    goto failed;
  }

  if (debug >= 2)
     _putenv ("PYTHONVERBOSE=1");

  py_main = (Py_Main_t) GetProcAddress (module, "Py_Main");
  if (!py_main)
  {
    WARN ("Failed to find 'Py_Main()' in python27.dll; %s\n",
          win_strerror(GetLastError()));
    goto failed;
  }

#if (USE_PIPE)
  py_out_file = PIPE_NAME;

  /* Under Windows, named pipes MUST have the form
   * "\\<server>\pipe\<pipename>".  <server> may be "." for localhost.
   */
  pipe = CreateNamedPipe (PIPE_NAME,
                          PIPE_ACCESS_DUPLEX,  /* read/write pipe */
                          PIPE_TYPE_BYTE | PIPE_WAIT,
                          1, 4*1024, 4*1024, 0, NULL);
  if (pipe == INVALID_HANDLE_VALUE)
  {
    WARN ("CreateNamedPipe (\"%s\") failed; %s\n", PIPE_NAME, win_strerror(GetLastError()));
    goto failed;
  }
  fd = _open_osfhandle ((intptr_t)pipe, _O_TEXT);
  if (fd < 0)
  {
    DEBUGF (1, "_open_osfhandle() failed: %s.\n", strerror(errno));
    goto failed;
  }

  f = fdopen (fd, "r");
  if (!f)
  {
    DEBUGF (1, "fdopen() failed: %s.\n", strerror(errno));
    goto failed;
  }

#else
  py_in_file  = create_temp();
  py_out_file = create_temp();
  f2 = fopen (py_in_file, "w+t");
  fprintf (f2, PY_COMMANDS, py_out_file);
  fclose (f2);
#endif

  snprintf (py_cmd, sizeof(py_cmd), PYTHON_CMD_FMT, py_out_file);

  argv[0] = who_am_I;

#if (USE_PIPE == 0)
  argv[1] = py_in_file;
  argc = 2;
#else
  argv[1] = "-c";
  argv[2] = py_cmd;
  argc = 3;
#endif

  argv [argc] = NULL;

  DEBUGF (2, "Calling Py_Main():\n"
             "  argv[0] = \"%s\"\n"
             "  argv[1] = \"%s\"\n"
             "  argv[2] = \"%s\"\n"
             "  argv[3] = (null).\n", argv[0], argv[1], argv[2]);

  rc = (*py_main) (argc, argv);
  DEBUGF (1, "Py_Main(): rc=%d\n", rc);
  if (rc == 0)
  {
#if (USE_PIPE == 0)
    f = fopen (py_out_file, "rt");
#endif
    found = fgets_loop (f, add_to_py_array);
  }

  for (found = 0, py = py_array; py && py->dir; py++)
  {
    /* Don't warn on missing .zip files in 'sys.path[]' (unless in debug-mode)
     */
    if (!debug && !py->exist)
    {
      BOOL is_a_zip = !strnicmp (strrchr(py->dir,'\0')-4, ".zip", 4);
      if (is_a_zip)
         py->exist = py->is_dir = TRUE;
    }

    if (py->is_zip)
         found += process_zip (py->dir);
    else found += process_dir (py->dir, py->num_dup, py->exist, py->is_dir, TRUE,
                               "sys.path[]", NULL);
  }
  for (py = py_array; py && py->dir; py++)
      FREE (py->dir);

failed:
  if (f)
     fclose (f);

#if (USE_PIPE)
  if (pipe && pipe != INVALID_HANDLE_VALUE)
     CloseHandle (pipe);
#else
  if (py_in_file)
  {
    _unlink (py_in_file);
    FREE (py_in_file);
  }
  if (py_out_file)
  {
    _unlink (py_out_file);
    FREE (py_out_file);
  }
#endif

  if (module && module != INVALID_HANDLE_VALUE)
     CloseHandle (module);

  return (found);
}



