/* Copyright (C) 2002-2004 Timo Sirainen */

#include "lib.h"
#include "array.h"
#include "ioloop.h"
#include "str.h"
#include "maildir-storage.h"
#include "maildir-uidlist.h"
#include "maildir-keywords.h"
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
			mail_storage_set_critical(STORAGE(mbox->storage),
						  "stat(%s) failed: %m", path);
			return -1;
		}
		size = st.st_size;
	}

	str = t_strdup_printf(",S=%"PRIuUOFF_T, size);
	str_insert(ctx->dest_path, ctx->base_end_pos, str);

	ctx->dest_fname = strrchr(str_c(ctx->dest_path), '/') + 1;
	ctx->size_set = TRUE;
	return 1;
}

static int do_hardlink(struct maildir_mailbox *mbox, const char *path,
		       void *context)
{
	struct hardlink_ctx *ctx = context;
	int ret;

	if (!ctx->preserve_filename && mbox->storage->save_size_in_filename &&
	    !ctx->size_set) {
		if ((ret = do_save_mail_size(mbox, path, ctx)) <= 0)
			return ret;
	}

	if (link(path, str_c(ctx->dest_path)) < 0) {
		if (errno == ENOENT)
			return 0;

		if (ENOSPACE(errno)) {
			mail_storage_set_error(STORAGE(mbox->storage),
					       "Not enough disk space");
			return -1;
		}

		/* we could handle the EEXIST condition by changing the
		   filename, but it practically never happens so just fallback
		   to standard copying for the rare cases when it does. */
		if (errno == EACCES || ECANTLINK(errno) || errno == EEXIST)
			return 1;

		mail_storage_set_critical(STORAGE(mbox->storage),
					  "link(%s, %s) failed: %m",
					  path, str_c(ctx->dest_path));
		return -1;
	}

	ctx->success = TRUE;
	return 1;
}

static int
maildir_copy_hardlink(struct maildir_transaction_context *t, struct mail *mail,
		      enum mail_flags flags, struct mail_keywords *keywords,
		      struct mail *dest_mail)
{
	struct maildir_mailbox *dest_mbox =
		(struct maildir_mailbox *)t->ictx.ibox;
	struct maildir_mailbox *src_mbox =
		(struct maildir_mailbox *)mail->box;
	struct maildir_save_context *ctx;
	struct hardlink_ctx do_ctx;
	const char *filename = NULL;
	uint32_t seq;

	i_assert((t->ictx.flags & MAILBOX_TRANSACTION_FLAG_EXTERNAL) != 0);

	if (t->save_ctx == NULL)
		t->save_ctx = maildir_save_transaction_init(t);
	ctx = t->save_ctx;

	/* don't allow caller to specify recent flag */
	flags &= ~MAIL_RECENT;
	if (dest_mbox->ibox.keep_recent)
		flags |= MAIL_RECENT;

	memset(&do_ctx, 0, sizeof(do_ctx));
	do_ctx.dest_path = str_new(default_pool, 512);

	if (dest_mbox->storage->copy_preserve_filename) {
		enum maildir_uidlist_rec_flag src_flags;
		const char *src_fname;

		/* see if the filename exists in destination maildir's
		   uidlist. if it doesn't, we can use it. otherwise generate
		   a new filename */
		src_fname = maildir_uidlist_lookup(src_mbox->uidlist,
						   mail->uid, &src_flags);
		if (src_fname != NULL &&
		    maildir_uidlist_update(dest_mbox->uidlist) >= 0 &&
		    maildir_uidlist_get_full_filename(dest_mbox->uidlist,
						      src_fname) == NULL)
			filename = t_strcut(src_fname, ':');
	}
	if (filename == NULL) {
		/* the generated filename is _always_ unique, so we don't
		   bother trying to check if it already exists */
		do_ctx.dest_fname =
			maildir_generate_tmp_filename(&ioloop_timeval);
	} else {
		do_ctx.dest_fname = filename;
		do_ctx.preserve_filename = TRUE;
	}

	if (keywords == NULL || keywords->count == 0) {
		/* no keywords, hardlink directly to destination */
		if (flags == MAIL_RECENT) {
			str_printfa(do_ctx.dest_path, "%s/new/%s",
				    dest_mbox->path, do_ctx.dest_fname);
			do_ctx.base_end_pos = str_len(do_ctx.dest_path);
		} else {
			str_printfa(do_ctx.dest_path, "%s/cur/",
				    dest_mbox->path);
			do_ctx.base_end_pos = str_len(do_ctx.dest_path) +
				strlen(do_ctx.dest_fname);
			str_append(do_ctx.dest_path,
				   maildir_filename_set_flags(NULL,
							      do_ctx.dest_fname,
							      flags, NULL));
		}
	} else {
		/* keywords, hardlink to tmp/ with basename and later when we
		   have uidlist locked, move it to new/cur. */
		str_printfa(do_ctx.dest_path, "%s/tmp/%s",
			    dest_mbox->path, do_ctx.dest_fname);
		do_ctx.base_end_pos = str_len(do_ctx.dest_path);
	}
	if (maildir_file_do(src_mbox, mail->uid, do_hardlink, &do_ctx) < 0)
		return -1;

	if (!do_ctx.success) {
		/* couldn't copy with hardlinking, fallback to copying */
		return 0;
	}

	if (keywords == NULL || keywords->count == 0) {
		/* hardlinked to destination, set hardlinked-flag */
		seq = maildir_save_add(t, do_ctx.dest_fname,
				       flags | MAILDIR_SAVE_FLAG_HARDLINK, NULL,
				       dest_mail != NULL);
	} else {
		/* hardlinked to tmp/, treat as normal copied mail */
		seq = maildir_save_add(t, do_ctx.dest_fname, flags, keywords,
				       dest_mail != NULL);
	}

	if (dest_mail != NULL) {
		i_assert(seq != 0);

		if (mail_set_seq(dest_mail, seq) < 0)
			return -1;
	}
	return 1;
}

int maildir_copy(struct mailbox_transaction_context *_t, struct mail *mail,
		 enum mail_flags flags, struct mail_keywords *keywords,
		 struct mail *dest_mail)
{
	struct maildir_transaction_context *t =
		(struct maildir_transaction_context *)_t;
	struct maildir_mailbox *mbox = (struct maildir_mailbox *)t->ictx.ibox;
	int ret;

	if (mbox->storage->copy_with_hardlinks &&
	    mail->box->storage == mbox->ibox.box.storage) {
		t_push();
		ret = maildir_copy_hardlink(t, mail, flags,
					    keywords, dest_mail);
		t_pop();

		if (ret > 0)
			return 0;
		if (ret < 0)
			return -1;

		/* non-fatal hardlinking failure, try the slow way */
	}

	return mail_storage_copy(_t, mail, flags, keywords, dest_mail);
}
