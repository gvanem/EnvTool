/*
 * du - A simple "Disk Usage" program that
 * spawns `dirlist.exe` and calls `scandir2()`.
 */
#include <process.h>
#include "envtool.h"

int main (int argc, char **argv)
{
  char  my_dir  [_MAX_PATH] = "?";
  char  dirlist [_MAX_PATH + 100];
  char *end, *args [5];
  int   i, j;

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
  args [1] = "-r";

  i = 2;
  for (j = 1; j < argc; i++, j++)
  {
    if (i == DIM(args)-1)
    {
      fprintf (stderr, "Too many args. Max: %u.\n", DIM(args)-1);
      break;
    }
    args[i] = argv[j];
  }
  return _spawnvp (_P_WAIT, dirlist, args);
}

