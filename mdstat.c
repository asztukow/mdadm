/*
 * mdstat - parse /proc/mdstat file. Part of:
 * mdadm - manage Linux "md" devices aka RAID arrays.
 *
 * Copyright (C) 2002-2009 Neil Brown <neilb@suse.de>
 *
 *
 *    This program is free software; you can redistribute it and/or modify
 *    it under the terms of the GNU General Public License as published by
 *    the Free Software Foundation; either version 2 of the License, or
 *    (at your option) any later version.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU General Public License for more details.
 *
 *    You should have received a copy of the GNU General Public License
 *    along with this program; if not, write to the Free Software
 *    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 *    Author: Neil Brown
 *    Email: <neilb@suse.de>
 */

/*
 * The /proc/mdstat file comes in at least 3 flavours:
 * In an unpatched 2.2 kernel (md 0.36.6):
 *  Personalities : [n raidx] ...
 *  read_ahead {not set|%d sectors}
 *  md0 : {in}active{ raidX /dev/hda...  %d blocks{ maxfault=%d}}
 *  md1 : .....
 *
 * Normally only 4 md lines, but all are listed.
 *
 * In a patched 2.2 kernel (md 0.90.0)
 *  Personalities : [raidx] ...
 *  read_ahead {not set|%d sectors}
 *  mdN : {in}active {(readonly)} raidX dev[%d]{(F)} ... %d blocks STATUS RESYNC
 *  ... Only initialised arrays listed
 *  unused devices: {dev dev ... | <none>}
 *
 * STATUS is personality dependant:
 *    linear:  %dk rounding
 *    raid0:   %dk chunks
 *    raid1:   [%d/%d] [U_U]   ( raid/working.  operational or not)
 *    raid5:   level 4/5, %dk chunk, algorithm %d [%d/%d] [U_U]
 *
 * RESYNC is  empty or:
 *    {resync|recovery}=%u%% finish=%u.%umin
 *  or
 *    resync=DELAYED
 *
 * In a 2.4 kernel (md 0.90.0/2.4)
 *  Personalities : [raidX] ...
 *  read_ahead {not set|%d sectors}
 *  mdN : {in}active {(read-only)} raidX dev[%d]{(F)} ...
 *       %d blocks STATUS
 *       RESYNC
 *  unused devices: {dev dev .. | <none>}
 *
 *  STATUS matches 0.90.0/2.2
 *  RESYNC includes [===>....],
 *          adds a space after {resync|recovery} and before and after '='
 *          adds a decimal to the recovery percent.
 *	    adds (%d/%d) resync amount  and max_blocks, before finish.
 *	    adds speed=%dK/sec after finish
 *
 *
 *
 * Out of this we want to extract:
 *   list of devices, active or not
 *   pattern of failed drives (so need number of drives)
 *   percent resync complete
 *
 * As continuation is indicated by leading space, we use
 *  conf_line from config.c to read logical lines
 *
 */

#include	"mdadm.h"
#include	"dlink.h"
#include	"xmalloc.h"

#include	<sys/select.h>
#include	<ctype.h>

static void free_member_devnames(struct dev_member *m)
{
	while(m) {
		struct dev_member *t = m;

		m = m->next;
		free(t->name);
		free(t);
	}
}

static int add_member_devname(struct dev_member **m, char *name)
{
	struct dev_member *new;
	char *t;

	if ((t = strchr(name, '[')) == NULL)
		/* not a device */
		return 0;

	new = xmalloc(sizeof(*new));
	new->name = strndup(name, t - name);
	new->next = *m;
	*m = new;
	return 1;
}

/* Detach element from the list, it may modify list_head */
static void mdstat_ent_list_detach_element(struct mdstat_ent **list_head, struct mdstat_ent *el)
{
	struct mdstat_ent *ent = *list_head;

	if (ent == el) {
		*list_head = ent->next;
	} else {
		while (ent) {
			if (ent->next == el) {
				ent->next = el->next;
				break;
			}

			ent = ent->next;
		}

	}

	/* Guard if not found or list is empty - not allowed */
	assert(ent);
	el->next = NULL;
}

void free_mdstat(struct mdstat_ent *ms)
{
	while (ms) {
		struct mdstat_ent *t;
		free(ms->level);
		free(ms->pattern);
		free(ms->metadata_version);
		free_member_devnames(ms->members);
		t = ms;
		ms = ms->next;
		free(t);
	}
}

bool is_mdstat_ent_external(struct mdstat_ent *ent)
{
	if (!ent->metadata_version)
		return false;

	if (strncmp(ent->metadata_version, "external:", 9) == 0)
		return true;
	return false;
}

bool is_mdstat_ent_subarray(struct mdstat_ent *ent)
{
	if (is_mdstat_ent_external(ent) && is_subarray(ent->metadata_version + 9))
		return true;
	return false;
}

bool is_container_member(struct mdstat_ent *mdstat, char *container)
{
	if (is_mdstat_ent_external(mdstat) &&
	    metadata_container_matches(mdstat->metadata_version + 9, container))
		return true;

	return false;
}

static int mdstat_fd = -1;
struct mdstat_ent *mdstat_read(int hold, int start)
{
	FILE *f;
	struct mdstat_ent *all, *rv, **end, **insert_here;
	char *line;
	int fd;

	if (hold && mdstat_fd != -1) {
		off_t offset = lseek(mdstat_fd, 0L, 0);
		if (offset == (off_t)-1) {
			return NULL;
		}
		fd = dup(mdstat_fd);
		if (fd >= 0)
			f = fdopen(fd, "r");
		else
			return NULL;
	} else
		f = fopen("/proc/mdstat", "r");
	if (f == NULL)
		return NULL;

	if (fcntl(fileno(f), F_SETFD, FD_CLOEXEC) < 0) {
		fclose(f);
		return NULL;
	}

	all = NULL;
	end = &all;
	for (; (line = conf_line(f)) ; free_line(line)) {
		struct mdstat_ent *ent;
		char *w;
		char devnm[32];
		int in_devs = 0;

		if (strcmp(line, "Personalities") == 0)
			continue;
		if (strcmp(line, "read_ahead") == 0)
			continue;
		if (strcmp(line, "unused") == 0)
			continue;
		insert_here = NULL;
		/* Better be an md line.. */
		if (strncmp(line, "md", 2)!= 0 || strlen(line) >= 32 ||
		    (line[2] != '_' && !isdigit(line[2])))
			continue;
		strcpy(devnm, line);

		ent = xmalloc(sizeof(*ent));
		ent->level = ent->pattern= NULL;
		ent->next = NULL;
		ent->percent = RESYNC_NONE;
		ent->active = -1;
		ent->resync = 0;
		ent->metadata_version = NULL;
		ent->raid_disks = 0;
		ent->devcnt = 0;
		ent->members = NULL;

		strcpy(ent->devnm, devnm);

		for (w=dl_next(line); w!= line ; w=dl_next(w)) {
			int l = strlen(w);
			char *eq;
			if (strcmp(w, "active") == 0)
				ent->active = 1;
			else if (strcmp(w, "inactive") == 0) {
				ent->active = 0;
				in_devs = 1;
			} else if (strcmp(w, "bitmap:") == 0) {
				/* We need to stop parsing here;
				 * otherwise, ent->raid_disks will be
				 * overwritten by the wrong value.
				 */
				break;
			} else if (ent->active > 0 &&
				 ent->level == NULL &&
				 w[0] != '(' /*readonly*/) {
				ent->level = xstrdup(w);
				in_devs = 1;
			} else if (in_devs && strcmp(w, "blocks") == 0)
				in_devs = 0;
			else if (in_devs) {
				char *ep = strchr(w, '[');
				ent->devcnt +=
					add_member_devname(&ent->members, w);
				if (ep && strncmp(w, "md", 2) == 0) {
					/* This has an md device as a component.
					 * If that device is already in the
					 * list, make sure we insert before
					 * there.
					 */
					struct mdstat_ent **ih;
					ih = &all;
					while (ih != insert_here && *ih &&
					       ((int)strlen((*ih)->devnm) !=
						ep-w ||
						strncmp((*ih)->devnm, w,
							ep-w) != 0))
						ih = & (*ih)->next;
					insert_here = ih;
				}
			} else if (strcmp(w, "super") == 0 &&
				   dl_next(w) != line) {
				w = dl_next(w);
				ent->metadata_version = xstrdup(w);
			} else if (w[0] == '[' && isdigit(w[1])) {
				ent->raid_disks = atoi(w+1);
			} else if (!ent->pattern &&
				   w[0] == '[' &&
				   (w[1] == 'U' || w[1] == '_')) {
				ent->pattern = xstrdup(w+1);
				if (ent->pattern[l-2] == ']')
					ent->pattern[l-2] = '\0';
			} else if (ent->percent == RESYNC_NONE &&
				   strncmp(w, "re", 2) == 0 &&
				   w[l-1] == '%' &&
				   (eq = strchr(w, '=')) != NULL ) {
				ent->percent = atoi(eq+1);
				if (strncmp(w,"resync", 6) == 0)
					ent->resync = 1;
				else if (strncmp(w, "reshape", 7) == 0)
					ent->resync = 2;
				else
					ent->resync = 0;
			} else if (ent->percent == RESYNC_NONE &&
				   (w[0] == 'r' || w[0] == 'c')) {
				if (strncmp(w, "resync", 6) == 0)
					ent->resync = 1;
				if (strncmp(w, "reshape", 7) == 0)
					ent->resync = 2;
				if (strncmp(w, "recovery", 8) == 0)
					ent->resync = 0;
				if (strncmp(w, "check", 5) == 0)
					ent->resync = 3;

				if (l > 8 && strcmp(w+l-8, "=DELAYED") == 0)
					ent->percent = RESYNC_DELAYED;
				if (l > 8 && strcmp(w+l-8, "=PENDING") == 0)
					ent->percent = RESYNC_PENDING;
				if (l > 7 && strcmp(w+l-7, "=REMOTE") == 0)
					ent->percent = RESYNC_REMOTE;
			} else if (ent->percent == RESYNC_NONE &&
				   w[0] >= '0' &&
				   w[0] <= '9' &&
				   w[l-1] == '%') {
				ent->percent = atoi(w);
			}
		}
		if (insert_here && (*insert_here)) {
			ent->next = *insert_here;
			*insert_here = ent;
		} else {
			*end = ent;
			end = &ent->next;
		}
	}
	if (hold && mdstat_fd == -1) {
		mdstat_fd = dup(fileno(f));
		if (fcntl(mdstat_fd, F_SETFD, FD_CLOEXEC) < 0) {
			fclose(f);
			return NULL;
		}
	}
	fclose(f);

	/* If we might want to start array,
	 * reverse the order, so that components comes before composites
	 */
	if (start) {
		rv = NULL;
		while (all) {
			struct mdstat_ent *e = all;
			all = all->next;
			e->next = rv;
			rv = e;
		}
	} else
		rv = all;
	return rv;
}

void mdstat_close(void)
{
	if (mdstat_fd >= 0)
		close(mdstat_fd);
	mdstat_fd = -1;
}

/*
 * function: mdstat_wait
 * Description: Function waits for event on mdstat.
 * Parameters:
 *		seconds - timeout for waiting
 * Returns:
 *		> 0 - detected event
 *		0 - timeout
 *		< 0 - detected error
 */
int mdstat_wait(int seconds)
{
	fd_set fds;
	struct timeval tm;
	int maxfd = 0;
	FD_ZERO(&fds);
	if (mdstat_fd >= 0) {
		FD_SET(mdstat_fd, &fds);
		maxfd = mdstat_fd;
	} else
		return -1;

	tm.tv_sec = seconds;
	tm.tv_usec = 0;

	return select(maxfd + 1, NULL, NULL, &fds, &tm);
}

void mdstat_wait_fd(int fd, const sigset_t *sigmask)
{
	fd_set fds, rfds;
	int maxfd = 0;

	FD_ZERO(&fds);
	FD_ZERO(&rfds);
	if (mdstat_fd >= 0)
		FD_SET(mdstat_fd, &fds);

	if (fd >= 0) {
		struct stat stb;
		if (fstat(fd, &stb) != 0)
			return;
		if ((stb.st_mode & S_IFMT) == S_IFREG)
			/* Must be a /proc or /sys fd, so expect
			 * POLLPRI
			 * i.e. an 'exceptional' event.
			 */
			FD_SET(fd, &fds);
		else
			FD_SET(fd, &rfds);

		if (fd > maxfd)
			maxfd = fd;

	}
	if (mdstat_fd > maxfd)
		maxfd = mdstat_fd;

	pselect(maxfd + 1, &rfds, NULL, &fds,
		NULL, sigmask);
}

int mddev_busy(char *devnm)
{
	struct mdstat_ent *mdstat = mdstat_read(0, 0);
	struct mdstat_ent *me;

	for (me = mdstat ; me ; me = me->next)
		if (strcmp(me->devnm, devnm) == 0)
			break;
	free_mdstat(mdstat);
	return me != NULL;
}

/**
 * mdstat_find_by_member_devnm()- Return first array or external container with member device.
 * @mdstat: Preloaded mdstat to iterate over.
 * @member_devnm: devnm of the device to find.
 *
 * External subarrays are skipped.
 */
struct mdstat_ent *mdstat_find_by_member_name(struct mdstat_ent *mdstat, char *member_devnm)
{
	struct mdstat_ent *ent;

	for (ent = mdstat; ent; ent = ent->next) {
		struct dev_member *member;

		if (is_mdstat_ent_subarray(ent))
			continue;

		for (member = ent->members; member; member = member->next)
			if (strcmp(member->name, member_devnm) == 0)
				return ent;
	}

	return NULL;
}


struct mdstat_ent *mdstat_by_component(char *name)
{
	struct mdstat_ent *mdstat = mdstat_read(0, 0);
	struct mdstat_ent *ent = mdstat_find_by_member_name(mdstat, name);

	if (ent)
		mdstat_ent_list_detach_element(&mdstat, ent);

	free_mdstat(mdstat);

	return ent;
}

struct mdstat_ent *mdstat_by_subdev(char *subdev, char *container)
{
	struct mdstat_ent *mdstat = mdstat_read(0, 0);
	struct mdstat_ent *ent = NULL;

	for (ent = mdstat; ent; ent = ent->next) {
		/* metadata version must match:
		 *   external:[/-]%s/%s
		 * where first %s is 'container' and second %s is 'subdev'
		 */

		if (!is_mdstat_ent_external(ent))
			continue;

		if (!metadata_container_matches(ent->metadata_version + 9, container))
			continue;
		if (!metadata_subdev_matches(ent->metadata_version + 9, subdev))
			continue;

		break;
	}

	if (ent)
		mdstat_ent_list_detach_element(&mdstat, ent);

	free_mdstat(mdstat);
	return ent;
}
