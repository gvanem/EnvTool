/*
 * du - A simple "Disk Usage" program that
 * spawns `dirlist.exe` and calls `scandir2()`.
 */
#include <process.h>
#include "envtool.h"

/**
 * A long path must be passed quoted to the
 * `dirlist.exe` sub-program.
 */
static char *check_long_name (const char *arg)
{
  if (strchr(arg, ' '))
  {
    char dir [_MAX_PATH];

    snprintf (dir, sizeof(dir), "\"%s\"", arg);
    return strdup (dir);
  }
  return strdup (arg);
}

static void free_args (char **args)
{
  int i;
  for (i = 0; args[i]; i++)
     free (args[i]);
}

int main (int argc, char **argv)
{
  char  my_dir  [_MAX_PATH] = "?";
  char  dirlist [_MAX_PATH + 100];
  char *end, *args [5];
  int   i, j, rc;
  int   debug = (getenv("DU_DEBUG") != NULL);

  GetModuleFileName (NULL, my_dir, sizeof(my_dir));
  end = strrchr (my_dir, '\\');
  if (end)
     *end = '\0';

  snprintf (dirlist, sizeof(dirlist), "%s\\dirlist.exe", my_dir);
  if (access(dirlist, 0))
  {
    fprintf (stderr, "The program `%s` was not found.\n", dirlist);
    return (1);
  }

  memset (&args, '\0', sizeof(args));
  args [0] = "--disk-usage";

  i = 1;
  for (j = 0; j < argc; i++, j++)
  {
    if (i == DIM(args)-1)
    {
      fprintf (stderr, "Too many args. Max: %u.\n", DIM(args)-1);
      break;
    }
    args[i] = check_long_name (argv[j]);
    if (debug)
       fprintf (stderr, "args[i]: '%s'.\n", args[i]);
  }
  rc = _spawnvp (_P_WAIT, dirlist, (char const *const *) args);
  free_args (args + 1);
  return (rc);
}

