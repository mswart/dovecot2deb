/* Copyright (c) 2005-2010 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "str.h"
#include "file-lock.h"
#include "mail-storage.h"
#include "mail-namespace.h"

#include <stdlib.h>

void (*hook_mail_namespaces_created)(struct mail_namespace *namespaces);

void mail_namespace_init_storage(struct mail_namespace *ns)
{
	ns->list = mail_storage_get_list(ns->storage);
	ns->prefix_len = strlen(ns->prefix);
	/* allow plugins to override real_sep */
	if (ns->real_sep == '\0')
		ns->real_sep = mailbox_list_get_hierarchy_sep(ns->list);

	if (ns->sep == '\0')
                ns->sep = ns->real_sep;

	if (ns->sep == '"' || ns->sep == '\\') {
		ns->sep_str[0] = '\\';
		ns->sep_str[1] = ns->sep;
	} else {
		ns->sep_str[0] = ns->sep;
	}
}

static void mail_namespace_free(struct mail_namespace *ns)
{
	if (ns->owner != ns->user && ns->owner != NULL)
		mail_user_unref(&ns->owner);
	i_free(ns->prefix);
	i_free(ns);
}

static struct mail_namespace *
namespace_add_env(const char *data, unsigned int num,
		  struct mail_user *user, enum mail_storage_flags flags,
		  enum file_lock_method lock_method,
		  struct mail_namespace *prev_namespaces)
{
        struct mail_namespace *ns;
	const char *sep, *type, *prefix, *driver, *error, *list, *alias_for;

	ns = i_new(struct mail_namespace, 1);
	ns->user = user;

	sep = getenv(t_strdup_printf("NAMESPACE_%u_SEP", num));
	type = getenv(t_strdup_printf("NAMESPACE_%u_TYPE", num));
	prefix = getenv(t_strdup_printf("NAMESPACE_%u_PREFIX", num));
	list = getenv(t_strdup_printf("NAMESPACE_%u_LIST", num));
	alias_for = getenv(t_strdup_printf("NAMESPACE_%u_ALIAS", num));
	if (getenv(t_strdup_printf("NAMESPACE_%u_INBOX", num)) != NULL)
		ns->flags |= NAMESPACE_FLAG_INBOX;
	if (getenv(t_strdup_printf("NAMESPACE_%u_HIDDEN", num)) != NULL)
		ns->flags |= NAMESPACE_FLAG_HIDDEN;
	if (list == NULL)
		list = "no";
	else {
		if (strcmp(list, "children") == 0)
			ns->flags |= NAMESPACE_FLAG_LIST_CHILDREN;
		else
			ns->flags |= NAMESPACE_FLAG_LIST_PREFIX;
	}
	if (getenv(t_strdup_printf("NAMESPACE_%u_SUBSCRIPTIONS", num)) != NULL)
		ns->flags |= NAMESPACE_FLAG_SUBSCRIPTIONS;

	if (type == NULL || *type == '\0' || strncmp(type, "private", 7) == 0) {
		ns->type = NAMESPACE_PRIVATE;
		ns->owner = user;
	} else if (strncmp(type, "shared", 6) == 0)
		ns->type = NAMESPACE_SHARED;
	else if (strncmp(type, "public", 6) == 0)
		ns->type = NAMESPACE_PUBLIC;
	else {
		i_error("Unknown namespace type: %s", type);
		mail_namespace_free(ns);
		return NULL;
	}

	if (alias_for != NULL) {
		ns->alias_for = mail_namespace_find_prefix(prev_namespaces,
							   alias_for);
		if (ns->alias_for == NULL) {
			i_error("Invalid namespace alias_for: %s", alias_for);
			mail_namespace_free(ns);
			return NULL;
		}
		if (ns->alias_for->alias_for != NULL) {
			i_error("Chained namespace alias_for: %s", alias_for);
			mail_namespace_free(ns);
			return NULL;
		}
		ns->alias_chain_next = ns->alias_for->alias_chain_next;
		ns->alias_for->alias_chain_next = ns;
	}

	if (prefix == NULL)
		prefix = "";

	if ((flags & MAIL_STORAGE_FLAG_DEBUG) != 0) {
		i_info("Namespace: type=%s, prefix=%s, sep=%s, "
		       "inbox=%s, hidden=%s, list=%s, subscriptions=%s",
		       type == NULL ? "" : type, prefix, sep == NULL ? "" : sep,
		       (ns->flags & NAMESPACE_FLAG_INBOX) ? "yes" : "no",
		       (ns->flags & NAMESPACE_FLAG_HIDDEN) ? "yes" : "no",
		       list,
		       (ns->flags & NAMESPACE_FLAG_SUBSCRIPTIONS) ?
		       "yes" : "no");
	}

	if (sep != NULL)
		ns->sep = *sep;
	ns->prefix = i_strdup(prefix);

	if (ns->type == NAMESPACE_SHARED && strchr(ns->prefix, '%') != NULL) {
		/* dynamic shared namespace */
		ns->flags |= NAMESPACE_FLAG_NOQUOTA | NAMESPACE_FLAG_NOACL;
		driver = "shared";
	} else {
		driver = NULL;
	}

	if (mail_storage_create(ns, driver, data, flags, lock_method,
				&error) < 0) {
		i_error("Namespace '%s': %s", ns->prefix, error);
		mail_namespace_free(ns);
		return NULL;
	}
	return ns;
}

static bool namespaces_check(struct mail_namespace *namespaces)
{
	struct mail_namespace *ns, *inbox_ns = NULL;
	unsigned int subscriptions_count = 0;
	char list_sep = '\0';

	for (ns = namespaces; ns != NULL; ns = ns->next) {
		if ((ns->flags & NAMESPACE_FLAG_INBOX) != 0) {
			if (inbox_ns != NULL) {
				i_error("namespace configuration error: "
					"There can be only one namespace with "
					"inbox=yes");
				return FALSE;
			}
			inbox_ns = ns;
		}
		if (*ns->prefix != '\0' &&
		    (ns->flags & (NAMESPACE_FLAG_LIST_PREFIX |
				  NAMESPACE_FLAG_LIST_CHILDREN)) != 0 &&
		    ns->prefix[strlen(ns->prefix)-1] != ns->sep) {
			i_error("namespace configuration error: "
				"list=yes requires prefix=%s "
				"to end with separator", ns->prefix);
			return FALSE;
		}
		if ((ns->flags & (NAMESPACE_FLAG_LIST_PREFIX |
				  NAMESPACE_FLAG_LIST_CHILDREN)) != 0) {
			if (list_sep == '\0')
				list_sep = ns->sep;
			else if (list_sep != ns->sep) {
				i_error("namespace configuration error: "
					"All list=yes namespaces must use "
					"the same separator");
				return FALSE;
			}
		}
		if ((ns->flags & NAMESPACE_FLAG_SUBSCRIPTIONS) != 0)
			subscriptions_count++;
	}

	if (inbox_ns == NULL) {
		i_error("namespace configuration error: "
			"inbox=yes namespace missing");
		return FALSE;
	}
	if (list_sep == '\0') {
		i_error("namespace configuration error: "
			"no list=yes namespaces");
		return FALSE;
	}
	if (subscriptions_count == 0) {
		i_error("namespace configuration error: "
			"no subscriptions=yes namespaces");
		return FALSE;
	}
	return TRUE;
}

int mail_namespaces_init(struct mail_user *user)
{
	struct mail_namespace *namespaces, *ns, **ns_p;
	enum mail_storage_flags flags;
        enum file_lock_method lock_method;
	const char *mail, *data, *error;
	unsigned int i;

	mail_storage_parse_env(&flags, &lock_method);
        namespaces = NULL; ns_p = &namespaces;

	/* first try NAMESPACE_* environments */
	for (i = 1; ; i++) {
		T_BEGIN {
			data = getenv(t_strdup_printf("NAMESPACE_%u", i));
		} T_END;

		if (data == NULL)
			break;

		T_BEGIN {
			*ns_p = namespace_add_env(data, i, user, flags,
						  lock_method, namespaces);
		} T_END;

		if (*ns_p == NULL)
			return -1;

		ns_p = &(*ns_p)->next;
	}

	if (namespaces != NULL) {
		if (!namespaces_check(namespaces)) {
			while (namespaces != NULL) {
				ns = namespaces;
				namespaces = ns->next;
				mail_namespace_free(ns);
			}
			return -1;
		}
		mail_user_add_namespace(user, &namespaces);

		if (hook_mail_namespaces_created != NULL) {
			T_BEGIN {
				hook_mail_namespaces_created(namespaces);
			} T_END;
		}
		return 0;
	}

	/* fallback to MAIL */
	mail = getenv("MAIL");
	if (mail == NULL) {
		/* support also maildir-specific environment */
		mail = getenv("MAILDIR");
		if (mail != NULL)
			mail = t_strconcat("maildir:", mail, NULL);
	}

	ns = i_new(struct mail_namespace, 1);
	ns->type = NAMESPACE_PRIVATE;
	ns->flags = NAMESPACE_FLAG_INBOX | NAMESPACE_FLAG_LIST_PREFIX |
		NAMESPACE_FLAG_SUBSCRIPTIONS;
	ns->prefix = i_strdup("");
	ns->user = user;
	ns->owner = user;

	if (mail_storage_create(ns, NULL, mail, flags, lock_method,
				&error) < 0) {
		if (mail != NULL && *mail != '\0')
			i_error("mail_location: %s", error);
		else {
			i_error("mail_location not set and "
				"autodetection failed: %s", error);
		}
		mail_namespace_free(ns);
		return -1;
	}
	user->namespaces = ns;

	if (hook_mail_namespaces_created != NULL) {
		T_BEGIN {
			hook_mail_namespaces_created(ns);
		} T_END;
	}
	return 0;
}

struct mail_namespace *
mail_namespaces_init_empty(struct mail_user *user)
{
	struct mail_namespace *ns;

	ns = i_new(struct mail_namespace, 1);
	ns->user = user;
	ns->owner = user;
	ns->prefix = i_strdup("");
	ns->flags = NAMESPACE_FLAG_INBOX | NAMESPACE_FLAG_LIST_PREFIX |
		NAMESPACE_FLAG_SUBSCRIPTIONS;
	user->namespaces = ns;
	return ns;
}

void mail_namespaces_deinit(struct mail_namespace **_namespaces)
{
	struct mail_namespace *ns, *namespaces = *_namespaces;

	*_namespaces = NULL;
	while (namespaces != NULL) {
		ns = namespaces;
		namespaces = namespaces->next;

		if (ns->storage != NULL)
			mail_storage_destroy(&ns->storage);
		mail_namespace_free(ns);
	}
}

void mail_namespace_destroy(struct mail_namespace *ns)
{
	struct mail_namespace **nsp;

	/* remove from user's namespaces list */
	for (nsp = &ns->user->namespaces; *nsp != NULL; nsp = &(*nsp)->next) {
		if (*nsp == ns) {
			*nsp = ns->next;
			break;
		}
	}

	if (ns->storage != NULL)
		mail_storage_destroy(&ns->storage);
	mail_namespace_free(ns);
}

const char *mail_namespace_fix_sep(struct mail_namespace *ns, const char *name)
{
	char *ret, *p;

	if (ns->sep == ns->real_sep)
		return name;
	if (ns->type == NAMESPACE_SHARED &&
	    (ns->flags & NAMESPACE_FLAG_AUTOCREATED) == 0) {
		/* shared namespace root. the backend storage's hierarchy
		   separator isn't known yet, so do nothing. */
		return name;
	}

	ret = p_strdup(unsafe_data_stack_pool, name);
	for (p = ret; *p != '\0'; p++) {
		if (*p == ns->sep)
			*p = ns->real_sep;
	}
	return ret;
}

const char *mail_namespace_get_vname(struct mail_namespace *ns, string_t *dest,
				     const char *name)
{
	str_truncate(dest, 0);
	if ((ns->flags & NAMESPACE_FLAG_INBOX) == 0 ||
	    strcasecmp(name, "INBOX") != 0 ||
	    ns->user != ns->owner)
		str_append(dest, ns->prefix);

	for (; *name != '\0'; name++) {
		if (*name == ns->real_sep)
			str_append_c(dest, ns->sep);
		else
			str_append_c(dest, *name);
	}
	return str_c(dest);
}

char mail_namespace_get_root_sep(const struct mail_namespace *namespaces)
{
	while ((namespaces->flags & NAMESPACE_FLAG_LIST_PREFIX) == 0)
		namespaces = namespaces->next;
	return namespaces->sep;
}

static bool mail_namespace_is_usable_prefix(struct mail_namespace *ns,
					    const char *mailbox, bool inbox)
{
	if (strncmp(ns->prefix, mailbox, ns->prefix_len) == 0) {
		/* true exact prefix match */
		return TRUE;
	}

	if (inbox && strncmp(ns->prefix, "INBOX", 5) == 0 &&
	    strncmp(ns->prefix+5, mailbox+5, ns->prefix_len-5) == 0) {
		/* we already checked that mailbox begins with case-insensitive
		   INBOX. this namespace also begins with INBOX and the rest
		   of the prefix matches too. */
		return TRUE;
	}

	if (strncmp(ns->prefix, mailbox, ns->prefix_len-1) == 0 &&
	    mailbox[ns->prefix_len-1] == '\0' &&
	    ns->prefix[ns->prefix_len-1] == ns->sep) {
		/* we're trying to access the namespace prefix itself */
		return TRUE;
	}
	return FALSE;
}

static struct mail_namespace *
mail_namespace_find_mask(struct mail_namespace *namespaces,
			 const char **mailbox,
			 enum namespace_flags flags,
			 enum namespace_flags mask)
{
        struct mail_namespace *ns = namespaces;
	const char *box = *mailbox;
	struct mail_namespace *best = NULL;
	unsigned int len, best_len = 0;
	bool inbox;

	inbox = strncasecmp(box, "INBOX", 5) == 0;
	if (inbox && box[5] == '\0') {
		/* find the INBOX namespace */
		*mailbox = "INBOX";
		while (ns != NULL) {
			if ((ns->flags & NAMESPACE_FLAG_INBOX) != 0 &&
			    (ns->flags & mask) == flags)
				return ns;
			if (*ns->prefix == '\0')
				best = ns;
			ns = ns->next;
		}
		return best;
	}

	for (; ns != NULL; ns = ns->next) {
		if (ns->prefix_len >= best_len && (ns->flags & mask) == flags &&
		    mail_namespace_is_usable_prefix(ns, box, inbox)) {
			best = ns;
			best_len = ns->prefix_len;
		}
	}

	if (best != NULL) {
		if (best_len > 0) {
			len = strlen(*mailbox);
			*mailbox += I_MIN(len, best_len);
		} else if (inbox && (box[5] == best->sep || box[5] == '\0'))
			*mailbox = t_strconcat("INBOX", box+5, NULL);

		*mailbox = mail_namespace_fix_sep(best, *mailbox);
	}
	return best;
}

struct mail_namespace *
mail_namespace_find(struct mail_namespace *namespaces, const char **mailbox)
{
	return mail_namespace_find_mask(namespaces, mailbox, 0, 0);
}

struct mail_namespace *
mail_namespace_find_visible(struct mail_namespace *namespaces,
			    const char **mailbox)
{
	return mail_namespace_find_mask(namespaces, mailbox, 0,
					NAMESPACE_FLAG_HIDDEN);
}

struct mail_namespace *
mail_namespace_find_subscribable(struct mail_namespace *namespaces,
				 const char **mailbox)
{
	return mail_namespace_find_mask(namespaces, mailbox,
					NAMESPACE_FLAG_SUBSCRIPTIONS,
					NAMESPACE_FLAG_SUBSCRIPTIONS);
}

struct mail_namespace *
mail_namespace_find_unsubscribable(struct mail_namespace *namespaces,
				   const char **mailbox)
{
	return mail_namespace_find_mask(namespaces, mailbox,
					0, NAMESPACE_FLAG_SUBSCRIPTIONS);
}

struct mail_namespace *
mail_namespace_find_inbox(struct mail_namespace *namespaces)
{
	while ((namespaces->flags & NAMESPACE_FLAG_INBOX) == 0)
		namespaces = namespaces->next;
	return namespaces;
}

bool mail_namespace_update_name(const struct mail_namespace *ns,
				const char **mailbox)
{
	struct mail_namespace tmp_ns = *ns;

	/* FIXME: a bit kludgy.. */
	tmp_ns.next = NULL;
	return mail_namespace_find_mask(&tmp_ns, mailbox, 0, 0) != NULL;
}

struct mail_namespace *
mail_namespace_find_prefix(struct mail_namespace *namespaces,
			   const char *prefix)
{
        struct mail_namespace *ns;
	unsigned int len = strlen(prefix);

	for (ns = namespaces; ns != NULL; ns = ns->next) {
		if (ns->prefix_len == len &&
		    strcmp(ns->prefix, prefix) == 0)
			return ns;
	}
	return NULL;
}

struct mail_namespace *
mail_namespace_find_prefix_nosep(struct mail_namespace *namespaces,
				 const char *prefix)
{
        struct mail_namespace *ns;
	unsigned int len = strlen(prefix);

	for (ns = namespaces; ns != NULL; ns = ns->next) {
		if (ns->prefix_len == len + 1 &&
		    strncmp(ns->prefix, prefix, len) == 0 &&
		    ns->prefix[len] == ns->sep)
			return ns;
	}
	return NULL;
}