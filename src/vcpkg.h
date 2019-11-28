/** \file vcpkg.h
 */
#ifndef _VCPKG_H
#define _VCPKG_H

/**
 * \def VCPKG_MAX_NAME
 * \def VCPKG_MAX_VERSION
 */
#define VCPKG_MAX_NAME     30   /**< The max size of a `vcpkg_node::package` and `vcpkg_package::package` entry */
#define VCPKG_MAX_VERSION  30   /**< The max size of a `vcpkg_node::version` entry */
#define VCPKG_MAX_URL     200   /**< The max size of a `vcpkg_node::homepage` entry */

/**
 * \enum VCPKG_platform
 * The platform enumeration.
 */
typedef enum VCPKG_platform {
        VCPKG_plat_ALL     = 0,        /**< Package is for all supported OSes */
        VCPKG_plat_WINDOWS = 0x0001,   /**< Package is for Windows only (desktop, not UWP). */
        VCPKG_plat_UWP     = 0x0002,   /**< Package is for Universal Windows Platform only. */
        VCPKG_plat_LINUX   = 0x0004,   /**< Package is for Linux only. */
        VCPKG_plat_x86     = 0x0008,   /**< Package is for x86 processors only. */
        VCPKG_plat_x64     = 0x0010,   /**< Package is for x64 processors only. */
        VCPKG_plat_ARM     = 0x0020,   /**< Package is for ARM processors only. */
        VCPKG_plat_ANDROID = 0x0040,   /**< Package is for Android only. */
        VCPKG_plat_OSX     = 0x0080    /**< Package is for Apple's OSX only. */
      } VCPKG_platform;

/**
 * \def VCPKG_platform_INVERSE
 * Package is the inverse of the above specified bit.
 */
#define VCPKG_platform_INVERSE  0x8000

/**
 * \struct vcpkg_node
 * The structure of a single VCPKG package entry in the `vcpkg_nodes`.
 */
struct vcpkg_node {
       char  package [VCPKG_MAX_NAME];     /**< The package name */
       char  version [VCPKG_MAX_VERSION];  /**< The version */
       char  homepage [VCPKG_MAX_URL];     /**< The URL of it's home-page */
       char *description;                  /**< The description */
       BOOL  have_CONTROL;                 /**< TRUE if this is a CONTROL-node */

       /** The dependencies; a smartlist of `struct vcpkg_package *`.
        */
       smartlist_t *deps;

       /** The features; a smartlist of `char *`.
        */
       smartlist_t *features;
     };

/**
 * \struct vcpkg_package
 * The structure of a single installed VCPKG package or the
 * structure of a package-dependency.
 */
struct vcpkg_package {
       char               package [VCPKG_MAX_NAME]; /**< The package name */
       VCPKG_platform     platform;                 /**< The supported OS platform */
       struct vcpkg_node *link;                     /**< A link to the corresponding CONTROL node */
     };

extern void        vcpkg_init (void);
extern void        vcpkg_exit (void);
extern unsigned    vcpkg_get_num_CONTROLS (void);
extern unsigned    vcpkg_get_num_portfile (void);
extern unsigned    vcpkg_get_num_installed (void);
extern unsigned    vcpkg_list_installed (void);
extern unsigned    vcpkg_find (const char *package_spec);
extern BOOL        vcpkg_get_only_installed (void);
extern BOOL        vcpkg_set_only_installed (BOOL True);
extern const char *vcpkg_last_error (void);

#endif



