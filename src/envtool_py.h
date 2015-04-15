#ifndef _ENVTOOL_PY_H
#define _ENVTOOL_PY_H

struct python_array {
       char *dir;           /* FQDN of this entry */
       int   exist;         /* does it exist? */
       int   is_dir;        /* and is it a dir; _S_ISDIR() */
       int   is_zip;        /* or is it a zip; an .EGG or .zip-file. */
       int   num_dup;       /* # of duplicates elsewhere in %VAR? */
     };

extern int   do_check_python       (void);
extern int   get_python_version    (const char **py_exe, int *major, int *minor, int *micro);
extern int   init_python_embedding (void);
extern void  exit_python_embedding (void);
extern char *call_python_func      (const char *py_str);
extern void  test_python_funcs     (void);
extern int   test_python_pipe      (void);

#endif  /* _ENVTOOL_PY_H */

