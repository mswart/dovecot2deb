/* Copyright (C) 2005-2006 Timo Sirainen */

/* Only for reporting filesystem quota */

#include "lib.h"
#include "array.h"
#include "str.h"
#include "mountpoint.h"
#include "quota-private.h"
#include "quota-fs.h"

#ifdef HAVE_FS_QUOTA

#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#ifdef HAVE_LINUX_DQBLK_XFS_H
#  include <linux/dqblk_xfs.h>
#  define HAVE_XFS_QUOTA
#elif defined (HAVE_XFS_XQM_H)
#  include <xfs/xqm.h> /* CentOS 4.x at least uses this */
#  define HAVE_XFS_QUOTA
#endif

#ifndef DEV_BSIZE
#  define DEV_BSIZE 512
#endif

#ifdef HAVE_STRUCT_DQBLK_CURSPACE
#  define dqb_curblocks dqb_curspace
#endif

/* Older sys/quota.h doesn't define _LINUX_QUOTA_VERSION at all, which means
   it supports only v1 quota */
#ifndef _LINUX_QUOTA_VERSION
#  define _LINUX_QUOTA_VERSION 1
#endif

struct fs_quota_mountpoint {
	char *mount_path;
	char *device_path;
	char *type;

#ifdef HAVE_Q_QUOTACTL
	int fd;
	char *path;
#endif
};

struct fs_quota_root {
	struct quota_root root;

	uid_t uid;
	struct fs_quota_mountpoint *mount;
};

struct fs_quota_root_iter {
	struct quota_root_iter iter;

	bool sent;
};

extern struct quota_backend quota_backend_fs;

static struct quota_root *
fs_quota_init(struct quota_setup *setup __attr_unused__, const char *name)
{
	struct fs_quota_root *root;

	root = i_new(struct fs_quota_root, 1);
	root->root.name = i_strdup(name);
	root->root.v = quota_backend_fs.v;
	root->uid = geteuid();

	return &root->root;
}

static void fs_quota_mountpoint_free(struct fs_quota_mountpoint *mount)
{
#ifdef HAVE_Q_QUOTACTL
	if (mount->fd != -1) {
		if (close(mount->fd) < 0)
			i_error("close(%s) failed: %m", mount->path);
	}
	i_free(mount->path);
#endif

	i_free(mount->device_path);
	i_free(mount->mount_path);
	i_free(mount->type);
	i_free(mount);
}

static void fs_quota_deinit(struct quota_root *_root)
{
	struct fs_quota_root *root = (struct fs_quota_root *)_root;

	if (root->mount != NULL)
		fs_quota_mountpoint_free(root->mount);
	i_free(root->root.name);
	i_free(root);
}

static struct fs_quota_mountpoint *fs_quota_mountpoint_get(const char *dir)
{
	struct fs_quota_mountpoint *mount;
	struct mountpoint point;
	int ret;

	ret = mountpoint_get(dir, default_pool, &point);
	if (ret <= 0)
		return NULL;

	mount = i_new(struct fs_quota_mountpoint, 1);
	mount->device_path = point.device_path;
	mount->mount_path = point.mount_path;
	mount->type = point.type;
	return mount;
}

static bool fs_quota_add_storage(struct quota_root *_root,
				 struct mail_storage *storage)
{
	struct fs_quota_root *root = (struct fs_quota_root *)_root;
	struct fs_quota_mountpoint *mount;
	const char *dir;
	bool is_file;

	dir = mail_storage_get_mailbox_path(storage, "", &is_file);

	if (getenv("DEBUG") != NULL)
		i_info("fs quota add storage dir = %s", dir);

	mount = fs_quota_mountpoint_get(dir);
	if (root->mount == NULL) {
		if (mount == NULL) {
			/* Not found */
			return TRUE;
		}
		root->mount = mount;
	} else {
		bool match = strcmp(root->mount->mount_path,
				    mount->mount_path) == 0;

		fs_quota_mountpoint_free(mount);
		if (!match) {
			/* different mountpoints, can't use this */
			return FALSE;
		}
		mount = root->mount;
	}

	if (getenv("DEBUG") != NULL) {
		i_info("fs quota block device = %s", mount->device_path);
		i_info("fs quota mount point = %s", mount->mount_path);
	}

#ifdef HAVE_Q_QUOTACTL
	if (mount->path == NULL) {
		mount->path = i_strconcat(mount->mount_path, "/quotas", NULL);
		mount->fd = open(mount->path, O_RDONLY);
		if (mount->fd == -1 && errno != ENOENT)
			i_error("open(%s) failed: %m", mount->path);
	}
#endif
	return TRUE;
}

static void
fs_quota_remove_storage(struct quota_root *root __attr_unused__,
			struct mail_storage *storage __attr_unused__)
{
}

static const char *const *
fs_quota_root_get_resources(struct quota_root *root __attr_unused__)
{
	static const char *resources[] = { QUOTA_NAME_STORAGE, NULL };

	return resources;
}

static int
fs_quota_get_resource(struct quota_root *_root, const char *name,
		      uint64_t *value_r, uint64_t *limit_r)
{
	struct fs_quota_root *root = (struct fs_quota_root *)_root;
	struct dqblk dqblk;
#ifdef HAVE_Q_QUOTACTL
	struct quotctl ctl;
#endif

	*value_r = 0;
	*limit_r = 0;

	if (strcasecmp(name, QUOTA_NAME_STORAGE) != 0 || root->mount == NULL)
		return 0;

#if defined (HAVE_QUOTACTL) && defined(HAVE_SYS_QUOTA_H)
	/* Linux */
#ifdef HAVE_XFS_QUOTA
	if (strcmp(root->mount->type, "xfs") == 0) {
		/* XFS */
		struct fs_disk_quota xdqblk;

		if (quotactl(QCMD(Q_XGETQUOTA, USRQUOTA),
			     root->mount->device_path,
			     root->uid, (caddr_t)&xdqblk) < 0) {
			i_error("quotactl(Q_XGETQUOTA, %s) failed: %m",
				root->mount->device_path);
			quota_set_error(_root->setup->quota,
					"Internal quota error");
			return -1;
		}

		/* values always returned in 512 byte blocks */
		*value_r = xdqblk.d_bcount >> 1;
		*limit_r = xdqblk.d_blk_softlimit >> 1;
	} else
#endif
	{
		/* ext2, ext3 */
		if (quotactl(QCMD(Q_GETQUOTA, USRQUOTA),
			     root->mount->device_path,
			     root->uid, (caddr_t)&dqblk) < 0) {
			i_error("quotactl(Q_GETQUOTA, %s) failed: %m",
				root->mount->device_path);
			if (errno == EINVAL) {
				i_error("Dovecot was compiled with Linux quota "
					"v%d support, try changing it "
					"(--with-linux-quota configure option)",
					_LINUX_QUOTA_VERSION);
			}
			quota_set_error(_root->setup->quota,
					"Internal quota error");
			return -1;
		}

		*value_r = dqblk.dqb_curblocks / 1024;
		*limit_r = dqblk.dqb_bsoftlimit;
	}
#elif defined(HAVE_QUOTACTL)
	/* BSD, AIX */
	if (quotactl(root->mount->mount_path, QCMD(Q_GETQUOTA, USRQUOTA),
		     root->uid, (void *)&dqblk) < 0) {
		i_error("quotactl(Q_GETQUOTA, %s) failed: %m",
			root->mount->mount_path);
		quota_set_error(_root->setup->quota, "Internal quota error");
		return -1;
	}
	*value_r = (uint64_t)dqblk.dqb_curblocks * 1024 / DEV_BSIZE;
	*limit_r = (uint64_t)dqblk.dqb_bsoftlimit * 1024 / DEV_BSIZE;
#else
	/* Solaris */
	if (root->mount->fd == -1)
		return 0;

	ctl.op = Q_GETQUOTA;
	ctl.uid = root->uid;
	ctl.addr = (caddr_t)&dqblk;
	if (ioctl(root->mount->fd, Q_QUOTACTL, &ctl) < 0) {
		i_error("ioctl(%s, Q_QUOTACTL) failed: %m", root->mount->path);
		quota_set_error(_root->setup->quota, "Internal quota error");
		return -1;
	}
	*value_r = (uint64_t)dqblk.dqb_curblocks * 1024 / DEV_BSIZE;
	*limit_r = (uint64_t)dqblk.dqb_bsoftlimit * 1024 / DEV_BSIZE;
#endif
	return 1;
}

static int
fs_quota_set_resource(struct quota_root *root,
		      const char *name __attr_unused__,
		      uint64_t value __attr_unused__)
{
	quota_set_error(root->setup->quota, MAIL_STORAGE_ERR_NO_PERMISSION);
	return -1;
}

static struct quota_root_transaction_context *
fs_quota_transaction_begin(struct quota_root *root,
			   struct quota_transaction_context *ctx,
			   struct mailbox *box __attr_unused__)
{
	struct quota_root_transaction_context *root_ctx;

	root_ctx = i_new(struct quota_root_transaction_context, 1);
	root_ctx->root = root;
	root_ctx->ctx = ctx;
	root_ctx->disabled = TRUE;
	return root_ctx;
}

static int
fs_quota_transaction_commit(struct quota_root_transaction_context *ctx)
{
	i_free(ctx);
	return 0;
}

struct quota_backend quota_backend_fs = {
	"fs",

	{
		fs_quota_init,
		fs_quota_deinit,

		fs_quota_add_storage,
		fs_quota_remove_storage,

		fs_quota_root_get_resources,

		fs_quota_get_resource,
		fs_quota_set_resource,

		fs_quota_transaction_begin,
		fs_quota_transaction_commit,
		quota_default_transaction_rollback,

		quota_default_try_alloc,
		quota_default_try_alloc_bytes,
		quota_default_test_alloc_bytes,
		quota_default_alloc,
		quota_default_free
	}
};

#endif
