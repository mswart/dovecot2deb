#ifndef USERDB_H
#define USERDB_H

#include "auth-stream.h"

struct auth_request;

enum userdb_result {
	USERDB_RESULT_INTERNAL_FAILURE = -1,
	USERDB_RESULT_USER_UNKNOWN = -2,

	USERDB_RESULT_OK = 1
};

typedef void userdb_callback_t(enum userdb_result result,
			       struct auth_request *request);

struct userdb_module {
	/* The caching key for this module, or NULL if caching isn't wanted. */
	const char *cache_key;

	/* If blocking is set to TRUE, use child processes to access
	   this userdb. */
	bool blocking;

	const struct userdb_module_interface *iface;
};

struct userdb_module_interface {
	const char *name;

	struct userdb_module *
		(*preinit)(struct auth_userdb *auth_userdb, const char *args);
	void (*init)(struct userdb_module *module, const char *args);
	void (*deinit)(struct userdb_module *module);

	void (*lookup)(struct auth_request *auth_request,
		       userdb_callback_t *callback);
};

uid_t userdb_parse_uid(struct auth_request *request, const char *str);
gid_t userdb_parse_gid(struct auth_request *request, const char *str);

void userdb_preinit(struct auth *auth, const char *driver, const char *args);
void userdb_init(struct auth_userdb *userdb);
void userdb_deinit(struct auth_userdb *userdb);

void userdb_register_module(struct userdb_module_interface *iface);
void userdb_unregister_module(struct userdb_module_interface *iface);

void userdbs_init(void);
void userdbs_deinit(void);

#include "auth-request.h"

#endif