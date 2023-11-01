/**\file    compiler.h
 * \ingroup Compiler
 */
#ifndef _COMPILER_H
#define _COMPILER_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * \enum compiler_type
 * The compiler types supported
 */
typedef enum compiler_type {
        CC_UNKNOWN = 0,  /**< Unknown compiler (not initialised value) */
        CC_GNU_GCC,      /**< == 1: Some sort of (prefixed) `*gcc.exe`. */
        CC_GNU_GXX,      /**< == 2: Some sort of (prefixed) `*g++.exe`. */
        CC_MSVC,         /**< == 3: A MSVC compiler */
        CC_CLANG,        /**< == 4: A clang/clang-cl compiler */
        CC_INTEL,        /**< == 5: A Intel oneAPI compiler */
        CC_BORLAND,      /**< == 6: A Borland / Embarcadero compiler */
        CC_WATCOM        /**< == 7: A Watcom/OpenWatcom compiler */
      } compiler_type;

struct compiler_info;  /* Opaque structure */

struct compiler_info *compiler_lookup (compiler_type type);
const char           *compiler_GCC_prefix_first (void);
const char           *compiler_GCC_prefix_next (void);

void compiler_init (BOOL print_info, BOOL print_lib_path);
void compiler_exit (void);

int  compiler_check_includes (compiler_type type);
int  compiler_check_libraries (compiler_type type);
BOOL compiler_cfg_handler (const char *section, const char *key, const char *value);

/**
 * Print compiler version and CFLAGS + LDFLAGS the
 * program was built with.
 */
const char *compiler_version (void);
const char *compiler_clang_version (void);
void        compiler_print_build_cflags (void);
void        compiler_print_build_ldflags (void);

#ifdef __cplusplus
}
#endif

#endif
