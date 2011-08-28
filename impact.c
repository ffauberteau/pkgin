/* $Id: impact.c,v 1.5 2011/08/28 21:38:45 imilh Exp $ */

/*
 * Copyright (c) 2009, 2010 The NetBSD Foundation, Inc.
 * All rights reserved.
 *
 * This code is derived from software contributed to The NetBSD Foundation
 * by Emile "iMil" Heitor <imil@NetBSD.org> .
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
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 */

/**
 * pkg_impact() calculates "impact" of installing a package
 * it is the heart of package install / update
 *
 * pkg_impact rationale:
 *
 * pkg_impact() receive a package list as an argument.
 * Those are the packages to be installed / upgraded.
 *
 * For every package, a dependency tree is loaded,
 * and every dependency is passed to deps_impact()
 * deps_impact() loops through local (installed) packages
 * and matches depencendy over them.
 * If the dependency exists but its version does not
 * satisfy pkg_match(), the package is marked as
 * "to upgrade", meaning it will be deleted (oldpkg) and
 * a new version will be installed (pkgname).
 *
 * Finally, the package itself is passed to deps_impact()
 * to figure out if it needs to be installed or upgraded
 */

#include "pkgin.h"

/**
 * \fn dep_present
 *
 * \brief check if a dependency is already recorded in the impact list
 */
static int
dep_present(Plisthead *impacthead, char *depname)
{
	Pkglist *pimpact;

	SLIST_FOREACH(pimpact, impacthead, next)
		if (pimpact->full != NULL &&
			pkg_match(depname, pimpact->full))
			return 1;

	return 0;
}

static void
break_depends(Plisthead *impacthead, Pkglist *pimpact)
{
	Pkglist	   	*rmimpact;
	Plisthead	*rdphead, *fdphead;
	Pkglist	   	*rdp, *fdp;
	char		*pkgname, *rpkg;
	int			dep_break, exists;

	rdphead = init_head();

	XSTRDUP(pkgname, pimpact->old);
	trunc_str(pkgname, '-', STR_BACKWARD);

	/* fetch old package reverse dependencies */
	full_dep_tree(pkgname, LOCAL_REVERSE_DEPS, rdphead);

	XFREE(pkgname);

	/* browse reverse dependencies */
	SLIST_FOREACH(rdp, rdphead, next) {

		exists = 0;
		/* check if rdp was already on impact list */
		SLIST_FOREACH(rmimpact, impacthead, next)
			if (strcmp(rmimpact->depend, rdp->depend) == 0) {
				exists = 1;
				break;
			}
		if (exists)
			continue;

		fdphead = init_head();

		/* reverse dependency is a full package name, use it and strip it */
		XSTRDUP(rpkg, rdp->depend);
		trunc_str(rpkg, '-', STR_BACKWARD);

		/* fetch dependencies for rdp */
		full_dep_tree(rpkg, DIRECT_DEPS, fdphead);

		/* initialize to broken dependency */
		dep_break = 1;

		/* empty full dep tree, this can't happen in normal situation.
		 * If it does, that means that the reverse dependency we're analyzing
		 * has no direct dependency.
		 * Such a situation could occur if the reverse dependency is not on
		 * the repository anymore, leading to no information regarding this
		 * package.
		 * So we will check if local package dependencies are satisfied by
		 * our newly upgraded packages.
		 */
		if (SLIST_EMPTY(fdphead)) {
			free_pkglist(&fdphead, DEPTREE);
			fdphead = init_head();
			full_dep_tree(rpkg, LOCAL_DIRECT_DEPS, fdphead);
		}
		XFREE(rpkg);

		/*
		 * browse dependencies for rdp and see if
		 * new package to be installed matches
		 */
		SLIST_FOREACH(fdp, fdphead, next)
			if (pkg_match(fdp->depend, pimpact->full)) {
				dep_break = 0;
				break;
			}

		free_pkglist(&fdphead, DEPTREE);

		if (!dep_break)
			continue;

		/* dependency break, insert rdp in remove-list */
		rmimpact = malloc_pkglist(IMPACT);
		XSTRDUP(rmimpact->depend, rdp->depend);
		XSTRDUP(rmimpact->full, rdp->depend);
		XSTRDUP(rmimpact->old, rdp->depend);
		rmimpact->action = TOREMOVE;
		rmimpact->level = 0;

		SLIST_INSERT_HEAD(impacthead, rmimpact, next);
	}
	free_pkglist(&rdphead, DEPTREE);
}

/**
 * loop through local packages and match for upgrades
 */
static int
deps_impact(Plisthead *impacthead, Pkglist *pdp)
{
	int			toupgrade;
	Pkglist		*pimpact, *plist, *mapplist;
	char		*remotepkg;

	/* local package list is empty */
	if (SLIST_EMPTY(&l_plisthead))
		return 1;

	/* record corresponding package on remote list*/
	if ((mapplist = map_pkg_to_dep(&r_plisthead, pdp->depend)) == NULL)
		return 1; /* no corresponding package in list */

	XSTRDUP(remotepkg, mapplist->full);

	/* create initial impact entry with a DONOTHING status, permitting
	 * to check if this dependency has already been recorded
	 */
	pimpact = malloc_pkglist(IMPACT);

	XSTRDUP(pimpact->depend, pdp->depend);

	pimpact->action = DONOTHING;
	pimpact->old = NULL;
	pimpact->full = NULL;

	SLIST_INSERT_HEAD(impacthead, pimpact, next);

	/* parse local packages to see if depedency is installed*/
	SLIST_FOREACH(plist, &l_plisthead, next) {

		/* match, package is installed */
		if (strcmp(plist->name, pdp->name) == 0) {

			/* default action when local package match */
			toupgrade = TOUPGRADE;

			/* installed version does not match dep requirement
			 * OR force reinstall, pkgkeep being use to inform -F was given
			 */
			if (!pkg_match(pdp->depend, plist->full) || pdp->keep < 0) {

				/* local pkgname didn't match deps, remote pkg has a
				 * lesser version than local package.
				*/
				if (version_check(plist->full, remotepkg) == 1) {
					/*
					 * proposing a downgrade is definitely not useful,
					 * not sure what I want to do with this...
					 */
						toupgrade = DONOTHING;

						return 1;
				}

				/* insert as an upgrade */
				/* oldpkg is used when building removal order list */
				XSTRDUP(pimpact->old, plist->full);

				pimpact->action = toupgrade;

				pimpact->full = remotepkg;
				/* record package dependency deepness */
				pimpact->level = pdp->level;
				/* record binary package size */
				pimpact->file_size = mapplist->file_size;
				/* record installed package size */
				pimpact->size_pkg = mapplist->size_pkg;

				/* does this upgrade break depedencies ? (php-4 -> php-5) */
				break_depends(impacthead, pimpact);
			}

			return 1;
		} /* if installed package match */

		/*
		 * check if another local package with option matches
		 * dependency, i.e. libflashsupport-pulse, ghostscript-esp...
		 * would probably lead to conflict if recorded, pass.
		 */
		if (pkg_match(pdp->depend, plist->full))
			return 1;

	} /* SLIST_FOREACH plist */

	if (!dep_present(impacthead, pdp->name)) {
		pimpact->old = NULL;
		pimpact->action = TOINSTALL;

		pimpact->full = remotepkg;
		/* record package dependency deepness */
		pimpact->level = pdp->level;

		pimpact->file_size = mapplist->file_size;
		pimpact->size_pkg = mapplist->size_pkg;
	}

	return 1;
}

/**
 * is pkgname already in impact list ?
 */
uint8_t
pkg_in_impact(Plisthead *impacthead, char *depname)
{
	Pkglist *pimpact;

	SLIST_FOREACH(pimpact, impacthead, next) {
		if (strcmp(pimpact->depend, depname) == 0)
			return 1;
	}

	return 0;
}

Plisthead *
pkg_impact(char **pkgargs)
{
#ifndef DEBUG
	static char	*icon = ICON_WAIT;
#endif
	Plisthead	*impacthead, *pdphead = NULL;
	Pkglist		*pimpact, *tmpimpact, *pdp;
	char		**ppkgargs, *pkgname;
#ifndef DEBUG
	char		tmpicon;
#endif

	if (SLIST_EMPTY(&r_plisthead)) {
		printf("%s\n", MSG_EMPTY_AVAIL_PKGLIST);
		return NULL;
	}

	impacthead = init_head();

	/* retreive impact list for all packages listed in the command line */
	for (ppkgargs = pkgargs; *ppkgargs != NULL; ppkgargs++) {

		if (strpbrk(*ppkgargs, "*%") != NULL) /* avoid SQL jokers */
			continue;

		/* check if this is a multiple-version package (apache, ...)
		 * and that the wanted package actually exists. Get pkgname
		 * from unique_pkg, full package format.
		 */
		if ((pkgname = unique_pkg(*ppkgargs, REMOTE_PKG)) == NULL) {
			/* package is not available on the repository */
			printf(MSG_PKG_NOT_AVAIL, *ppkgargs);
			continue;
		}

#ifndef DEBUG
		tmpicon = *icon++;
		printf(MSG_CALCULATING_DEPS" %c", tmpicon);
		fflush(stdout);
		if (*icon == '\0')
			icon = icon - strlen(ICON_WAIT);
#else
		printf(MSG_CALCULATING_DEPS, pkgname);
#endif
		pdphead = init_head();
		/* dependencies discovery */
		full_dep_tree(pkgname, DIRECT_DEPS, pdphead);

		/* parse dependencies for pkgname */
		SLIST_FOREACH(pdp, pdphead, next) {
		
			/* is dependency already recorded in impact list ? */
			if (pkg_in_impact(impacthead, pdp->depend))
				continue;
		
			/* compare needed deps with local packages */
			if (!deps_impact(impacthead, pdp)) {
				/* there was a versionning mismatch, proceed ? */
				if (!check_yesno(DEFAULT_NO)) {
					free_pkglist(&impacthead, IMPACT);
				
					goto impactend; /* avoid free's repetition */
				}
			}
		} /* SLIST_FOREACH deps */
		free_pkglist(&pdphead, DEPTREE);

		/* finally, insert package itself */	
		pdp = malloc_pkglist(DEPTREE);
	
		XSTRDUP(pdp->name, pkgname);
		trunc_str(pdp->name, '-', STR_BACKWARD);
	
		/* pkgname is not already recorded */
		if (!pkg_in_impact(impacthead, pkgname)) {
			/* passing pkgname as depname */
			XSTRDUP(pdp->depend, pkgname);
		
			/* reset pkgkeep */
			pdp->keep = 0;
		
			if (force_reinstall)
				/* use pkgkeep field to inform deps_impact the package
				 * has to be reinstalled. It is NOT the normal use for
				 * the pkgkeep field, it is just used as a temporary field
				 */
				pdp->keep = -1;

			deps_impact(impacthead, pdp);

			XFREE(pdp->depend);
		}

		XFREE(pdp->name);
		XFREE(pdp);

		XFREE(pkgname);

	} /* for (ppkgargs) */

#ifndef DEBUG
	printf(MSG_CALCULATING_DEPS" done.\n");
#endif

impactend:

	free_pkglist(&pdphead, DEPTREE);

	/* remove DONOTHING entries */
	SLIST_FOREACH_MUTABLE(pimpact, impacthead, next, tmpimpact) {
		if (pimpact->action == DONOTHING) {

			SLIST_REMOVE(impacthead, pimpact, Pkglist, next);

			free_pkglist_entry(&pimpact, IMPACT);
		}
	} /* SLIST_FOREACH_MUTABLE impacthead */

	/* no more impact, empty list */
	if (SLIST_EMPTY(impacthead))
		free_pkglist(&impacthead, IMPACT);

	return impacthead;
}
