/** \file envtool_py.h
 *  \ingroup Envtool_PY
 */
#pragma once

/**
 * \typedef enum python_variants
 * The types of Python we support.
 */
typedef enum python_variants {
        UNKNOWN_PYTHON,      /**< we have not found any suitable Python yet. */
        DEFAULT_PYTHON,      /**< a suitable Python found first on `PATH`. */
        PY2_PYTHON,          /**< a version 2.x Python was found and selected. */
        PY3_PYTHON,          /**< a version 3.x Python was found and selected. */
        PYPY_PYTHON,         /**< a PyPy was found and selected (any version). */
        ALL_PYTHONS          /**< any Python found. */
      } python_variants;

extern enum python_variants py_which;

struct ver_info;  /* Forward; in envtool.h */

extern void         py_init          (void);
extern void         py_exit          (void);
extern int          py_search        (void);
extern void         py_searchpaths   (void);
extern int          py_test          (void);
extern bool         py_get_info      (char **exe, struct ver_info *ver);
extern const char **py_get_variants  (void);
extern const char  *py_variant_name  (enum python_variants v);
extern int          py_variant_value (const char *short_name, const char *full_name);
extern char        *py_execfile      (const char **py_argv, bool capture, bool as_import);
extern bool         py_cfg_handler   (const char *section, const char *key, const char *value);

