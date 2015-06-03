#ifndef _ENVTOOL_PY_H
#define _ENVTOOL_PY_H

struct python_array {
       char *dir;           /* FQDN of this entry */
       int   exist;         /* does it exist? */
       int   is_dir;        /* and is it a dir; _S_ISDIR() */
       int   is_zip;        /* or is it a zip; an .EGG or .zip-file. */
       int   num_dup;       /* # of duplicates elsewhere in 'sys.path[]' */
     };

enum python_variants {
     DEFAULT_PYTHON,
     PY2_PYTHON,
     PY3_PYTHON,
     PY2or3_PYTHON,
     IRON2_PYTHON,
     IRON3_PYTHON,
     PYPY_PYTHON,
     JYTHON_PYTHON,
     ALL_PYTHONS
   };

extern enum python_variants which_python;

extern int  init_python        (void);
extern void exit_python        (void);
extern int  get_python_info    (const char **exe, const char **dll, int *major, int *minor, int *micro);
extern int  do_check_python    (void);
extern void test_python_funcs  (void);
extern int  test_python_pipe   (void);
extern int  test_pythons       (void);
extern void searchpath_pythons (void);

#endif  /* _ENVTOOL_PY_H */

