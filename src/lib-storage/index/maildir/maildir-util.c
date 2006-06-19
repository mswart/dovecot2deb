/* Copyright (C) 2004 Timo Sirainen */

#include "lib.h"
#include "hostpid.h"
#include "ioloop.h"
#include "str.h"
#include "maildir-storage.h"
#include "maildir-uidlist.h"
#include "maildir-keywords.h"

#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

static int maildir_file_do_try(struct maildir_mailbox *mbox, uint32_t uid,
			       maildir_file_do_func *func, void *context)
{
	const char *fname, *path;
        enum maildir_uidlist_rec_flag flags;
	int ret;

	fname = maildir_uidlist_lookup(mbox->uidlist, uid, &flags);
	if (fname == NULL)
		return -2; /* expunged */

	t_push();
	if ((flags & MAILDIR_UIDLIST_REC_FLAG_NEW_DIR) != 0) {
		/* probably in new/ dir */
		path = t_strconcat(mbox->path, "/new/", fname, NULL);
		ret = func(mbox, path, context);
		if (ret != 0) {
			t_pop();
			return ret;
		}
	}

	path = t_strconcat(mbox->path, "/cur/", fname, NULL);
	ret = func(mbox, path, context);
	t_pop();
	return ret;
}

int maildir_file_do(struct maildir_mailbox *mbox, uint32_t uid,
		    maildir_file_do_func *func, void *context)
{
	int i, ret;

	ret = maildir_file_do_try(mbox, uid, func, context);
	for (i = 0; i < 10 && ret == 0; i++) {
		/* file is either renamed or deleted. sync the maildir and
		   see which one. if file appears to be renamed constantly,
		   don't try to open it more than 10 times. */
		if (maildir_storage_sync_force(mbox) < 0)
			return -1;

		ret = maildir_file_do_try(mbox, uid, func, context);
	}

	if (i == 10) {
		ret = -1;
		mail_storage_set_critical(STORAGE(mbox->storage),
			"maildir_file_do(%s) racing", mbox->path);
	}

	return ret == -2 ? 0 : ret;
}

const char *maildir_generate_tmp_filename(const struct timeval *tv)
{
	static unsigned int create_count = 0;
	static time_t first_stamp = 0;

	if (first_stamp == 0 || first_stamp == ioloop_time) {
		/* it's possible that within last second another process had
		   the same PID as us. Use usecs to make sure we don't create
		   duplicate base name. */
		first_stamp = ioloop_time;
		return t_strdup_printf("%s.P%sQ%uM%s.%s",
				       dec2str(tv->tv_sec), my_pid,
				       create_count++,
				       dec2str(tv->tv_usec), my_hostname);
	} else {
		/* Don't bother with usecs. Saves a bit space :) */
		return t_strdup_printf("%s.P%sQ%u.%s",
				       dec2str(tv->tv_sec), my_pid,
				       create_count++, my_hostname);
	}
}

int maildir_create_tmp(struct maildir_mailbox *mbox, const char *dir,
		       mode_t mode, const char **fname_r)
{
	const char *path, *tmp_fname;
	struct stat st;
	struct timeval *tv, tv_now;
	pool_t pool;
	int fd;

	tv = &ioloop_timeval;
	pool = pool_alloconly_create("maildir_tmp", 4096);
	for (;;) {
		p_clear(pool);
		tmp_fname = maildir_generate_tmp_filename(tv);

		path = p_strconcat(pool, dir, "/", tmp_fname, NULL);
		if (stat(path, &st) < 0 && errno == ENOENT) {
			/* doesn't exist */
			mode_t old_mask = umask(0);
			fd = open(path, O_WRONLY | O_CREAT | O_EXCL, mode);
			umask(old_mask);
			if (fd != -1 || errno != EEXIST)
				break;
		}

		/* wait and try again - very unlikely */
		sleep(2);
		tv = &tv_now;
		if (gettimeofday(&tv_now, NULL) < 0)
			i_fatal("gettimeofday(): %m");
	}

	*fname_r = t_strdup(path);
	if (fd == -1) {
		if (ENOSPACE(errno)) {
			mail_storage_set_error(STORAGE(mbox->storage),
					       "Not enough disk space");
		} else {
			mail_storage_set_critical(STORAGE(mbox->storage),
						  "open(%s) failed: %m", path);
		}
	}

	pool_unref(pool);
	return fd;
}

bool maildir_filename_get_size(const char *fname, char type, uoff_t *size_r)
{
	uoff_t size = 0;

	for (; *fname != '\0'; fname++) {
		if (*fname == ',' && fname[1] == type && fname[2] == '=') {
			fname += 3;
			break;
		}
	}

	if (*fname == '\0')
		return FALSE;

	while (*fname >= '0' && *fname <= '9') {
		size = size * 10 + (*fname - '0');
		fname++;
	}

	if (*fname != MAILDIR_INFO_SEP &&
	    *fname != MAILDIR_EXTRA_SEP &&
	    *fname != '\0')
		return FALSE;

	*size_r = size;
	return TRUE;
}

/* a char* hash function from ASU -- from glib */
unsigned int maildir_hash(const void *p)
{
        const unsigned char *s = p;
	unsigned int g, h = 0;

	while (*s != MAILDIR_INFO_SEP && *s != '\0') {
		h = (h << 4) + *s;
		if ((g = h & 0xf0000000UL)) {
			h = h ^ (g >> 24);
			h = h ^ g;
		}

		s++;
	}

	return h;
}

int maildir_cmp(const void *p1, const void *p2)
{
	const char *s1 = p1, *s2 = p2;

	while (*s1 == *s2 && *s1 != MAILDIR_INFO_SEP && *s1 != '\0') {
		s1++; s2++;
	}
	if ((*s1 == '\0' || *s1 == MAILDIR_INFO_SEP) &&
	    (*s2 == '\0' || *s2 == MAILDIR_INFO_SEP))
		return 0;
	return *s1 - *s2;
}
