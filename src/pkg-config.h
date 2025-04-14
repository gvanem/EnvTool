/** \file    pkg-config.h
 *  \ingroup pkg_config
 */
#pragma once

#include <stdbool.h>

extern void     pkg_config_init (void);
extern void     pkg_config_exit (void);
extern int      pkg_config_search (const char *search_spec);
extern bool     pkg_config_get_info (char **exe, struct ver_info *ver);
extern int      pkg_config_get_details (const char *pc_file, const char *filler);
extern int      pkg_config_get_details2 (report *r);
extern unsigned pkg_config_get_num_installed (void);
extern unsigned pkg_config_list_installed (void);
extern void     pkg_config_extras (const struct ver_data *v, int pad_len);
