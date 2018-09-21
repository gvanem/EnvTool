/** \file vcpkg.h
 */
#ifndef _VCPKG_H
#define _VCPKG_H

#define VCPKG_MAX_NAME    20
#define VCPKG_MAX_VERSION 30
#define VCPKG_MAX_DESCR   200

struct vcpkg_node {
       char *descr;
       char *build_depends;

       char  package [VCPKG_MAX_NAME];
       char  source  [VCPKG_MAX_NAME];
       char  version [VCPKG_MAX_VERSION];
       BOOL  have_CONTROL;
       BOOL  have_portfile_cmake;
     };

extern int         vcpkg_list (void);
extern void        vcpkg_free (void);
extern int         vcpkg_dump (void);
extern int         vcpkg_dump_control (const char *packages);
extern int         vcpkg_get_control (const struct vcpkg_node **node_p, const char *packages);
extern const char *vcpkg_last_error (void);

#endif



