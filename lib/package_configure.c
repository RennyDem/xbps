/*-
 * Copyright (c) 2009-2014 Juan Romero Pardines.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "xbps_api_impl.h"
/**
 * @file lib/package_configure.c
 * @brief Package configuration routines
 * @defgroup configure Package configuration functions
 *
 * Configure a package or all packages. Only packages in XBPS_PKG_STATE_UNPACKED
 * state will be processed (unless overriden). Package configuration steps:
 *  - Its <b>post-install</b> target in the INSTALL script will be executed.
 *  - Its state will be changed to XBPS_PKG_STATE_INSTALLED if previous step
 *    ran successful.
 *
 * @note
 * If the \a XBPS_FLAG_FORCE_CONFIGURE is set through xbps_init() in the flags
  member, the package (or packages) will be reconfigured even if its
 * state is XBPS_PKG_STATE_INSTALLED.
 */
int
xbps_configure_packages(struct xbps_handle *xhp)
{
	xbps_dictionary_t pkgd;
	xbps_object_t obj;
	xbps_object_iterator_t iter;
	const char *pkgver;
	int rv;

	if ((rv = xbps_pkgdb_init(xhp)) != 0)
		return rv;

	iter = xbps_dictionary_iterator(xhp->pkgdb);
	assert(iter);
	while ((obj = xbps_object_iterator_next(iter))) {
		pkgd = xbps_dictionary_get_keysym(xhp->pkgdb, obj);
		xbps_dictionary_get_cstring_nocopy(pkgd, "pkgver", &pkgver);
		rv = xbps_configure_pkg(xhp, pkgver, true, false);
		if (rv != 0) {
			xbps_dbg_printf(xhp, "%s: failed to configure %s: %s\n",
			    __func__, pkgver, strerror(rv));
			break;
		}
	}
	xbps_object_iterator_release(iter);

	return rv;
}

int
xbps_configure_pkg(struct xbps_handle *xhp,
		   const char *pkgver,
		   bool check_state,
		   bool update)
{
	xbps_dictionary_t pkgd, pkgmetad;
	xbps_data_t imsg;
	FILE *f = NULL;
	const void *data = NULL;
	size_t len;
	char *buf = NULL, *pkgname, *plist;
	int rv = 0;
	pkg_state_t state = 0;

	assert(pkgver != NULL);

	if ((pkgname = xbps_pkg_name(pkgver)) == NULL) {
		xbps_dbg_printf(xhp, "[configure] cannot guess "
		    "pkgname for %s\n", pkgver);
		/* assume pkgver == pkgname */
		pkgname = strdup(pkgver);
		assert(pkgname);
	}
	pkgd = xbps_pkgdb_get_pkg(xhp, pkgname);
	if (pkgd == NULL) {
		xbps_dbg_printf(xhp, "[configure] cannot find %s (%s) "
		    "in pkgdb\n", pkgname, pkgver);
		free(pkgname);
		return ENOENT;
	}

	rv = xbps_pkg_state_dictionary(pkgd, &state);
	xbps_dbg_printf(xhp, "%s: state %d rv %d\n", pkgver, state, rv);
	if (rv != 0) {
		free(pkgname);
		xbps_dbg_printf(xhp, "%s: [configure] failed to get "
		    "pkg state: %s\n", pkgver, strerror(rv));
		return EINVAL;
	}

	if (check_state) {
		if (state == XBPS_PKG_STATE_INSTALLED) {
			if ((xhp->flags & XBPS_FLAG_FORCE_CONFIGURE) == 0) {
				free(pkgname);
				return 0;
			}
		} else if (state != XBPS_PKG_STATE_UNPACKED) {
			free(pkgname);
			return EINVAL;
		}
	}

	xbps_set_cb_state(xhp, XBPS_STATE_CONFIGURE, 0, pkgver, NULL);

	plist = xbps_xasprintf("%s/.%s.plist", xhp->metadir, pkgname);
	free(pkgname);

	pkgmetad = xbps_dictionary_internalize_from_file(plist);
	if (pkgmetad == NULL) {
		rv = errno;
		xbps_set_cb_state(xhp, XBPS_STATE_CONFIGURE_FAIL,
		    errno, pkgver,
		    "%s: [configure] cannot read metadata plist: %s",
		    pkgver, strerror(rv));
		return errno;
	}

	free(plist);
	assert(pkgmetad);

	rv = xbps_pkg_exec_script(xhp, pkgmetad, "install-script", "post", update);
	if (rv != 0) {
		xbps_set_cb_state(xhp, XBPS_STATE_CONFIGURE_FAIL,
		    errno, pkgver,
		    "%s: [configure] INSTALL script failed to execute "
		    "the post ACTION: %s", pkgver, strerror(rv));
		return rv;
	}
	if (state == XBPS_PKG_STATE_INSTALLED) {
		xbps_object_release(pkgmetad);
		return rv;
	}

	rv = xbps_set_pkg_state_dictionary(pkgd, XBPS_PKG_STATE_INSTALLED);
	if (rv != 0) {
		xbps_set_cb_state(xhp, XBPS_STATE_CONFIGURE_FAIL, rv,
		    pkgver, "%s: [configure] failed to set state to installed: %s",
		    pkgver, strerror(rv));
		return rv;
	}
	if (rv == 0)
		xbps_set_cb_state(xhp, XBPS_STATE_CONFIGURE_DONE, 0, pkgver, NULL);

	/* show install-msg if exists */
	imsg = xbps_dictionary_get(pkgmetad, "install-msg");
	if (xbps_object_type(imsg) != XBPS_TYPE_DATA)
		goto out;

	data = xbps_data_data_nocopy(imsg);
	len = xbps_data_size(imsg);
	if ((f = fmemopen(__UNCONST(data), len, "r")) == NULL) {
		rv = errno;
		xbps_dbg_printf(xhp, "[configure] %s: fmemopen %s\n", pkgver, strerror(rv));
		goto out;
	};
	buf = malloc(len);
	assert(buf);
	if (fread(buf, len, 1, f) != len) {
		if (ferror(f)) {
			rv = errno;
			xbps_dbg_printf(xhp, "[configure] %s: fread %s\n", pkgver, strerror(rv));
			fclose(f);
			free(buf);
			goto out;
		}
	}
	/* terminate buffer and notify client to show the post-install message */
	buf[len] = '\0';
	xbps_set_cb_state(xhp, XBPS_STATE_SHOW_INSTALL_MSG, 0, pkgver, "%s", buf);
	fclose(f);
	free(buf);

out:
	if (pkgmetad != NULL)
		xbps_object_release(pkgmetad);

	return rv;
}
