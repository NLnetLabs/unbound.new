/*
 * util/edns.c - handle base EDNS options.
 *
 * Copyright (c) 2018, NLnet Labs. All rights reserved.
 *
 * This software is open source.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * Redistributions of source code must retain the above copyright notice,
 * this list of conditions and the following disclaimer.
 *
 * Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 *
 * Neither the name of the NLNET LABS nor the names of its contributors may
 * be used to endorse or promote products derived from this software without
 * specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED
 * TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/**
 * \file
 *
 * This file contains functions for base EDNS options.
 */

#include "config.h"
#include "util/edns.h"
#include "util/config_file.h"
#include "util/netevent.h"
#include "util/net_help.h"
#include "util/regional.h"
#include "util/data/msgparse.h"
#include "util/data/msgreply.h"
#include "sldns/sbuffer.h"

#ifdef HAVE_SSL
#include <openssl/opensslv.h>
#include <openssl/evp.h>
#endif

struct edns_strings* edns_strings_create(void)
{
	struct edns_strings* edns_strings = calloc(1,
		sizeof(struct edns_strings));
	if(!edns_strings)
		return NULL;
	if(!(edns_strings->region = regional_create())) {
		edns_strings_delete(edns_strings);
		return NULL;
	}
	return edns_strings;
}

void edns_strings_delete(struct edns_strings* edns_strings)
{
	if(!edns_strings)
		return;
	regional_destroy(edns_strings->region);
	free(edns_strings);
}

static int
edns_strings_client_insert(struct edns_strings* edns_strings,
	struct sockaddr_storage* addr, socklen_t addrlen, int net,
	const char* string)
{
	struct edns_string_addr* esa = regional_alloc_zero(edns_strings->region,
		sizeof(struct edns_string_addr));
	if(!esa)
		return 0;
	esa->string_len = strlen(string);
	esa->string = regional_alloc_init(edns_strings->region, string,
		esa->string_len);
	if(!esa->string)
		return 0;
	if(!addr_tree_insert(&edns_strings->client_strings, &esa->node, addr,
		addrlen, net)) {
		verbose(VERB_QUERY, "duplicate EDNS client string ignored.");
	}
	return 1;
}

int edns_strings_apply_cfg(struct edns_strings* edns_strings,
	struct config_file* config)
{
	struct config_str2list* c;
	regional_free_all(edns_strings->region);
	addr_tree_init(&edns_strings->client_strings);

	for(c=config->edns_client_strings; c; c=c->next) {
		struct sockaddr_storage addr;
		socklen_t addrlen;
		int net;
		log_assert(c->str && c->str2);

		if(!netblockstrtoaddr(c->str, UNBOUND_DNS_PORT, &addr, &addrlen,
			&net)) {
			log_err("cannot parse EDNS client string IP netblock: "
				"%s", c->str);
			return 0;
		}
		if(!edns_strings_client_insert(edns_strings, &addr, addrlen,
			net, c->str2)) {
			log_err("out of memory while adding EDNS strings");
			return 0;
		}
	}
	edns_strings->client_string_opcode = config->edns_client_string_opcode;

	addr_tree_init_parents(&edns_strings->client_strings);
	return 1;
}

struct edns_string_addr*
edns_string_addr_lookup(rbtree_type* tree, struct sockaddr_storage* addr,
	socklen_t addrlen)
{
	return (struct edns_string_addr*)addr_tree_lookup(tree, addr, addrlen);
}

static int edns_keepalive(struct edns_data* edns_out, struct edns_data* edns_in,
		struct comm_point* c, struct regional* region)
{
	if(c->type == comm_udp)
		return 1;

	/* To respond with a Keepalive option, the client connection
	 * must have received one message with a TCP Keepalive EDNS option,
	 * and that option must have 0 length data. Subsequent messages
	 * sent on that connection will have a TCP Keepalive option.
	 */
	if(c->tcp_keepalive ||
		edns_opt_list_find(edns_in->opt_list, LDNS_EDNS_KEEPALIVE)) {
		int keepalive = c->tcp_timeout_msec / 100;
		uint8_t data[2];
		data[0] = (uint8_t)((keepalive >> 8) & 0xff);
		data[1] = (uint8_t)(keepalive & 0xff);
		if(!edns_opt_list_append(&edns_out->opt_list, LDNS_EDNS_KEEPALIVE,
			sizeof(data), data, region))
			return 0;
		c->tcp_keepalive = 1;
	}
	return 1;
}

int siphash(const uint8_t *in, const size_t inlen,
		const uint8_t *k, uint8_t *out, const size_t outlen);

/** RFC 1982 comparison, uses unsigned integers, and tries to avoid
 * compiler optimization (eg. by avoiding a-b<0 comparisons),
 * this routine matches compare_serial(), for SOA serial number checks */
static int
compare_1982(uint32_t a, uint32_t b)
{
	/* for 32 bit values */
	const uint32_t cutoff = ((uint32_t) 1 << (32 - 1));

	if (a == b) {
		return 0;
	} else if ((a < b && b - a < cutoff) || (a > b && a - b > cutoff)) {
		return -1;
	} else {
		return 1;
	}
}

/** if we know that b is larger than a, return the difference between them,
 * that is the distance between them. in RFC1982 arith */
static uint32_t
subtract_1982(uint32_t a, uint32_t b)
{
	/* for 32 bit values */
	const uint32_t cutoff = ((uint32_t) 1 << (32 - 1));

	if(a == b)
		return 0;
	if(a < b && b - a < cutoff) {
		return b-a;
	}
	if(a > b && a - b > cutoff) {
		return ((uint32_t)0xffffffff) - (a-b-1);
	}
	/* wrong case, b smaller than a */
	return 0;
}


int edns_cookie_validate(struct config_file* cfg, struct comm_reply* repinfo,
		struct edns_option* cookie_opt, time_t now)
{
	uint8_t hash[8], data2hash[40];
	uint32_t cookie_time, now_uint32 = now;

	/* We support only draft-sury-toorop-dns-cookies-algorithms
	 * sized cookies
	 */
	if(cookie_opt->opt_len != 24)
		return 0;

	if(cookie_opt->opt_data[8] != 1)
		return 0;

	cookie_time = sldns_read_uint32(cookie_opt->opt_data + 12);

	if(compare_1982(now_uint32, cookie_time) > 0) {
		if (subtract_1982(cookie_time, now_uint32) > 3600)
			return 0; /* Not older than 1 hour */

	} else if (subtract_1982(now_uint32, cookie_time) > 300)
		/* ignore cookies > 5 minutes in future */
		return 0;

	if (cfg->cookie_secret_len != 16)
		return 0;

	memcpy(data2hash, cookie_opt->opt_data, 16);
	if (repinfo->addr.ss_family == AF_INET6) {
		memcpy( data2hash + 16, &((struct sockaddr_in6 *)
		                        &repinfo->addr)->sin6_addr, 16);
		siphash(data2hash, 32, cfg->cookie_secret, hash, 8);
	} else {
		memcpy( data2hash + 16, &((struct sockaddr_in *)
		                        &repinfo->addr)->sin_addr, 4);
		siphash(data2hash, 20, cfg->cookie_secret, hash, 8);
	}
	return memcmp(cookie_opt->opt_data + 16, hash, 8) == 0;
}

static int edns_cookie(struct edns_data* edns_out, struct edns_data* edns_in,
	struct config_file* cfg, struct comm_point* c,
	struct comm_reply *repinfo, time_t now,
	struct regional* region)
{
	struct edns_option *opt;

	if(c->type != comm_udp)
		return 1;

	if(!cfg->do_answer_cookie)
		return 1;

	opt = edns_opt_list_find(edns_in->opt_list, LDNS_EDNS_COOKIE);
	if (opt && opt->opt_len >= 8) {
		uint8_t data_out[40], hash[8];

		log_assert(cfg->cookie_secret_len >= 16);

		(void)memcpy(data_out, opt->opt_data, 8);
		data_out[ 8] = 1;   /* Version */
		data_out[ 9] = 0;   /* Reserved */
		data_out[10] = 0;   /* Reserved */
		data_out[11] = 0;   /* Reserved */
		sldns_write_uint32(data_out + 12, time(NULL));
		if (repinfo->addr.ss_family == AF_INET6) {
			memcpy( data_out + 16, &((struct sockaddr_in6 *)
			                       &repinfo->addr)->sin6_addr, 16);
			siphash(data_out, 32, cfg->cookie_secret, hash, 8);
		} else {
			memcpy( data_out + 16, &((struct sockaddr_in *)
			                       &repinfo->addr)->sin_addr, 4);
			siphash(data_out, 20, cfg->cookie_secret, hash, 8);
		}
		memcpy(data_out + 16, hash, 8);
		if(!edns_opt_list_append(&edns_out->opt_list, LDNS_EDNS_COOKIE,
				24, data_out, region))
			return 0;
	}
	return 1;
}

static int edns_padding(struct edns_data* edns_out, struct edns_data* edns_in,
		struct comm_point* c, struct regional* region)
{
	if(c->type == comm_udp)
		return 1;

	if(edns_opt_list_find(edns_in->opt_list, LDNS_EDNS_PADDING)) {
		if(!edns_opt_list_append(&edns_out->opt_list,
					LDNS_EDNS_PADDING, 0, NULL, region))
			return 0;
	}
	return 1;
}

int apply_edns_options(struct edns_data* edns_out, struct edns_data* edns_in,
	struct config_file* cfg, struct comm_point* c,
	struct comm_reply *repinfo, time_t now, struct regional* region)
{
	if(cfg->do_tcp_keepalive &&
		!edns_keepalive(edns_out, edns_in, c, region))
		return 0;

	if(!edns_cookie(edns_out, edns_in, cfg, c, repinfo, now, region))
		return 0;

	if (cfg->nsid && edns_opt_list_find(edns_in->opt_list, LDNS_EDNS_NSID)
	&& !edns_opt_list_append(&edns_out->opt_list,
			LDNS_EDNS_NSID, cfg->nsid_len, cfg->nsid, region))
		return 0;

	if(!cfg->pad_responses || c->type != comm_tcp || !c->ssl
	|| !edns_opt_list_find(edns_in->opt_list, LDNS_EDNS_PADDING))
	       ; /* pass */

	else if(!edns_opt_list_append(&edns_out->opt_list, LDNS_EDNS_PADDING
	                                                 , 0, NULL, region))
		return 0;
	else
		edns_out->padding_block_size = cfg->pad_responses_block_size;

	return 1;
}
