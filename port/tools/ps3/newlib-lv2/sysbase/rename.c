#include "config.h"
#include <_ansi.h>
#include <_syslist.h>
#include <reent.h>
#include <errno.h>
#include <sys/syscalls.h>

#ifdef REENTRANT_SYSCALLS_PROVIDED
int
_DEFUN(_rename_r,(r,old,new),
	   struct _reent *r _AND
	   const char *old _AND
	   const char *new) {
#else
int
_DEFUN(_rename,(old,new),
	   const char *old _AND
	   const char *new) {
	struct _reent *r = _REENT;
#endif
	if(__syscalls.rename_r) {
		/* POSIX replaces a destination that exists; lv2 refuses it (EEXIST), so it is removed first. */
		int ret = __syscalls.rename_r(r,old,new);
		if(ret == -1 && r->_errno == EEXIST && __syscalls.unlink_r && __syscalls.unlink_r(r,new) == 0)
			ret = __syscalls.rename_r(r,old,new);
		return ret;
	}

	r->_errno = ENOSYS;
	return -1;
}
