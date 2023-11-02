/** \file    get_file_assoc.h
 *  \ingroup Misc
 */
#pragma once

#include <stdbool.h>

extern bool get_file_assoc_all (const char *extension);
extern bool get_file_assoc (const char *extension, char **program, char **exe);
extern bool get_actual_filename (char **file_p, bool allocated);

