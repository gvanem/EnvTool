/** \file envtool_py.h
 *  \ingroup Envtool_PY
 */
#ifndef _ENVTOOL_PY_H
#define _ENVTOOL_PY_H

/**
 *\enum python_variants
 * UNKNOWN_PYTHON  we have not found any suitable Python yet.\n
 * DEFAULT_PYTHON  a suiteable Python found first on \c PATH.\n
 * PY2_PYTHON      a Python ver 2.x found and selected.\n
 * PY3_PYTHON      a Python ver 3.x found and selected.\n
 * IRON2_PYTHON    a IronPython ver 2.x found and selected.\n
 * IRON3_PYTHON    a IronPython ver 3.x found and selected.\n
 * PYPY_PYTHON     a PyPy ver x.x found and selected.\n
 * JYTHON_PYTHON   a JavaPython ver x.x found and selected.\n
 * ALL_PYTHONS     any Python found.
 */
enum python_variants {
     UNKNOWN_PYTHON,
     DEFAULT_PYTHON,
     PY2_PYTHON,
     PY3_PYTHON,
     IRON2_PYTHON,
     IRON3_PYTHON,
     PYPY_PYTHON,
     JYTHON_PYTHON,
     ALL_PYTHONS
   };

extern enum python_variants py_which;

struct python_info;

extern void         py_init          (void);
extern void         py_exit          (void);
extern int          py_search        (void);
extern void         py_searchpaths   (void);
extern int          py_test          (void);
extern int          py_get_info      (char **exe, char **dll, struct ver_info *ver);
extern const char **py_get_variants  (void);
extern const char  *py_variant_name  (enum python_variants v);
extern int          py_variant_value (const char *short_name, const char *full_name);
extern int          py_print_modules (void);
struct python_info *py_select        (enum python_variants which);
extern char        *py_execfile      (char **py_argv);

#endif  /* _ENVTOOL_PY_H */

