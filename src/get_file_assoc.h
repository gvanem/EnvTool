/** \file    get_file_assoc.h
 *  \ingroup Misc
 */
#ifndef _GET_FILE_ASSOC_H
#define _GET_FILE_ASSOC_H

extern BOOL get_file_assoc_all (const char *extension);
extern BOOL get_file_assoc (const char *extension, char **program, char **exe);
extern BOOL get_actual_filename (char **file_p, BOOL allocated);

#endif

