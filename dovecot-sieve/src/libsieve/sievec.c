/* sievec.c -- compile a sieve script to bytecode manually
 * Rob Siemborski
 * $Id: sievec.c,v 1.2 2006-10-10 17:01:15 tss Exp $
 */
/*
 * Copyright (c) 1999-2000 Carnegie Mellon University.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer. 
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *
 * 3. The name "Carnegie Mellon University" must not be used to
 *    endorse or promote products derived from this software without
 *    prior written permission. For permission or any other legal
 *    details, please contact  
 *      Office of Technology Transfer
 *      Carnegie Mellon University
 *      5000 Forbes Avenue
 *      Pittsburgh, PA  15213-3890
 *      (412) 268-4387, fax: (412) 268-7395
 *      tech-transfer@andrew.cmu.edu
 *
 * 4. Redistributions of any form whatsoever must retain the following
 *    acknowledgment:
 *    "This product includes software developed by Computing Services
 *     at Carnegie Mellon University (http://www.cmu.edu/computing/)."
 *
 * CARNEGIE MELLON UNIVERSITY DISCLAIMS ALL WARRANTIES WITH REGARD TO
 * THIS SOFTWARE, INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS, IN NO EVENT SHALL CARNEGIE MELLON UNIVERSITY BE LIABLE
 * FOR ANY SPECIAL, INDIRECT OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN
 * AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING
 * OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include "sieve_interface.h"

#include "libconfig.h"
#include "xmalloc.h"

#include "script.h"
#include <string.h> 
#include <stdlib.h>
#include <sys/file.h>
#include <unistd.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

struct et_list *_et_list = NULL;

int is_script_parsable(FILE *stream, char **errstr, sieve_script_t **ret);

#define TIMSIEVE_FAIL -1
#define TIMSIEVE_OK 0

int main(int argc, char **argv) 
{
    FILE *instream;
    char *err = NULL;
    sieve_script_t *s;
    bytecode_info_t *bc;
    int c, fd, usage_error = 0;

    while ((c = getopt(argc, argv, "C:")) != EOF)
	switch (c) {
	default:
	    usage_error = 1;
	    break;
	}

    if (usage_error || (argc - optind) < 2) {
	printf("Syntax: %s <filename> <outputfile>\n",
	       argv[0]);
	exit(1);
    }

    instream = fopen(argv[optind++],"r");
    if(instream == NULL) {
	printf("Unable to open %s for reading\n", argv[1]);
	exit(1);
    }
    
    if(is_script_parsable(instream, &err, &s) == TIMSIEVE_FAIL) {
	if(err) {
	    printf("Unable to parse script: %s\n", err);
	} else {
	    printf("Unable to parse script.\n");
	}
	 
	exit(1);
    }
    
    /* Now, generate the bytecode */
    if(sieve_generate_bytecode(&bc, s) == -1) {
	printf("bytecode generate failed\n");
	exit(1);
    }

    /* Now, open the new file */
    fd = open(argv[optind], O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if(fd < 0) {
	printf("couldn't open bytecode output file\n");
	exit(1);
    }  

    /* Now, emit the bytecode */
    if(sieve_emit_bytecode(fd, bc) == -1) {
	printf("bytecode emit failed\n");
	exit(1);
    }

    close(fd);
    
    return 0;
}

/* to make larry's stupid functions happy :) */ 
static void foo(void)
{
    i_fatal("stub function called");
}
sieve_vacation_t vacation = {
    0,				/* min response */
    0,				/* max response */
    (sieve_callback *) &foo,	/* autorespond() */
    (sieve_callback *) &foo	/* send_response() */
};

static int sieve_notify(void *ac __attr_unused__, 
			void *interp_context __attr_unused__, 
			void *script_context __attr_unused__,
			void *message_context __attr_unused__,
			const char **errmsg __attr_unused__)
{
    i_fatal("stub function called");
    return SIEVE_FAIL;
}

static int mysieve_error(int lineno, const char *msg,
			 void *i __attr_unused__, void *s)
{
    char buf[1024];
    char **errstr = (char **) s;

    snprintf(buf, 80, "line %d: %s\r\n", lineno, msg);
    *errstr = xrealloc(*errstr, strlen(*errstr) + strlen(buf) + 30);
    i_info("%s", buf);
    strcat(*errstr, buf);

    return SIEVE_OK;
}

/* end the boilerplate */

/* returns TRUE or FALSE */
int is_script_parsable(FILE *stream, char **errstr, sieve_script_t **ret)
{
    sieve_interp_t *i;
    sieve_script_t *s;
    int res;
  
    res = sieve_interp_alloc(&i, NULL);
    if (res != SIEVE_OK) {
	i_error("sieve_interp_alloc() returns %d\n", res);
	return TIMSIEVE_FAIL;
    }

    res = sieve_register_redirect(i, (sieve_callback *) &foo);
    if (res != SIEVE_OK) {
	i_error("sieve_register_redirect() returns %d\n", res);
	return TIMSIEVE_FAIL;
    }
    res = sieve_register_discard(i, (sieve_callback *) &foo);
    if (res != SIEVE_OK) {
	i_error("sieve_register_discard() returns %d\n", res);
	return TIMSIEVE_FAIL;
    }
    res = sieve_register_reject(i, (sieve_callback *) &foo);
    if (res != SIEVE_OK) {
	i_error("sieve_register_reject() returns %d\n", res);
	return TIMSIEVE_FAIL;
    }
    res = sieve_register_fileinto(i, (sieve_callback *) &foo);
    if (res != SIEVE_OK) {
	i_error("sieve_register_fileinto() returns %d\n", res);
	return TIMSIEVE_FAIL;
    }
    res = sieve_register_keep(i, (sieve_callback *) &foo);
    if (res != SIEVE_OK) {
	i_error("sieve_register_keep() returns %d\n", res);
	return TIMSIEVE_FAIL;
    }

    res = sieve_register_imapflags(i, NULL);
    if (res != SIEVE_OK) {
	i_error("sieve_register_imapflags() returns %d\n", res);
	return TIMSIEVE_FAIL;
    }

    res = sieve_register_size(i, (sieve_get_size *) &foo);
    if (res != SIEVE_OK) {
	i_error("sieve_register_size() returns %d\n", res);
	return TIMSIEVE_FAIL;
    }
  
    res = sieve_register_header(i, (sieve_get_header *) &foo);
    if (res != SIEVE_OK) {
	i_error("sieve_register_header() returns %d\n", res);
	return TIMSIEVE_FAIL;
    }
  
    res = sieve_register_envelope(i, (sieve_get_envelope *) &foo);
    if (res != SIEVE_OK) {
	i_error("sieve_register_envelope() returns %d\n", res);
	return TIMSIEVE_FAIL;
    }
  
    res = sieve_register_vacation(i, &vacation);
    if (res != SIEVE_OK) {
	i_error("sieve_register_vacation() returns %d\n", res);
	return TIMSIEVE_FAIL;
    }

    res = sieve_register_notify(i, &sieve_notify);
    if (res != SIEVE_OK) {
	i_error("sieve_register_notify() returns %d\n", res);
	return TIMSIEVE_FAIL;
    }

    res = sieve_register_parse_error(i, &mysieve_error);
    if (res != SIEVE_OK) {
	i_error("sieve_register_parse_error() returns %d\n", res);
	return TIMSIEVE_FAIL;
    }

    rewind(stream);

    *errstr = (char *) xmalloc(20 * sizeof(char));
    strcpy(*errstr, "script errors:\r\n");

    res = sieve_script_parse(i, stream, errstr, &s);

    if (res == SIEVE_OK) {
	if(ret) {
	    *ret = s;
	} else {
	    sieve_script_free(&s);
	}
	free(*errstr);
	*errstr = NULL;
    }

    /* free interpreter */
    sieve_interp_free(&i);

    return (res == SIEVE_OK) ? TIMSIEVE_OK : TIMSIEVE_FAIL;
}