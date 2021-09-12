/** \file vcpkg.h
 *  \ingroup Misc
 */
#ifndef _VCPKG_H
#define _VCPKG_H

extern void        vcpkg_init (void);
extern void        vcpkg_exit (void);
extern BOOL        vcpkg_get_info (char **exe_p, struct ver_info *ver);
extern unsigned    vcpkg_get_num_CONTROLS (void);
extern unsigned    vcpkg_get_num_JSON (void);
extern unsigned    vcpkg_get_num_portfile (void);
extern unsigned    vcpkg_get_num_installed (void);
extern unsigned    vcpkg_list_installed (BOOL detailed);
extern unsigned    vcpkg_find (const char *package_spec);
extern BOOL        vcpkg_get_only_installed (void);
extern BOOL        vcpkg_set_only_installed (BOOL True);
extern const char *vcpkg_last_error (void);
extern void        vcpkg_clear_error (void);
extern void        vcpkg_extras (const struct ver_data *v, int pad_len);

extern int test_vcpkg_json_parser (void);

#endif



