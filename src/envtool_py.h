/** \file envtool_py.h
 *  \ingroup Envtool_PY
 */
#ifndef _ENVTOOL_PY_H
#define _ENVTOOL_PY_H

/**
 *\enum   python_variants
 *\anchor python_variants
 * <table>
 *   <tr><td width="20%">\c UNKNOWN_PYTHON <td> we have not found any suitable Python yet.
 *   <tr><td>            \c DEFAULT_PYTHON <td> a suiteable Python found first on \c PATH.
 *   <tr><td>            \c UNKNOWN_PYTHON <td> we have not found any suitable Python yet.
 *   <tr><td>            \c DEFAULT_PYTHON <td> a suiteable Python found first on \c PATH.
 *   <tr><td>            \c PY2_PYTHON     <td> a Python ver 2.x found and selected.
 *   <tr><td>            \c PY3_PYTHON     <td> a Python ver 3.x found and selected.
 *   <tr><td>            \c IRON2_PYTHON   <td> a IronPython ver 2.x found and selected.
 *   <tr><td>            \c IRON3_PYTHON   <td> a IronPython ver 3.x found and selected.
 *   <tr><td>            \c PYPY_PYTHON    <td> a PyPy ver x.x found and selected.
 *   <tr><td>            \c JYTHON_PYTHON  <td> a JavaPython ver x.x found and selected.
 *   <tr><td>            \c ALL_PYTHONS    <td> any Python found. \anchor ALL_PYTHONS
 * </table>
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
extern char        *py_execfile      (const char **py_argv);

#endif  /* _ENVTOOL_PY_H */

