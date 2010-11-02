/* Copyright (c) 2006-2010 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "ioloop.h"
#include "array.h"
#include "str.h"
#include "dict.h"
#include "mail-namespace.h"
#include "index-mail.h"
#include "index-storage.h"
#include "expire-env.h"
#include "expire-plugin.h"

#include <stdlib.h>

#define EXPIRE_CONTEXT(obj) \
	MODULE_CONTEXT(obj, expire_storage_module)
#define EXPIRE_MAIL_CONTEXT(obj) \
	MODULE_CONTEXT(obj, expire_mail_module)

struct expire {
	struct dict *db;
	struct expire_env *env;

	void (*next_hook_mail_storage_created)(struct mail_storage *storage);
};

struct expire_mailbox {
	union mailbox_module_context module_ctx;
	time_t expire_secs;
	unsigned int altmove:1;
};

struct expire_transaction_context {
	union mailbox_transaction_module_context module_ctx;

	unsigned int saves:1;
	unsigned int first_expunged:1;
};

const char *expire_plugin_version = PACKAGE_VERSION;

static bool expire_debug;
static struct expire expire;
static MODULE_CONTEXT_DEFINE_INIT(expire_storage_module,
				  &mail_storage_module_register);
static MODULE_CONTEXT_DEFINE_INIT(expire_mail_module, &mail_module_register);

static struct mailbox_transaction_context *
expire_mailbox_transaction_begin(struct mailbox *box,
				 enum mailbox_transaction_flags flags)
{
	struct expire_mailbox *xpr_box = EXPIRE_CONTEXT(box);
	struct mailbox_transaction_context *t;
	struct expire_transaction_context *xt;

	t = xpr_box->module_ctx.super.transaction_begin(box, flags);
	xt = i_new(struct expire_transaction_context, 1);

	MODULE_CONTEXT_SET(t, expire_storage_module, xt);
	return t;
}

static void first_nonexpunged_timestamp(struct mailbox_transaction_context *_t,
					time_t *stamp_r)
{
	struct index_transaction_context *t =
		(struct index_transaction_context *)_t;
	struct mail_index_view *view = t->trans_view;
	const struct mail_index_header *hdr;
	struct mail *mail;
	uint32_t seq;

	mail = mail_alloc(_t, 0, NULL);

	/* find the first non-expunged mail. we're here because the first
	   mail was expunged, so don't bother checking it. */
	hdr = mail_index_get_header(view);
	for (seq = 2; seq <= hdr->messages_count; seq++) {
		if (!mail_index_is_expunged(view, seq)) {
			mail_set_seq(mail, seq);
			if (mail_get_save_date(mail, stamp_r) == 0)
				break;
		}
	}
	mail_free(&mail);

	if (seq > hdr->messages_count) {
		/* everything expunged */
		*stamp_r = 0;
	}
}

static int
expire_mailbox_transaction_commit(struct mailbox_transaction_context *t,
				  uint32_t *uid_validity_r,
				  uint32_t *first_saved_uid_r,
				  uint32_t *last_saved_uid_r)
{
	struct expire_mailbox *xpr_box = EXPIRE_CONTEXT(t->box);
	struct expire_transaction_context *xt = EXPIRE_CONTEXT(t);
	struct mailbox *box = t->box;
	time_t new_stamp;
	bool update_dict = FALSE;
	int ret;

	if (xpr_box->altmove) {
		/* only moving mails - don't update the move stamps */
	} else if (xt->first_expunged) {
		/* first mail expunged. dict needs updating. */
		first_nonexpunged_timestamp(t, &new_stamp);
		update_dict = TRUE;
	}

	if (xpr_box->module_ctx.super.
	    	transaction_commit(t, uid_validity_r,
				   first_saved_uid_r, last_saved_uid_r) < 0) {
		i_free(xt);
		return -1;
	}
	/* transaction is freed now */
	t = NULL;

	if (xt->first_expunged || xt->saves) T_BEGIN {
		const char *key, *value;
		string_t *vname = t_str_new(128);

		mail_namespace_get_vname(box->storage->ns->user->namespaces,
					 vname, box->name);
		key = t_strconcat(DICT_EXPIRE_PREFIX,
				  box->storage->ns->user->username, "/",
				  str_c(vname), NULL);
		if (xt->first_expunged) {
			if (new_stamp == 0 && xt->saves)
				new_stamp = ioloop_time;
		} else {
			i_assert(xt->saves);
			/* saved new mails. dict needs to be updated only if
			   this is the first mail in the database */
			ret = dict_lookup(expire.db, pool_datastack_create(),
					  key, &value);
			update_dict = ret == 0 ||
				(ret > 0 && strtoul(value, NULL, 10) == 0);
			/* may not be exactly the first message's save time
			   but a few second difference doesn't matter */
			new_stamp = ioloop_time;
		}

		if (update_dict) {
			struct dict_transaction_context *dctx;

			dctx = dict_transaction_begin(expire.db);
			if (new_stamp == 0) {
				/* everything expunged */
				dict_unset(dctx, key);
			} else {
				new_stamp += xpr_box->expire_secs;
				dict_set(dctx, key, dec2str(new_stamp));
			}
			dict_transaction_commit(&dctx);
		}
	} T_END;
	i_free(xt);
	return 0;
}

static void
expire_mailbox_transaction_rollback(struct mailbox_transaction_context *t)
{
	struct expire_mailbox *xpr_box = EXPIRE_CONTEXT(t->box);
	struct expire_transaction_context *xt = EXPIRE_CONTEXT(t);

	xpr_box->module_ctx.super.transaction_rollback(t);
	i_free(xt);
}

static void expire_mail_expunge(struct mail *_mail)
{
	struct mail_private *mail = (struct mail_private *)_mail;
	union mail_module_context *xpr_mail = EXPIRE_MAIL_CONTEXT(mail);
	struct expire_transaction_context *xt =
		EXPIRE_CONTEXT(_mail->transaction);

	if (_mail->seq == 1) {
		/* first mail expunged, database needs to be updated */
		xt->first_expunged = TRUE;
	}
	xpr_mail->super.expunge(_mail);
}

static struct mail *
expire_mail_alloc(struct mailbox_transaction_context *t,
		  enum mail_fetch_field wanted_fields,
		  struct mailbox_header_lookup_ctx *wanted_headers)
{
	struct expire_mailbox *xpr_box = EXPIRE_CONTEXT(t->box);
	union mail_module_context *xpr_mail;
	struct mail *_mail;
	struct mail_private *mail;

	_mail = xpr_box->module_ctx.super.
		mail_alloc(t, wanted_fields, wanted_headers);
	mail = (struct mail_private *)_mail;

	xpr_mail = p_new(mail->pool, union mail_module_context, 1);
	xpr_mail->super = mail->v;

	mail->v.expunge = expire_mail_expunge;
	MODULE_CONTEXT_SET_SELF(mail, expire_mail_module, xpr_mail);
	return _mail;
}

static int expire_save_finish(struct mail_save_context *ctx)
{
	struct expire_transaction_context *xt =
		EXPIRE_CONTEXT(ctx->transaction);
	struct expire_mailbox *xpr_box = EXPIRE_CONTEXT(ctx->transaction->box);

	xt->saves = TRUE;
	return xpr_box->module_ctx.super.save_finish(ctx);
}

static int
expire_copy(struct mail_save_context *ctx, struct mail *mail)
{
	struct expire_transaction_context *xt =
		EXPIRE_CONTEXT(ctx->transaction);
	struct expire_mailbox *xpr_box = EXPIRE_CONTEXT(ctx->transaction->box);

	xt->saves = TRUE;
	return xpr_box->module_ctx.super.copy(ctx, mail);
}

static void
mailbox_expire_hook(struct mailbox *box, time_t expire_secs, bool altmove)
{
	struct expire_mailbox *xpr_box;

	xpr_box = p_new(box->pool, struct expire_mailbox, 1);
	xpr_box->module_ctx.super = box->v;

	box->v.transaction_begin = expire_mailbox_transaction_begin;
	box->v.transaction_commit = expire_mailbox_transaction_commit;
	box->v.transaction_rollback = expire_mailbox_transaction_rollback;
	box->v.mail_alloc = expire_mail_alloc;
	box->v.save_finish = expire_save_finish;
	box->v.copy = expire_copy;

	xpr_box->altmove = altmove;
	xpr_box->expire_secs = expire_secs;

	MODULE_CONTEXT_SET(box, expire_storage_module, xpr_box);
}

static struct mailbox *
expire_mailbox_open(struct mail_storage *storage, const char *name,
		    struct istream *input, enum mailbox_open_flags flags)
{
	union mail_storage_module_context *xpr_storage =
		EXPIRE_CONTEXT(storage);
	struct mailbox *box;
	string_t *vname;
	unsigned int secs;
	bool altmove;

	box = xpr_storage->super.mailbox_open(storage, name, input, flags);
	if (box == NULL)
		return NULL;

	vname = t_str_new(128);
	(void)mail_namespace_get_vname(storage->ns, vname, name);

	secs = expire_box_find_min_secs(expire.env, str_c(vname), &altmove);
	if (expire_debug) {
		if (secs == 0) {
			i_info("expire: No expiring in mailbox: %s",
			       str_c(vname));
		} else {
			i_info("expire: Mails expire in %u secs in mailbox: %s",
			       secs, str_c(vname));
		}
	}
	if (secs != 0)
		mailbox_expire_hook(box, secs, altmove);
	return box;
}

static void expire_mail_storage_created(struct mail_storage *storage)
{
	union mail_storage_module_context *xpr_storage;

	xpr_storage =
		p_new(storage->pool, union mail_storage_module_context, 1);
	xpr_storage->super = storage->v;
	storage->v.mailbox_open = expire_mailbox_open;

	MODULE_CONTEXT_SET_SELF(storage, expire_storage_module, xpr_storage);

	if (expire.next_hook_mail_storage_created != NULL)
		expire.next_hook_mail_storage_created(storage);
}

void expire_plugin_init(void)
{
	const char *expunge_env, *altmove_env, *dict_uri, *base_dir;

	expire_debug = getenv("DEBUG") != NULL;
	expunge_env = getenv("EXPIRE");
	altmove_env = getenv("EXPIRE_ALTMOVE");
	if (expunge_env != NULL || altmove_env != NULL) {
		dict_uri = getenv("EXPIRE_DICT");
		if (dict_uri == NULL)
			i_fatal("expire plugin: expire_dict setting missing");

		expire.env = expire_env_init(expunge_env, altmove_env);
		/* we're using only shared dictionary, the username
		   doesn't matter. */
		base_dir = getenv("BASE_DIR");
		if (base_dir == NULL)
			base_dir = PKG_RUNDIR;
		expire.db = dict_init(dict_uri, DICT_DATA_TYPE_UINT32, "",
				      base_dir);
		if (expire.db == NULL)
			i_fatal("expire plugin: dict_init() failed");

		expire.next_hook_mail_storage_created =
			hook_mail_storage_created;
		hook_mail_storage_created = expire_mail_storage_created;
	} else if (expire_debug && getenv("EXPIRE_TOOL_BINARY") == NULL) {
		i_info("expire: No expire or expire_altmove settings - "
		       "plugin disabled");
	}
}

void expire_plugin_deinit(void)
{
	if (expire.db != NULL) {
		hook_mail_storage_created =
			expire.next_hook_mail_storage_created;

		dict_deinit(&expire.db);
		expire_env_deinit(expire.env);
	}
}