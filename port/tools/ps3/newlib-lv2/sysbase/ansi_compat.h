/* The declaration macros newlib's _ansi.h had until 2.x, which these glue sources (ps3toolchain's
 * libsysbase, written against newlib 1.20) use; newlib 4.6 no longer defines them. Given to every
 * source by the Makefile's -include. */
#ifndef OPENRAC_ANSI_COMPAT_H
#define OPENRAC_ANSI_COMPAT_H
#define _DEFUN(name, arglist, args) name(args)
#define _DEFUN_VOID(name) name(void)
#define _AND ,
#define _VOID void
#define _CONST const
#define _PTR void*
#define _EXFUN(name, proto) name proto
#define _PARAMS(proto) proto
#endif
