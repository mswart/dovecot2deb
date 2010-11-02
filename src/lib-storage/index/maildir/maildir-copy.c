/* Copyright (c) 2002-2010 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "array.h"
#include "ioloop.h"
#include "str.h"
#include "nfs-workarounds.h"
#include "maildir-storage.h"
#include "maildir-uidlist.h"
#include "maildir-filename.h"
#include "maildir-keywords.h"
#include "maildir-sync.h"
#include "index-mail.h"
#include "mail-copy.h"

#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>

struct hardlink_ctx {
	string_t *dest_path;
	const char *dest_fname;
	unsigned int base_end_pos;

	unsigned int size_set:1;
	unsigned int vsize_set:1;
	unsigned int success:1;
	unsigned int preserve_filename:1;
};

static int do_save_mail_size(struct maildir_mailbox *mbox, const char *path,
			     struct hardlink_ctx *ctx)
{
	const char *fname, *str;
	struct stat st;
	uoff_t size;

	fname = strrchr(path, '/');
	fname = fname != NULL ? fname + 1 : path;

	if (!maildir_filename_get_size(fname, MAILDIR_EXTRA_FILE_SIZE,
				       &size)) {
		if (stat(path, &st) < 0) {
			if (errno == ENOENT)
				return 0;
			mail_storage_set_critical(&mbox->storage->storage,
						  "stat(%s) failed: %m", path);
			return -1;
		}
		size = st.st_size;
	}

	str = t_strdup_printf(",%c=%"PRIuUOFF_T, MAILDIR_EXTRA_FILE_SIZE, size);
	str_insert(ctx->dest_path, ctx->base_end_pos, str);

	ctx->dest_fname = strrchr(str_c(ctx->dest_path), '/') + 1;
	ctx->size_set = TRUE;
	return 1;
}

static void do_save_mail_vsize(const char *path, struct hardlink_ctx *ctx)
{
	const char *fname, *str;
	uoff_t size;

	fname = strrchr(path, '/');
	fname = fname != NULL ? fname + 1 : path;

	if (!maildir_filename_get_size(fname, MAILDIR_EXTRA_VIRTUAL_SIZE,
				       &size))
		return;

	str = t_strdup_printf(",%c=%"PRIuUOFF_T,
			      MAILDIR_EXTRA_VIRTUAL_SIZE, size);
	str_insert(ctx->dest_path, ctx->base_end_pos, str);

	ctx->dest_fname = strrchr(str_c(ctx->dest_path), '/') + 1;
	ctx->vsize_set = TRUE;
}

static int do_hardlink(struct maildir_mailbox *mbox, const char *path,
		       struct hardlink_ctx *ctx)
{
	int ret;

	if (!ctx->preserve_filename) {
		if (!ctx->size_set) {
			if ((ret = do_save_mail_size(mbox, path, ctx)) <= 0)
				return ret;
		}
		/* set virtual size if it's in the original file name */
		if (!ctx->vsize_set)
			do_save_mail_vsize(path, ctx);
	}

	if ((mbox->storage->storage.flags &
	     MAIL_STORAGE_FLAG_NFS_FLUSH_STORAGE) != 0)
		ret = nfs_safe_link(path, str_c(ctx->dest_path), FALSE);
	else
		ret = link(path, str_c(ctx->dest_path));
	if (ret < 0) {
		if (errno == ENOENT)
			return 0;

		if (ENOSPACE(errno)) {
			mail_storage_set_error(&mbox->storage->storage,
				MAIL_ERROR_NOSPACE, MAIL_ERRSTR_NO_SPACE);
			return -1;
		}

		/* we could handle the EEXIST condition by changing the
		   filename, but it practically never happens so just fallback
		   to standard copying for the rare cases when it does. */
		if (errno == EACCES || ECANTLINK(errno) || errno == EEXIST)
			return 1;

		mail_storage_set_critical(&mbox->storage->storage,
					  "link(%s, %s) failed: %m",
					  path, str_c(ctx->dest_path));
		return -1;
	}

	ctx->success = TRUE;
	return 1;
}

static const char *
maildir_copy_get_preserved_fname(struct maildir_mailbox *src_mbox,
				 struct maildir_mailbox *dest_mbox,
				 uint32_t uid)
{
	enum maildir_uidlist_rec_flag flags;
	const char *fname;

	/* see if the filename exists in destination maildir's
	   uidlist. if it doesn't, we can use it. otherwise generate
	   a new filename. FIXME: There's a race condition here if
	   another process is just doing the same copy. */
	if (maildir_uidlist_lookup(src_mbox->uidlist, uid, &flags,
				   &fname) <= 0)
		return NULL;

	if (maildir_uidlist_refresh(dest_mbox->uidlist) <= 0)
		return NULL;
	if (maildir_uidlist_get_full_filename(dest_mbox->uidlist,
					      fname) != NULL) {
		/* already exists in destination */
		return NULL;
	}
	/* fname may be freed by a later uidlist sync. make sure it gets
	   strduped. */
	return t_strcut(t_strdup(fname), ':');
}

static int
maildir_copy_hardlink(struct maildir_transaction_context *t, struct mail *mail,
		      enum mail_flags flags, struct mail_keywords *keywords,
		      struct mail *dest_mail, struct hardlink_ctx *do_ctx)
{
	struct maildir_mailbox *dest_mbox =
		(struct maildir_mailbox *)t->ictx.ibox;
	struct maildir_mailbox *src_mbox;
	const char *path, *filename = NULL;

	i_assert((t->ictx.flags & MAILBOX_TRANSACTION_FLAG_EXTERNAL) != 0);

	if (strcmp(mail->box->storage->name, MAILDIR_STORAGE_NAME) == 0)
		src_mbox = (struct maildir_mailbox *)mail->box;
	else if (strcmp(mail->box->storage->name, "raw") == 0) {
		/* deliver uses raw format */
		src_mbox = NULL;
	} else {
		/* Can't hard link files from the source storage */
		return 0;
	}

	if (t->save_ctx == NULL)
		t->save_ctx = maildir_save_transaction_init(t);

	/* don't allow caller to specify recent flag */
	flags &= ~MAIL_RECENT;
	if (dest_mbox->ibox.keep_recent)
		flags |= MAIL_RECENT;

	do_ctx->dest_path = str_new(default_pool, 512);

	if (dest_mbox->storage->copy_preserve_filename && src_mbox != NULL) {
		filename = maildir_copy_get_preserved_fname(src_mbox, dest_mbox,
							    mail->uid);
	}
	if (filename == NULL) {
		/* the generated filename is _always_ unique, so we don't
		   bother trying to check if it already exists */
		do_ctx->dest_fname = maildir_filename_generate();
	} else {
		do_ctx->dest_fname = filename;
		do_ctx->preserve_filename = TRUE;
	}

	/* FIXME: We could hardlink the files directly to destination, but
	   that would require checking if someone else had already assigned
	   UIDs for them after we have the uidlist locked. Index would also
	   need to be properly not-updated somehow.. */
#if 0
	if (keywords == NULL || keywords->count == 0) {
		/* no keywords, hardlink directly to destination */
		if (flags == MAIL_RECENT) {
			str_printfa(do_ctx->dest_path, "%s/new/%s",
				    dest_mbox->path, do_ctx->dest_fname);
			do_ctx->base_end_pos = str_len(do_ctx->dest_path);
		} else {
			str_printfa(do_ctx->dest_path, "%s/cur/",
				    dest_mbox->path);
			do_ctx->base_end_pos = str_len(do_ctx->dest_path) +
				strlen(do_ctx->dest_fname);
			str_append(do_ctx->dest_path,
				   maildir_filename_set_flags(NULL,
							      do_ctx->dest_fname,
							      flags, NULL));
		}
	} else
#endif
	{
		/* keywords, hardlink to tmp/ with basename and later when we
		   have uidlist locked, move it to new/cur. */
		str_printfa(do_ctx->dest_path, "%s/tmp/%s",
			    dest_mbox->path, do_ctx->dest_fname);
		do_ctx->base_end_pos = str_len(do_ctx->dest_path);
	}
	if (src_mbox != NULL) {
		/* maildir */
		if (maildir_file_do(src_mbox, mail->uid,
				    do_hardlink, do_ctx) < 0)
			return -1;
	} else {
		/* raw / deliver */
		if (mail_get_special(mail, MAIL_FETCH_UIDL_FILE_NAME,
				     &path) < 0 || *path == '\0')
			return 0;
		if (do_hardlink(dest_mbox, path, do_ctx) < 0)
			return -1;
	}

	if (!do_ctx->success) {
		/* couldn't copy with hardlinking, fallback to copying */
		return 0;
	}

#if 0
	if (keywords == NULL || keywords->count == 0) {
		/* hardlinked to destination, set hardlinked-flag */
		maildir_save_add(t, do_ctx->dest_fname,
				 flags | MAILDIR_SAVE_FLAG_HARDLINK, NULL,
				 dest_mail);
	} else
#endif
{
		/* hardlinked to tmp/, treat as normal copied mail */
		maildir_save_add(t, do_ctx->dest_fname, flags, keywords,
				 dest_mail);
	}
	return 1;
}

static bool
maildir_compatible_file_modes(struct mailbox *box1, struct mailbox *box2)
{
	return box1->file_create_mode == box2->file_create_mode &&
		box1->file_create_gid == box2->file_create_gid;
}

int maildir_copy(struct mail_save_context *ctx, struct mail *mail)
{
	struct maildir_transaction_context *t =
		(struct maildir_transaction_context *)ctx->transaction;
	struct maildir_mailbox *mbox = (struct maildir_mailbox *)t->ictx.ibox;
	struct hardlink_ctx do_ctx;
	int ret;

	if (mbox->storage->copy_with_hardlinks &&
	    maildir_compatible_file_modes(&mbox->ibox.box, mail->box)) {
		T_BEGIN {
			memset(&do_ctx, 0, sizeof(do_ctx));
			ret = maildir_copy_hardlink(t, mail, ctx->flags,
						    ctx->keywords,
						    ctx->dest_mail, &do_ctx);
			if (do_ctx.dest_path != NULL)
				str_free(&do_ctx.dest_path);
		} T_END;

		if (ret != 0) {
			index_save_context_free(ctx);
			return ret > 0 ? 0 : -1;
		}

		/* non-fatal hardlinking failure, try the slow way */
	}

	return mail_storage_copy(ctx, mail);
}