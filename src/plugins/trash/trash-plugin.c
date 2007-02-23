/* Copyright (C) 2005 Timo Sirainen */

#include "lib.h"
#include "array.h"
#include "istream.h"
#include "home-expand.h"
#include "mail-search.h"
#include "quota-private.h"
#include "quota-plugin.h"
#include "trash-plugin.h"

#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>

#define INIT_TRASH_MAILBOX_COUNT 4
#define MAX_RETRY_COUNT 3

#define TRASH_CONTEXT(obj) \
	*((void **)array_idx_modifyable(&(obj)->quota_module_contexts, \
					trash_quota_module_id))

struct trash_quota_root {
	struct quota_backend_vfuncs super;
};

struct trash_mailbox {
	const char *name;
	int priority; /* lower number = higher priority */

	struct mail_storage *storage;

	/* temporarily set while cleaning: */
	struct mailbox *box;
	struct mailbox_transaction_context *trans;
        struct mail_search_arg search_arg;
	struct mail_search_context *search_ctx;
	struct mail *mail;

	unsigned int mail_set:1;
};

/* defined by imap, pop3, lda */
extern void (*hook_quota_root_created)(struct quota_root *root);

const char *trash_plugin_version = PACKAGE_VERSION;

static void (*trash_next_hook_quota_root_created)(struct quota_root *root);
static unsigned int trash_quota_module_id = 0;
static bool trash_quota_module_id_set = FALSE;

static pool_t config_pool;
/* trash_boxes ordered by priority, highest first */
static array_t ARRAY_DEFINE(trash_boxes, struct trash_mailbox);

static int sync_mailbox(struct mailbox *box)
{
	struct mailbox_sync_context *ctx;
        struct mailbox_sync_rec sync_rec;
        struct mailbox_status status;

	ctx = mailbox_sync_init(box, MAILBOX_SYNC_FLAG_FULL_READ);
	while (mailbox_sync_next(ctx, &sync_rec) > 0)
		;
	return mailbox_sync_deinit(&ctx, &status);
}

static int trash_clean_mailbox_open(struct trash_mailbox *trash)
{
	trash->box = mailbox_open(trash->storage, trash->name, NULL,
				  MAILBOX_OPEN_KEEP_RECENT);
	if (trash->box == NULL)
		return 0;

	if (sync_mailbox(trash->box) < 0)
		return -1;

	trash->trans = mailbox_transaction_begin(trash->box, 0);

	trash->search_ctx = mailbox_search_init(trash->trans, NULL,
						&trash->search_arg, NULL);
	trash->mail = mail_alloc(trash->trans, MAIL_FETCH_PHYSICAL_SIZE |
				 MAIL_FETCH_RECEIVED_DATE, NULL);

	return mailbox_search_next(trash->search_ctx, trash->mail);
}

static int trash_clean_mailbox_get_next(struct trash_mailbox *trash,
					time_t *received_time_r)
{
	int ret;

	if (!trash->mail_set) {
		if (trash->box == NULL)
			ret = trash_clean_mailbox_open(trash);
		else
			ret = mailbox_search_next(trash->search_ctx,
						  trash->mail);
		if (ret <= 0)
			return ret;

		trash->mail_set = TRUE;
	}

	*received_time_r = mail_get_received_date(trash->mail);
	return 1;
}

static int trash_try_clean_mails(struct quota_root_transaction_context *ctx,
				 uint64_t size_needed)
{
	struct trash_mailbox *trashes;
	unsigned int i, j, count, oldest_idx;
	time_t oldest, received;
	uint64_t size, size_expunged = 0, expunged_count = 0;
	int ret = 0;

	trashes = array_get_modifyable(&trash_boxes, &count);
	for (i = 0; i < count; ) {
		/* expunge oldest mails first in all trash boxes with
		   same priority */
		oldest_idx = count;
		oldest = (time_t)-1;
		for (j = i; j < count; j++) {
			if (trashes[j].priority != trashes[i].priority)
				break;

			if (trashes[j].storage == NULL) {
				/* FIXME: this is really ugly. it'll do however
				   until we get proper namespace support for
				   lib-storage. */
				struct mail_storage *const *storage;

				storage = array_idx(&ctx->root->storages, 0);
				trashes[j].storage = *storage;
			}

			ret = trash_clean_mailbox_get_next(&trashes[j],
							   &received);
			if (ret < 0)
				goto __err;
			if (ret > 0) {
				if (oldest == (time_t)-1 ||
				    received < oldest) {
					oldest = received;
					oldest_idx = j;
				}
			}
		}

		if (oldest_idx < count) {
			size = mail_get_physical_size(trashes[oldest_idx].mail);
			if (size == (uoff_t)-1) {
				/* maybe expunged already? */
				trashes[oldest_idx].mail_set = FALSE;
				continue;
			}

			if (mail_expunge(trashes[oldest_idx].mail) < 0)
				break;

			expunged_count++;
			size_expunged += size;
			if (size_expunged >= size_needed)
				break;
			trashes[oldest_idx].mail_set = FALSE;
		} else {
			/* find more mails from next priority's mailbox */
			i = j;
		}
	}

__err:
	for (i = 0; i < count; i++) {
		struct trash_mailbox *trash = &trashes[i];

		if (trash->box == NULL)
			continue;

		trash->mail_set = FALSE;
		mail_free(&trash->mail);
		(void)mailbox_search_deinit(&trash->search_ctx);

		if (size_expunged >= size_needed) {
			(void)mailbox_transaction_commit(&trash->trans,
				MAILBOX_SYNC_FLAG_FULL_WRITE);
		} else {
			/* couldn't get enough space, don't expunge anything */
                        mailbox_transaction_rollback(&trash->trans);
		}

		mailbox_close(&trash->box);
	}

	if (size_expunged < size_needed)
		return FALSE;

	ctx->bytes_current = ctx->bytes_current > size_expunged ?
		ctx->bytes_current - size_expunged : 0;
	ctx->count_current = ctx->count_current > expunged_count ?
		ctx->count_current - expunged_count : 0;
	return TRUE;
}

static int
trash_quota_root_try_alloc(struct quota_root_transaction_context *ctx,
			   struct mail *mail, bool *too_large_r)
{
	struct trash_quota_root *troot = TRASH_CONTEXT(ctx->root);
	int ret, i;

	for (i = 0; ; i++) {
		ret = troot->super.try_alloc(ctx, mail, too_large_r);
		if (ret != 0 || *too_large_r)
			return ret;

		if (i == MAX_RETRY_COUNT) {
			/* trash_try_clean_mails() should have returned 0 if
			   it couldn't get enough space, but allow retrying
			   it a couple of times if there was some extra space
			   that was needed.. */
			break;
		}

		/* not enough space. try deleting some from mailbox. */
		ret = trash_try_clean_mails(ctx, mail_get_physical_size(mail));
		if (ret <= 0)
			return 0;
	}

	return 0;
}

static int
trash_quota_root_try_alloc_bytes(struct quota_root_transaction_context *ctx,
				 uoff_t size, bool *too_large_r)
{
	struct trash_quota_root *troot = TRASH_CONTEXT(ctx->root);
	int ret, i;

	for (i = 0; ; i++) {
		ret = troot->super.try_alloc_bytes(ctx, size, too_large_r);
		if (ret != 0 || *too_large_r)
			return ret;

		if (i == MAX_RETRY_COUNT)
			break;

		/* not enough space. try deleting some from mailbox. */
		ret = trash_try_clean_mails(ctx, size);
		if (ret <= 0)
			return 0;
	}

	return 0;
}

static int
trash_quota_root_test_alloc_bytes(struct quota_root_transaction_context *ctx,
				  uoff_t size, bool *too_large_r)
{
	struct trash_quota_root *troot = TRASH_CONTEXT(ctx->root);
	int ret, i;

	for (i = 0; ; i++) {
		ret = troot->super.test_alloc_bytes(ctx, size, too_large_r);
		if (ret != 0 || *too_large_r)
			return ret;

		if (i == MAX_RETRY_COUNT)
			break;

		/* not enough space. try deleting some from mailbox. */
		ret = trash_try_clean_mails(ctx, size);
		if (ret <= 0)
			return 0;
	}

	return 0;
}

static void trash_quota_root_deinit(struct quota_root *root)
{
	struct trash_quota_root *troot = TRASH_CONTEXT(root);
	void *null = NULL;

	array_idx_set(&root->quota_module_contexts,
		      trash_quota_module_id, &null);
	troot->super.deinit(root);
	i_free(troot);
}

static void trash_quota_root_created(struct quota_root *root)
{
	struct trash_quota_root *troot;

	if (trash_next_hook_quota_root_created != NULL)
		trash_next_hook_quota_root_created(root);

	troot = i_new(struct trash_quota_root, 1);
	troot->super = root->v;
	root->v.deinit = trash_quota_root_deinit;
	root->v.try_alloc = trash_quota_root_try_alloc;
	root->v.try_alloc_bytes = trash_quota_root_try_alloc_bytes;
	root->v.test_alloc_bytes = trash_quota_root_test_alloc_bytes;

	if (!trash_quota_module_id_set) {
		trash_quota_module_id_set = TRUE;
		trash_quota_module_id = quota_module_id++;
	}
	array_idx_set(&root->quota_module_contexts,
		      trash_quota_module_id, &troot);
}

static int trash_mailbox_priority_cmp(const void *p1, const void *p2)
{
	const struct trash_mailbox *t1 = p1, *t2 = p2;

	return t1->priority - t2->priority;
}

static int read_configuration(const char *path)
{
	struct istream *input;
	const char *line, *name;
	struct trash_mailbox *trash;
	int fd;

	fd = open(path, O_RDONLY);
	if (fd == -1) {
		i_error("open(%s) failed: %m", path);
		return -1;
	}

	p_clear(config_pool);
	ARRAY_CREATE(&trash_boxes, config_pool, struct trash_mailbox,
		     INIT_TRASH_MAILBOX_COUNT);

	input = i_stream_create_file(fd, default_pool, (size_t)-1, FALSE);
	while ((line = i_stream_read_next_line(input)) != NULL) {
		/* <priority> <mailbox name> */
		name = strchr(line, ' ');
		if (name == NULL || name[1] == '\0')
			continue;

		trash = array_append_space(&trash_boxes);
		trash->name = p_strdup(config_pool, name+1);
		trash->priority = atoi(t_strdup_until(line, name));
		trash->search_arg.type = SEARCH_ALL;
	}
	i_stream_destroy(&input);
	(void)close(fd);

	qsort(array_get_modifyable(&trash_boxes, NULL),
	      array_count(&trash_boxes), sizeof(struct trash_mailbox),
	      trash_mailbox_priority_cmp);
	return 0;
}

void trash_plugin_init(void)
{
	const char *env;

	trash_next_hook_quota_root_created = hook_quota_root_created;

	env = getenv("TRASH");
	if (env == NULL)
		return;

	if (quota_set == NULL) {
		i_error("trash plugin: quota plugin not initialized");
		return;
	}

	config_pool = pool_alloconly_create("trash config",
					sizeof(trash_boxes) +
					BUFFER_APPROX_SIZE +
					INIT_TRASH_MAILBOX_COUNT *
					(sizeof(struct trash_mailbox) + 32));
	if (read_configuration(env) < 0)
		return;

	hook_quota_root_created = trash_quota_root_created;
}

void trash_plugin_deinit(void)
{
	if (config_pool != NULL)
		pool_unref(config_pool);

	hook_quota_root_created = trash_next_hook_quota_root_created;
}
