#ifndef POP3_PROXY_H
#define POP3_PROXY_H

#include "login-proxy.h"

int pop3_proxy_new(struct pop3_client *client, const char *host,
		   unsigned int port, const char *user, const char *master_user,
		   const char *password, enum login_proxy_ssl_flags ssl_flags,
		   unsigned int connect_timeout_msecs);

#endif