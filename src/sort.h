/** \file sort.h
 */
#ifndef _SORT_H
#define _SORT_H

/** \enum SortMethod
 *
 * Used with the `-S` or `--sort` cmd-line options to sort
 * matches on file-name, file-extension, date/time, size or version.
 * The latter only applies to *PE-files* with version-information in it's
 * *Resource* section.
 *
 * Always sort from low to high.
 *
 * Several of these values can be set in `opt.sort_methods[]` to form a first-level and
 * second-level sort.
 * \eg a `opt.sort_method[0] = SORT_FILE_NAME`,
 *       `opt.sort_method[1] = SORT_FILE_EXTENSION` and
 *       `opt.sort_method[2] = 0` applied to this list of files:
 * ```
 *  file-b.txt
 *  file-a.txt
 *  file-a.a
 *  file-b.a
 * ```
 *
 * will return this list of files:
 * ```
 * file-a.a
 * file-a.txt
 * file-b.a
 * file-b.txt
 * ```
 */
typedef enum SortMethod {
        SORT_FILE_UNSORTED   = 0x00,
        SORT_FILE_NAME       = 0x01,
        SORT_FILE_EXTENSION  = 0x02,
        SORT_FILE_DATETIME   = 0x04,
        SORT_FILE_SIZE       = 0x08,
        SORT_PE_VERSION      = 0x10
      } SortMethod;

extern BOOL        set_sort_method (const char *opt, char **err_opt);
extern const char *get_sort_methods (void);

#endif

