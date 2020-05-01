/** \file tests.h
 *  \ingroup Misc
 */
#ifndef _TESTS_H
#define _TESTS_H

extern int do_tests (void);

#if defined(USE_SQLITE3)
extern void test_sqlite3 (void);   /* In win_sqlite3.c */
#endif

#endif /* _TESTS_H */
