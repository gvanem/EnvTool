/** \file sort.h
 */
#ifndef _SORT_H
#define _SORT_H

/** \enum SortMethod
 *
 * Used with the "-S" or "--sort" cmd-line options to sort
 * matches on file-name, file-extension, date/time or size.
 * Always sort from low to high.
 */
typedef enum SortMethod {
        SORT_FILE_UNSORTED,
        SORT_FILE_NAME,
        SORT_FILE_EXTENSION,
        SORT_FILE_DATETIME,
        SORT_FILE_SIZE
      } SortMethod;

extern BOOL        set_sort_method (const char *short_opt, const char *long_opt);
extern const char *get_sort_methods_short (void);
extern const char *get_sort_methods_long (void);

#endif

