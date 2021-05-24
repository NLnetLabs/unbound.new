/*
 * daemon/remote.c - remote control for the unbound daemon.
 *
 * Copyright (c) 2008, NLnet Labs. All rights reserved.
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
 * This file contains the remote control functionality for the daemon.
 * The remote control can be performed using either the commandline
 * unbound-control tool, or a TLS capable web browser. 
 * The channel is secured using TLSv1, and certificates.
 * Both the server and the client(control tool) have their own keys.
 */
#include "config.h"
#ifdef HAVE_OPENSSL_ERR_H
#include <openssl/err.h>
#endif
#ifdef HAVE_OPENSSL_DH_H
#include <openssl/dh.h>
#endif
#ifdef HAVE_OPENSSL_BN_H
#include <openssl/bn.h>
#endif

#include <ctype.h>
#include "daemon/remote.h"
#include "daemon/worker.h"
#include "daemon/daemon.h"
#include "daemon/stats.h"
#include "daemon/cachedump.h"
#include "util/log.h"
#include "util/config_file.h"
#include "util/net_help.h"
#include "util/module.h"
#include "services/listen_dnsport.h"
#include "services/cache/rrset.h"
#include "services/cache/infra.h"
#include "services/mesh.h"
#include "services/localzone.h"
#include "services/authzone.h"
#include "services/rpz.h"
#include "util/storage/slabhash.h"
#include "util/fptr_wlist.h"
#include "util/data/dname.h"
#include "validator/validator.h"
#include "validator/val_kcache.h"
#include "validator/val_kentry.h"
#include "validator/val_anchor.h"
#include "iterator/iterator.h"
#include "iterator/iter_fwd.h"
#include "iterator/iter_hints.h"
#include "iterator/iter_delegpt.h"
#include "services/outbound_list.h"
#include "services/outside_network.h"
#include "sldns/str2wire.h"
#include "sldns/parseutil.h"
#include "sldns/wire2str.h"
#include "sldns/sbuffer.h"

#ifdef HAVE_SYS_TYPES_H
#  include <sys/types.h>
#endif
#ifdef HAVE_SYS_STAT_H
#include <sys/stat.h>
#endif
#ifdef HAVE_NETDB_H
#include <netdb.h>
#endif

/* just for portability */
#ifdef SQ
#undef SQ
#endif

/** what to put on statistics lines between var and value, ": " or "=" */
#define SQ "="
/** if true, inhibits a lot of =0 lines from the stats output */
static const int inhibit_zero = 1;

/** subtract timers and the values do not overflow or become negative */
static void
timeval_subtract(struct timeval* d, const struct timeval* end, 
	const struct timeval* start)
{
#ifndef S_SPLINT_S
	time_t end_usec = end->tv_usec;
	d->tv_sec = end->tv_sec - start->tv_sec;
	if(end_usec < start->tv_usec) {
		end_usec += 1000000;
		d->tv_sec--;
	}
	d->tv_usec = end_usec - start->tv_usec;
#endif
}

/** divide sum of timers to get average */
static void
timeval_divide(struct timeval* avg, const struct timeval* sum, long long d)
{
#ifndef S_SPLINT_S
	size_t leftover;
	if(d == 0) {
		avg->tv_sec = 0;
		avg->tv_usec = 0;
		return;
	}
	avg->tv_sec = sum->tv_sec / d;
	avg->tv_usec = sum->tv_usec / d;
	/* handle fraction from seconds divide */
	leftover = sum->tv_sec - avg->tv_sec*d;
	avg->tv_usec += (leftover*1000000)/d;
#endif
}

static int
remote_setup_ctx(struct daemon_remote* rc, struct config_file* cfg)
{
	char* s_cert;
	char* s_key;
	rc->ctx = SSL_CTX_new(SSLv23_server_method());
	if(!rc->ctx) {
		log_crypto_err("could not SSL_CTX_new");
		return 0;
	}
	if(!listen_sslctx_setup(rc->ctx)) {
		return 0;
	}

	s_cert = fname_after_chroot(cfg->server_cert_file, cfg, 1);
	s_key = fname_after_chroot(cfg->server_key_file, cfg, 1);
	if(!s_cert || !s_key) {
		log_err("out of memory in remote control fname");
		goto setup_error;
	}
	verbose(VERB_ALGO, "setup SSL certificates");
	if (!SSL_CTX_use_certificate_chain_file(rc->ctx,s_cert)) {
		log_err("Error for server-cert-file: %s", s_cert);
		log_crypto_err("Error in SSL_CTX use_certificate_chain_file");
		goto setup_error;
	}
	if(!SSL_CTX_use_PrivateKey_file(rc->ctx,s_key,SSL_FILETYPE_PEM)) {
		log_err("Error for server-key-file: %s", s_key);
		log_crypto_err("Error in SSL_CTX use_PrivateKey_file");
		goto setup_error;
	}
	if(!SSL_CTX_check_private_key(rc->ctx)) {
		log_err("Error for server-key-file: %s", s_key);
		log_crypto_err("Error in SSL_CTX check_private_key");
		goto setup_error;
	}
	listen_sslctx_setup_2(rc->ctx);
	if(!SSL_CTX_load_verify_locations(rc->ctx, s_cert, NULL)) {
		log_crypto_err("Error setting up SSL_CTX verify locations");
	setup_error:
		free(s_cert);
		free(s_key);
		return 0;
	}
	SSL_CTX_set_client_CA_list(rc->ctx, SSL_load_client_CA_file(s_cert));
	SSL_CTX_set_verify(rc->ctx, SSL_VERIFY_PEER, NULL);
	free(s_cert);
	free(s_key);
	return 1;
}

struct daemon_remote*
daemon_remote_create(struct config_file* cfg)
{
	struct daemon_remote* rc = (struct daemon_remote*)calloc(1, 
		sizeof(*rc));
	if(!rc) {
		log_err("out of memory in daemon_remote_create");
		return NULL;
	}
	rc->max_active = 10;

	if(!cfg->remote_control_enable) {
		rc->ctx = NULL;
		return rc;
	}
	if(options_remote_is_address(cfg) && cfg->control_use_cert) {
		if(!remote_setup_ctx(rc, cfg)) {
			daemon_remote_delete(rc);
			return NULL;
		}
		rc->use_cert = 1;
	} else {
		struct config_strlist* p;
		rc->ctx = NULL;
		rc->use_cert = 0;
		if(!options_remote_is_address(cfg))
		  for(p = cfg->control_ifs.first; p; p = p->next) {
			if(p->str && p->str[0] != '/')
				log_warn("control-interface %s is not using TLS, but plain transfer, because first control-interface in config file is a local socket (starts with a /).", p->str);
		}
	}
	return rc;
}

void daemon_remote_clear(struct daemon_remote* rc)
{
	struct rc_state* p, *np;
	if(!rc) return;
	/* but do not close the ports */
	listen_list_delete(rc->accept_list);
	rc->accept_list = NULL;
	/* do close these sockets */
	p = rc->busy_list;
	while(p) {
		np = p->next;
		if(p->ssl)
			SSL_free(p->ssl);
		comm_point_delete(p->c);
		free(p);
		p = np;
	}
	rc->busy_list = NULL;
	rc->active = 0;
	rc->worker = NULL;
}

void daemon_remote_delete(struct daemon_remote* rc)
{
	if(!rc) return;
	daemon_remote_clear(rc);
	if(rc->ctx) {
		SSL_CTX_free(rc->ctx);
	}
	free(rc);
}

/**
 * Add and open a new control port
 * @param ip: ip str
 * @param nr: port nr
 * @param list: list head
 * @param noproto_is_err: if lack of protocol support is an error.
 * @param cfg: config with username for chown of unix-sockets.
 * @return false on failure.
 */
static int
add_open(const char* ip, int nr, struct listen_port** list, int noproto_is_err,
	struct config_file* cfg)
{
	struct addrinfo hints;
	struct addrinfo* res;
	struct listen_port* n;
	int noproto = 0;
	int fd, r;
	char port[15];
	snprintf(port, sizeof(port), "%d", nr);
	port[sizeof(port)-1]=0;
	memset(&hints, 0, sizeof(hints));
	log_assert(ip);

	if(ip[0] == '/') {
		/* This looks like a local socket */
		fd = create_local_accept_sock(ip, &noproto, cfg->use_systemd);
		/*
		 * Change socket ownership and permissions so users other
		 * than root can access it provided they are in the same
		 * group as the user we run as.
		 */
		if(fd != -1) {
#ifdef HAVE_CHOWN
			if (cfg->username && cfg->username[0] &&
				cfg_uid != (uid_t)-1) {
				if(chown(ip, cfg_uid, cfg_gid) == -1)
					verbose(VERB_QUERY, "cannot chown %u.%u %s: %s",
					  (unsigned)cfg_uid, (unsigned)cfg_gid,
					  ip, strerror(errno));
			}
			chmod(ip, (mode_t)(S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP));
#else
			(void)cfg;
#endif
		}
	} else {
		hints.ai_socktype = SOCK_STREAM;
		hints.ai_flags = AI_PASSIVE | AI_NUMERICHOST;
		if((r = getaddrinfo(ip, port, &hints, &res)) != 0 || !res) {
#ifdef USE_WINSOCK
			if(!noproto_is_err && r == EAI_NONAME) {
				/* tried to lookup the address as name */
				return 1; /* return success, but do nothing */
			}
#endif /* USE_WINSOCK */
			log_err("control interface %s:%s getaddrinfo: %s %s",
				ip?ip:"default", port, gai_strerror(r),
#ifdef EAI_SYSTEM
				r==EAI_SYSTEM?(char*)strerror(errno):""
#else
				""
#endif
			);
			return 0;
		}

		/* open fd */
		fd = create_tcp_accept_sock(res, 1, &noproto, 0,
			cfg->ip_transparent, 0, 0, cfg->ip_freebind,
			cfg->use_systemd, cfg->ip_dscp);
		freeaddrinfo(res);
	}

	if(fd == -1 && noproto) {
		if(!noproto_is_err)
			return 1; /* return success, but do nothing */
		log_err("cannot open control interface %s %d : "
			"protocol not supported", ip, nr);
		return 0;
	}
	if(fd == -1) {
		log_err("cannot open control interface %s %d", ip, nr);
		return 0;
	}

	/* alloc */
	n = (struct listen_port*)calloc(1, sizeof(*n));
	if(!n) {
		sock_close(fd);
		log_err("out of memory");
		return 0;
	}
	n->next = *list;
	*list = n;
	n->fd = fd;
	return 1;
}

struct listen_port* daemon_remote_open_ports(struct config_file* cfg)
{
	struct listen_port* l = NULL;
	log_assert(cfg->remote_control_enable && cfg->control_port);
	if(cfg->control_ifs.first) {
		struct config_strlist* p;
		for(p = cfg->control_ifs.first; p; p = p->next) {
			if(!add_open(p->str, cfg->control_port, &l, 1, cfg)) {
				listening_ports_free(l);
				return NULL;
			}
		}
	} else {
		/* defaults */
		if(cfg->do_ip6 &&
			!add_open("::1", cfg->control_port, &l, 0, cfg)) {
			listening_ports_free(l);
			return NULL;
		}
		if(cfg->do_ip4 &&
			!add_open("127.0.0.1", cfg->control_port, &l, 1, cfg)) {
			listening_ports_free(l);
			return NULL;
		}
	}
	return l;
}

/** open accept commpoint */
static int
accept_open(struct daemon_remote* rc, int fd)
{
	struct listen_list* n = (struct listen_list*)malloc(sizeof(*n));
	if(!n) {
		log_err("out of memory");
		return 0;
	}
	n->next = rc->accept_list;
	rc->accept_list = n;
	/* open commpt */
	n->com = comm_point_create_raw(rc->worker->base, fd, 0, 
		&remote_accept_callback, rc);
	if(!n->com)
		return 0;
	/* keep this port open, its fd is kept in the rc portlist */
	n->com->do_not_close = 1;
	return 1;
}

int daemon_remote_open_accept(struct daemon_remote* rc, 
	struct listen_port* ports, struct worker* worker)
{
	struct listen_port* p;
	rc->worker = worker;
	for(p = ports; p; p = p->next) {
		if(!accept_open(rc, p->fd)) {
			log_err("could not create accept comm point");
			return 0;
		}
	}
	return 1;
}

void daemon_remote_stop_accept(struct daemon_remote* rc)
{
	struct listen_list* p;
	for(p=rc->accept_list; p; p=p->next) {
		comm_point_stop_listening(p->com);	
	}
}

void daemon_remote_start_accept(struct daemon_remote* rc)
{
	struct listen_list* p;
	for(p=rc->accept_list; p; p=p->next) {
		comm_point_start_listening(p->com, -1, -1);	
	}
}

int remote_accept_callback(struct comm_point* c, void* arg, int err, 
	struct comm_reply* ATTR_UNUSED(rep))
{
	struct daemon_remote* rc = (struct daemon_remote*)arg;
	struct sockaddr_storage addr;
	socklen_t addrlen;
	int newfd;
	struct rc_state* n;
	if(err != NETEVENT_NOERROR) {
		log_err("error %d on remote_accept_callback", err);
		return 0;
	}
	/* perform the accept */
	newfd = comm_point_perform_accept(c, &addr, &addrlen);
	if(newfd == -1)
		return 0;
	/* create new commpoint unless we are servicing already */
	if(rc->active >= rc->max_active) {
		log_warn("drop incoming remote control: too many connections");
	close_exit:
		sock_close(newfd);
		return 0;
	}

	/* setup commpoint to service the remote control command */
	n = (struct rc_state*)calloc(1, sizeof(*n));
	if(!n) {
		log_err("out of memory");
		goto close_exit;
	}
	n->fd = newfd;
	/* start in reading state */
	n->c = comm_point_create_raw(rc->worker->base, newfd, 0, 
		&remote_control_callback, n);
	if(!n->c) {
		log_err("out of memory");
		free(n);
		goto close_exit;
	}
	log_addr(VERB_QUERY, "new control connection from", &addr, addrlen);
	n->c->do_not_close = 0;
	comm_point_stop_listening(n->c);
	comm_point_start_listening(n->c, -1, REMOTE_CONTROL_TCP_TIMEOUT);
	memcpy(&n->c->repinfo.addr, &addr, addrlen);
	n->c->repinfo.addrlen = addrlen;
	if(rc->use_cert) {
		n->shake_state = rc_hs_read;
		n->ssl = SSL_new(rc->ctx);
		if(!n->ssl) {
			log_crypto_err("could not SSL_new");
			comm_point_delete(n->c);
			free(n);
			goto close_exit;
		}
		SSL_set_accept_state(n->ssl);
		(void)SSL_set_mode(n->ssl, (long)SSL_MODE_AUTO_RETRY);
		if(!SSL_set_fd(n->ssl, newfd)) {
			log_crypto_err("could not SSL_set_fd");
			SSL_free(n->ssl);
			comm_point_delete(n->c);
			free(n);
			goto close_exit;
		}
	} else {
		n->ssl = NULL;
	}

	n->rc = rc;
	n->next = rc->busy_list;
	rc->busy_list = n;
	rc->active ++;

	/* perform the first nonblocking read already, for windows, 
	 * so it can return wouldblock. could be faster too. */
	(void)remote_control_callback(n->c, n, NETEVENT_NOERROR, NULL);
	return 0;
}

/** delete from list */
static void
state_list_remove_elem(struct rc_state** list, struct comm_point* c)
{
	while(*list) {
		if( (*list)->c == c) {
			*list = (*list)->next;
			return;
		}
		list = &(*list)->next;
	}
}

/** decrease active count and remove commpoint from busy list */
static void
clean_point(struct daemon_remote* rc, struct rc_state* s)
{
	state_list_remove_elem(&rc->busy_list, s->c);
	rc->active --;
	if(s->ssl) {
		SSL_shutdown(s->ssl);
		SSL_free(s->ssl);
	}
	comm_point_delete(s->c);
	free(s);
}

int
ssl_print_text(RES* res, const char* text)
{
	int r;
	if(!res) 
		return 0;
	if(res->ssl) {
		ERR_clear_error();
		if((r=SSL_write(res->ssl, text, (int)strlen(text))) <= 0) {
			if(SSL_get_error(res->ssl, r) == SSL_ERROR_ZERO_RETURN) {
				verbose(VERB_QUERY, "warning, in SSL_write, peer "
					"closed connection");
				return 0;
			}
			log_crypto_err("could not SSL_write");
			return 0;
		}
	} else {
		size_t at = 0;
		while(at < strlen(text)) {
			ssize_t r = send(res->fd, text+at, strlen(text)-at, 0);
			if(r == -1) {
				if(errno == EAGAIN || errno == EINTR)
					continue;
				log_err("could not send: %s",
					sock_strerror(errno));
				return 0;
			}
			at += r;
		}
	}
	return 1;
}

/** print text over the ssl connection */
static int
ssl_print_vmsg(RES* ssl, const char* format, va_list args)
{
	char msg[1024];
	vsnprintf(msg, sizeof(msg), format, args);
	return ssl_print_text(ssl, msg);
}

/** printf style printing to the ssl connection */
int ssl_printf(RES* ssl, const char* format, ...)
{
	va_list args;
	int ret;
	va_start(args, format);
	ret = ssl_print_vmsg(ssl, format, args);
	va_end(args);
	return ret;
}

int
ssl_read_line(RES* res, char* buf, size_t max)
{
	int r;
	size_t len = 0;
	if(!res)
		return 0;
	while(len < max) {
		if(res->ssl) {
			ERR_clear_error();
			if((r=SSL_read(res->ssl, buf+len, 1)) <= 0) {
				if(SSL_get_error(res->ssl, r) == SSL_ERROR_ZERO_RETURN) {
					buf[len] = 0;
					return 1;
				}
				log_crypto_err("could not SSL_read");
				return 0;
			}
		} else {
			while(1) {
				ssize_t rr = recv(res->fd, buf+len, 1, 0);
				if(rr <= 0) {
					if(rr == 0) {
						buf[len] = 0;
						return 1;
					}
					if(errno == EINTR || errno == EAGAIN)
						continue;
					log_err("could not recv: %s",
						sock_strerror(errno));
					return 0;
				}
				break;
			}
		}
		if(buf[len] == '\n') {
			/* return string without \n */
			buf[len] = 0;
			return 1;
		}
		len++;
	}
	buf[max-1] = 0;
	log_err("control line too long (%d): %s", (int)max, buf);
	return 0;
}

/** send the OK to the control client */
static void send_ok(RES* ssl)
{
	(void)ssl_printf(ssl, "ok\n");
}

/** Return 1 if this is the server view; complain and return 0 otherwise */
static inline int
view_is_server(struct view* v)
{
	return (v->server_view == v);
}

/** Return 1 if this is the server view; complain and return 0 otherwise */
static int
view_is_valid(RES *ssl, struct view* v)
{
	if (view_is_server(v)) {
		return 1;
	}

	ssl_printf(ssl, "not a valid command for a view\n");
	return 0;
}

/** Return 1 if view has its own caches; complain and return 0 otherwise */
static int
view_has_caches(RES *ssl, struct view* v)
{
	if (view_is_server(v) || (v->view_flags & VIEW_FLAG_SHARE_CACHE) == 0) {
		return 1;
	}

	ssl_printf(ssl, "View %s does not have its own caches\n", v->name);
	return 0;
}

/** Return 1 if view has its own forwards; complain and return 0 otherwise */
static int
view_has_forwards(RES *ssl, struct view* v)
{
	if (view_is_server(v) || (v->view_flags & VIEW_FLAG_SHARE_FWDS) == 0) {
		return 1;
	}

	ssl_printf(ssl, "View %s does not have its own forward zones\n", v->name);
	return 0;
}

/** Return 1 if view has its own forwards; complain and return 0 otherwise */
static int
view_has_hints(RES *ssl, struct view* v)
{
	if (view_is_server(v) || (v->view_flags & VIEW_FLAG_SHARE_HINTS) == 0) {
		return 1;
	}

	ssl_printf(ssl, "View %s does not have its own iterator hints\n", v->name);
	return 0;
}

/** do the stop command */
static void
do_stop(RES* ssl,
        struct worker* worker,
        struct view* v,
        char* argp)
{
	if (view_is_valid(ssl, v)) {
		worker->need_to_exit = 1;
		comm_base_exit(worker->base);
		send_ok(ssl);
	}
}

/** do the reload command */
static void
do_reload(RES* ssl,
          struct worker* worker,
          struct view* v,
          char* argp)
{
	if (view_is_valid(ssl, v)) {
		worker->need_to_exit = 0;
		comm_base_exit(worker->base);
		send_ok(ssl);
	}
}

/** do the verbosity command */
static void
do_verbosity(RES* ssl,
             struct worker* worker,
             struct view* v,
             char* argp)
{
	if (view_is_valid(ssl, v)) {
		int val = atoi(argp);

		if(val == 0 && strcmp(argp, "0") != 0) {
			ssl_printf(ssl, "error in verbosity number syntax: %s\n", argp);
		} else {
			verbosity = val;
			send_ok(ssl);
		}
	}
}

/** print stats from statinfo */
static int
print_stats(RES* ssl, const char* nm, struct ub_stats_info* s)
{
	struct timeval sumwait, avg;
	if(!ssl_printf(ssl, "%s.num.queries"SQ"%lu\n", nm, 
		(unsigned long)s->svr.num_queries)) return 0;
	if(!ssl_printf(ssl, "%s.num.queries_ip_ratelimited"SQ"%lu\n", nm,
		(unsigned long)s->svr.num_queries_ip_ratelimited)) return 0;
	if(!ssl_printf(ssl, "%s.num.cachehits"SQ"%lu\n", nm, 
		(unsigned long)(s->svr.num_queries 
			- s->svr.num_queries_missed_cache))) return 0;
	if(!ssl_printf(ssl, "%s.num.cachemiss"SQ"%lu\n", nm, 
		(unsigned long)s->svr.num_queries_missed_cache)) return 0;
	if(!ssl_printf(ssl, "%s.num.prefetch"SQ"%lu\n", nm, 
		(unsigned long)s->svr.num_queries_prefetch)) return 0;
	if(!ssl_printf(ssl, "%s.num.expired"SQ"%lu\n", nm,
		(unsigned long)s->svr.ans_expired)) return 0;
	if(!ssl_printf(ssl, "%s.num.recursivereplies"SQ"%lu\n", nm, 
		(unsigned long)s->mesh_replies_sent)) return 0;
#ifdef USE_DNSCRYPT
	if(!ssl_printf(ssl, "%s.num.dnscrypt.crypted"SQ"%lu\n", nm,
		(unsigned long)s->svr.num_query_dnscrypt_crypted)) return 0;
	if(!ssl_printf(ssl, "%s.num.dnscrypt.cert"SQ"%lu\n", nm,
		(unsigned long)s->svr.num_query_dnscrypt_cert)) return 0;
	if(!ssl_printf(ssl, "%s.num.dnscrypt.cleartext"SQ"%lu\n", nm,
		(unsigned long)s->svr.num_query_dnscrypt_cleartext)) return 0;
	if(!ssl_printf(ssl, "%s.num.dnscrypt.malformed"SQ"%lu\n", nm,
		(unsigned long)s->svr.num_query_dnscrypt_crypted_malformed)) return 0;
#endif
	if(!ssl_printf(ssl, "%s.requestlist.avg"SQ"%g\n", nm,
		(s->svr.num_queries_missed_cache+s->svr.num_queries_prefetch)?
			(double)s->svr.sum_query_list_size/
			(double)(s->svr.num_queries_missed_cache+
			s->svr.num_queries_prefetch) : 0.0)) return 0;
	if(!ssl_printf(ssl, "%s.requestlist.max"SQ"%lu\n", nm,
		(unsigned long)s->svr.max_query_list_size)) return 0;
	if(!ssl_printf(ssl, "%s.requestlist.overwritten"SQ"%lu\n", nm,
		(unsigned long)s->mesh_jostled)) return 0;
	if(!ssl_printf(ssl, "%s.requestlist.exceeded"SQ"%lu\n", nm,
		(unsigned long)s->mesh_dropped)) return 0;
	if(!ssl_printf(ssl, "%s.requestlist.current.all"SQ"%lu\n", nm,
		(unsigned long)s->mesh_num_states)) return 0;
	if(!ssl_printf(ssl, "%s.requestlist.current.user"SQ"%lu\n", nm,
		(unsigned long)s->mesh_num_reply_states)) return 0;
#ifndef S_SPLINT_S
	sumwait.tv_sec = s->mesh_replies_sum_wait_sec;
	sumwait.tv_usec = s->mesh_replies_sum_wait_usec;
#endif
	timeval_divide(&avg, &sumwait, s->mesh_replies_sent);
	if(!ssl_printf(ssl, "%s.recursion.time.avg"SQ ARG_LL "d.%6.6d\n", nm,
		(long long)avg.tv_sec, (int)avg.tv_usec)) return 0;
	if(!ssl_printf(ssl, "%s.recursion.time.median"SQ"%g\n", nm, 
		s->mesh_time_median)) return 0;
	if(!ssl_printf(ssl, "%s.tcpusage"SQ"%lu\n", nm,
		(unsigned long)s->svr.tcp_accept_usage)) return 0;
	return 1;
}

/** print stats for one thread */
static int
print_thread_stats(RES* ssl, int i, struct ub_stats_info* s)
{
	char nm[32];
	snprintf(nm, sizeof(nm), "thread%d", i);
	nm[sizeof(nm)-1]=0;
	return print_stats(ssl, nm, s);
}

/** print long number */
static int
print_longnum(RES* ssl, const char* desc, size_t x)
{
	if(x > 1024*1024*1024) {
		/* more than a Gb */
		size_t front = x / (size_t)1000000;
		size_t back = x % (size_t)1000000;
		return ssl_printf(ssl, "%s%u%6.6u\n", desc, 
			(unsigned)front, (unsigned)back);
	} else {
		return ssl_printf(ssl, "%s%lu\n", desc, (unsigned long)x);
	}
}

/** print mem stats */
static int
print_mem(RES* ssl, struct worker* worker, struct daemon* daemon,
	struct ub_stats_info* s)
{
	size_t msg, rrset, val, iter, respip;
#ifdef CLIENT_SUBNET
	size_t subnet = 0;
#endif /* CLIENT_SUBNET */
#ifdef USE_IPSECMOD
	size_t ipsecmod = 0;
#endif /* USE_IPSECMOD */
#ifdef USE_DNSCRYPT
	size_t dnscrypt_shared_secret = 0;
	size_t dnscrypt_nonce = 0;
#endif /* USE_DNSCRYPT */
#ifdef WITH_DYNLIBMODULE
    size_t dynlib = 0;
#endif /* WITH_DYNLIBMODULE */
	msg = slabhash_get_mem(daemon->env->msg_cache);
	rrset = slabhash_get_mem(&daemon->env->rrset_cache->table);
	val = mod_get_mem(&worker->env, "validator");
	iter = mod_get_mem(&worker->env, "iterator");
	respip = mod_get_mem(&worker->env, "respip");
#ifdef CLIENT_SUBNET
	subnet = mod_get_mem(&worker->env, "subnet");
#endif /* CLIENT_SUBNET */
#ifdef USE_IPSECMOD
	ipsecmod = mod_get_mem(&worker->env, "ipsecmod");
#endif /* USE_IPSECMOD */
#ifdef USE_DNSCRYPT
	if(daemon->dnscenv) {
		dnscrypt_shared_secret = slabhash_get_mem(
			daemon->dnscenv->shared_secrets_cache);
		dnscrypt_nonce = slabhash_get_mem(daemon->dnscenv->nonces_cache);
	}
#endif /* USE_DNSCRYPT */
#ifdef WITH_DYNLIBMODULE
    dynlib = mod_get_mem(&worker->env, "dynlib");
#endif /* WITH_DYNLIBMODULE */

	if(!print_longnum(ssl, "mem.cache.rrset"SQ, rrset))
		return 0;
	if(!print_longnum(ssl, "mem.cache.message"SQ, msg))
		return 0;
	if(!print_longnum(ssl, "mem.mod.iterator"SQ, iter))
		return 0;
	if(!print_longnum(ssl, "mem.mod.validator"SQ, val))
		return 0;
	if(!print_longnum(ssl, "mem.mod.respip"SQ, respip))
		return 0;
#ifdef CLIENT_SUBNET
	if(!print_longnum(ssl, "mem.mod.subnet"SQ, subnet))
		return 0;
#endif /* CLIENT_SUBNET */
#ifdef USE_IPSECMOD
	if(!print_longnum(ssl, "mem.mod.ipsecmod"SQ, ipsecmod))
		return 0;
#endif /* USE_IPSECMOD */
#ifdef USE_DNSCRYPT
	if(!print_longnum(ssl, "mem.cache.dnscrypt_shared_secret"SQ,
			dnscrypt_shared_secret))
		return 0;
	if(!print_longnum(ssl, "mem.cache.dnscrypt_nonce"SQ,
			dnscrypt_nonce))
		return 0;
#endif /* USE_DNSCRYPT */
#ifdef WITH_DYNLIBMODULE
	if(!print_longnum(ssl, "mem.mod.dynlibmod"SQ, dynlib))
		return 0;
#endif /* WITH_DYNLIBMODULE */
	if(!print_longnum(ssl, "mem.streamwait"SQ,
		(size_t)s->svr.mem_stream_wait))
		return 0;
	if(!print_longnum(ssl, "mem.http.query_buffer"SQ,
		(size_t)s->svr.mem_http2_query_buffer))
		return 0;
	if(!print_longnum(ssl, "mem.http.response_buffer"SQ,
		(size_t)s->svr.mem_http2_response_buffer))
		return 0;
	return 1;
}

/** print uptime stats */
static int
print_uptime(RES* ssl, struct worker* worker, int reset)
{
	struct timeval now = *worker->env.now_tv;
	struct timeval up, dt;
	timeval_subtract(&up, &now, &worker->daemon->time_boot);
	timeval_subtract(&dt, &now, &worker->daemon->time_last_stat);
	if(reset)
		worker->daemon->time_last_stat = now;
	if(!ssl_printf(ssl, "time.now"SQ ARG_LL "d.%6.6d\n", 
		(long long)now.tv_sec, (unsigned)now.tv_usec)) return 0;
	if(!ssl_printf(ssl, "time.up"SQ ARG_LL "d.%6.6d\n", 
		(long long)up.tv_sec, (unsigned)up.tv_usec)) return 0;
	if(!ssl_printf(ssl, "time.elapsed"SQ ARG_LL "d.%6.6d\n", 
		(long long)dt.tv_sec, (unsigned)dt.tv_usec)) return 0;
	return 1;
}

/** print extended histogram */
static int
print_hist(RES* ssl, struct ub_stats_info* s)
{
	struct timehist* hist;
	size_t i;
	hist = timehist_setup();
	if(!hist) {
		log_err("out of memory");
		return 0;
	}
	timehist_import(hist, s->svr.hist, NUM_BUCKETS_HIST);
	for(i=0; i<hist->num; i++) {
		if(!ssl_printf(ssl, 
			"histogram.%6.6d.%6.6d.to.%6.6d.%6.6d=%lu\n",
			(int)hist->buckets[i].lower.tv_sec,
			(int)hist->buckets[i].lower.tv_usec,
			(int)hist->buckets[i].upper.tv_sec,
			(int)hist->buckets[i].upper.tv_usec,
			(unsigned long)hist->buckets[i].count)) {
			timehist_delete(hist);
			return 0;
		}
	}
	timehist_delete(hist);
	return 1;
}

/** print extended stats */
static int
print_ext(RES* ssl, struct ub_stats_info* s)
{
	int i;
	char nm[32];
	const sldns_rr_descriptor* desc;
	const sldns_lookup_table* lt;
	/* TYPE */
	for(i=0; i<UB_STATS_QTYPE_NUM; i++) {
		if(inhibit_zero && s->svr.qtype[i] == 0)
			continue;
		desc = sldns_rr_descript((uint16_t)i);
		if(desc && desc->_name) {
			snprintf(nm, sizeof(nm), "%s", desc->_name);
		} else if (i == LDNS_RR_TYPE_IXFR) {
			snprintf(nm, sizeof(nm), "IXFR");
		} else if (i == LDNS_RR_TYPE_AXFR) {
			snprintf(nm, sizeof(nm), "AXFR");
		} else if (i == LDNS_RR_TYPE_MAILA) {
			snprintf(nm, sizeof(nm), "MAILA");
		} else if (i == LDNS_RR_TYPE_MAILB) {
			snprintf(nm, sizeof(nm), "MAILB");
		} else if (i == LDNS_RR_TYPE_ANY) {
			snprintf(nm, sizeof(nm), "ANY");
		} else {
			snprintf(nm, sizeof(nm), "TYPE%d", i);
		}
		if(!ssl_printf(ssl, "num.query.type.%s"SQ"%lu\n", 
			nm, (unsigned long)s->svr.qtype[i])) return 0;
	}
	if(!inhibit_zero || s->svr.qtype_big) {
		if(!ssl_printf(ssl, "num.query.type.other"SQ"%lu\n", 
			(unsigned long)s->svr.qtype_big)) return 0;
	}
	/* CLASS */
	for(i=0; i<UB_STATS_QCLASS_NUM; i++) {
		if(inhibit_zero && s->svr.qclass[i] == 0)
			continue;
		lt = sldns_lookup_by_id(sldns_rr_classes, i);
		if(lt && lt->name) {
			snprintf(nm, sizeof(nm), "%s", lt->name);
		} else {
			snprintf(nm, sizeof(nm), "CLASS%d", i);
		}
		if(!ssl_printf(ssl, "num.query.class.%s"SQ"%lu\n", 
			nm, (unsigned long)s->svr.qclass[i])) return 0;
	}
	if(!inhibit_zero || s->svr.qclass_big) {
		if(!ssl_printf(ssl, "num.query.class.other"SQ"%lu\n", 
			(unsigned long)s->svr.qclass_big)) return 0;
	}
	/* OPCODE */
	for(i=0; i<UB_STATS_OPCODE_NUM; i++) {
		if(inhibit_zero && s->svr.qopcode[i] == 0)
			continue;
		lt = sldns_lookup_by_id(sldns_opcodes, i);
		if(lt && lt->name) {
			snprintf(nm, sizeof(nm), "%s", lt->name);
		} else {
			snprintf(nm, sizeof(nm), "OPCODE%d", i);
		}
		if(!ssl_printf(ssl, "num.query.opcode.%s"SQ"%lu\n", 
			nm, (unsigned long)s->svr.qopcode[i])) return 0;
	}
	/* transport */
	if(!ssl_printf(ssl, "num.query.tcp"SQ"%lu\n", 
		(unsigned long)s->svr.qtcp)) return 0;
	if(!ssl_printf(ssl, "num.query.tcpout"SQ"%lu\n", 
		(unsigned long)s->svr.qtcp_outgoing)) return 0;
	if(!ssl_printf(ssl, "num.query.tls"SQ"%lu\n", 
		(unsigned long)s->svr.qtls)) return 0;
	if(!ssl_printf(ssl, "num.query.tls.resume"SQ"%lu\n", 
		(unsigned long)s->svr.qtls_resume)) return 0;
	if(!ssl_printf(ssl, "num.query.ipv6"SQ"%lu\n", 
		(unsigned long)s->svr.qipv6)) return 0;
	if(!ssl_printf(ssl, "num.query.https"SQ"%lu\n",
		(unsigned long)s->svr.qhttps)) return 0;
	/* flags */
	if(!ssl_printf(ssl, "num.query.flags.QR"SQ"%lu\n", 
		(unsigned long)s->svr.qbit_QR)) return 0;
	if(!ssl_printf(ssl, "num.query.flags.AA"SQ"%lu\n", 
		(unsigned long)s->svr.qbit_AA)) return 0;
	if(!ssl_printf(ssl, "num.query.flags.TC"SQ"%lu\n", 
		(unsigned long)s->svr.qbit_TC)) return 0;
	if(!ssl_printf(ssl, "num.query.flags.RD"SQ"%lu\n", 
		(unsigned long)s->svr.qbit_RD)) return 0;
	if(!ssl_printf(ssl, "num.query.flags.RA"SQ"%lu\n", 
		(unsigned long)s->svr.qbit_RA)) return 0;
	if(!ssl_printf(ssl, "num.query.flags.Z"SQ"%lu\n", 
		(unsigned long)s->svr.qbit_Z)) return 0;
	if(!ssl_printf(ssl, "num.query.flags.AD"SQ"%lu\n", 
		(unsigned long)s->svr.qbit_AD)) return 0;
	if(!ssl_printf(ssl, "num.query.flags.CD"SQ"%lu\n", 
		(unsigned long)s->svr.qbit_CD)) return 0;
	if(!ssl_printf(ssl, "num.query.edns.present"SQ"%lu\n", 
		(unsigned long)s->svr.qEDNS)) return 0;
	if(!ssl_printf(ssl, "num.query.edns.DO"SQ"%lu\n", 
		(unsigned long)s->svr.qEDNS_DO)) return 0;

	/* RCODE */
	for(i=0; i<UB_STATS_RCODE_NUM; i++) {
		/* Always include RCODEs 0-5 */
		if(inhibit_zero && i > LDNS_RCODE_REFUSED && s->svr.ans_rcode[i] == 0)
			continue;
		lt = sldns_lookup_by_id(sldns_rcodes, i);
		if(lt && lt->name) {
			snprintf(nm, sizeof(nm), "%s", lt->name);
		} else {
			snprintf(nm, sizeof(nm), "RCODE%d", i);
		}
		if(!ssl_printf(ssl, "num.answer.rcode.%s"SQ"%lu\n", 
			nm, (unsigned long)s->svr.ans_rcode[i])) return 0;
	}
	if(!inhibit_zero || s->svr.ans_rcode_nodata) {
		if(!ssl_printf(ssl, "num.answer.rcode.nodata"SQ"%lu\n", 
			(unsigned long)s->svr.ans_rcode_nodata)) return 0;
	}
	/* iteration */
	if(!ssl_printf(ssl, "num.query.ratelimited"SQ"%lu\n", 
		(unsigned long)s->svr.queries_ratelimited)) return 0;
	/* validation */
	if(!ssl_printf(ssl, "num.answer.secure"SQ"%lu\n", 
		(unsigned long)s->svr.ans_secure)) return 0;
	if(!ssl_printf(ssl, "num.answer.bogus"SQ"%lu\n", 
		(unsigned long)s->svr.ans_bogus)) return 0;
	if(!ssl_printf(ssl, "num.rrset.bogus"SQ"%lu\n", 
		(unsigned long)s->svr.rrset_bogus)) return 0;
	if(!ssl_printf(ssl, "num.query.aggressive.NOERROR"SQ"%lu\n", 
		(unsigned long)s->svr.num_neg_cache_noerror)) return 0;
	if(!ssl_printf(ssl, "num.query.aggressive.NXDOMAIN"SQ"%lu\n", 
		(unsigned long)s->svr.num_neg_cache_nxdomain)) return 0;
	/* threat detection */
	if(!ssl_printf(ssl, "unwanted.queries"SQ"%lu\n", 
		(unsigned long)s->svr.unwanted_queries)) return 0;
	if(!ssl_printf(ssl, "unwanted.replies"SQ"%lu\n", 
		(unsigned long)s->svr.unwanted_replies)) return 0;
	/* cache counts */
	if(!ssl_printf(ssl, "msg.cache.count"SQ"%u\n",
		(unsigned)s->svr.msg_cache_count)) return 0;
	if(!ssl_printf(ssl, "rrset.cache.count"SQ"%u\n",
		(unsigned)s->svr.rrset_cache_count)) return 0;
	if(!ssl_printf(ssl, "infra.cache.count"SQ"%u\n",
		(unsigned)s->svr.infra_cache_count)) return 0;
	if(!ssl_printf(ssl, "key.cache.count"SQ"%u\n",
		(unsigned)s->svr.key_cache_count)) return 0;
	/* applied RPZ actions */
	for(i=0; i<UB_STATS_RPZ_ACTION_NUM; i++) {
		if(i == RPZ_NO_OVERRIDE_ACTION)
			continue;
		if(inhibit_zero && s->svr.rpz_action[i] == 0)
			continue;
		if(!ssl_printf(ssl, "num.rpz.action.%s"SQ"%lu\n",
			rpz_action_to_string(i),
			(unsigned long)s->svr.rpz_action[i])) return 0;
	}
#ifdef USE_DNSCRYPT
	if(!ssl_printf(ssl, "dnscrypt_shared_secret.cache.count"SQ"%u\n",
		(unsigned)s->svr.shared_secret_cache_count)) return 0;
	if(!ssl_printf(ssl, "dnscrypt_nonce.cache.count"SQ"%u\n",
		(unsigned)s->svr.nonce_cache_count)) return 0;
	if(!ssl_printf(ssl, "num.query.dnscrypt.shared_secret.cachemiss"SQ"%lu\n",
		(unsigned long)s->svr.num_query_dnscrypt_secret_missed_cache)) return 0;
	if(!ssl_printf(ssl, "num.query.dnscrypt.replay"SQ"%lu\n",
		(unsigned long)s->svr.num_query_dnscrypt_replay)) return 0;
#endif /* USE_DNSCRYPT */
	if(!ssl_printf(ssl, "num.query.authzone.up"SQ"%lu\n",
		(unsigned long)s->svr.num_query_authzone_up)) return 0;
	if(!ssl_printf(ssl, "num.query.authzone.down"SQ"%lu\n",
		(unsigned long)s->svr.num_query_authzone_down)) return 0;
#ifdef CLIENT_SUBNET
	if(!ssl_printf(ssl, "num.query.subnet"SQ"%lu\n",
		(unsigned long)s->svr.num_query_subnet)) return 0;
	if(!ssl_printf(ssl, "num.query.subnet_cache"SQ"%lu\n",
		(unsigned long)s->svr.num_query_subnet_cache)) return 0;
#endif /* CLIENT_SUBNET */
	return 1;
}

/** print the accumlated stats for the specified view */

static void
do_view_stats(RES* ssl,
              struct view* v,
              struct view_stats* vstats)
{
	char tag[strlen(v->name) + 7];

	sprintf(tag, "view.%s.", v->name);

	long long nq = vstats->num_queries;
	long long nm = vstats->num_queries_missed_cache;

	ssl_printf(ssl, "%snum.queries" SQ ARG_LL "u\n", tag, nq);
	ssl_printf(ssl, "%snum.queries_ip_ratelimited" SQ ARG_LL "u\n",
	                tag,
	                vstats->num_queries_ip_ratelimited);
	ssl_printf(ssl, "%snum.cachehits" SQ ARG_LL "u\n", tag, nq - nm);
	ssl_printf(ssl, "%snum.cachemiss" SQ ARG_LL "u\n", tag, nm);
	ssl_printf(ssl, "%snum.prefetch" SQ ARG_LL "u\n",
	                tag,
	                vstats->num_queries_prefetch);
	ssl_printf(ssl, "%snum.expired" SQ ARG_LL "u\n",
	                tag,
	                vstats->ans_expired);

	if ((v->view_flags & VIEW_FLAG_SHARE_CACHE) == 0) {
		ssl_printf(ssl, "%smem.cache.rrset" SQ "%lu\n",
	                    tag,
		                slabhash_get_mem(&v->rrset_cache->table));
		ssl_printf(ssl, "%smem.cache.message" SQ "%lu\n",
	                    tag,
		                slabhash_get_mem(v->msg_cache));
		ssl_printf(ssl, "%srrset.cache.count" SQ "%lu\n",
	                    tag,
		                count_slabhash_entries(&v->rrset_cache->table));
		ssl_printf(ssl, "%smsg.cache.count" SQ "%lu\n",
	                    tag,
		                count_slabhash_entries(v->msg_cache));
	}
}

/** do the stats command */
static void
do_server_stats(RES* ssl,
                struct worker* worker,
                int reset)
{
	struct daemon* daemon = worker->daemon;
	struct views* vs = worker->daemon->views;
	struct ub_stats_info total;
	struct ub_stats_info s;
	struct view_stats vstats[vs->vtree.count];
	int i;

	memset(&total, 0, sizeof(total));
	memset(vstats, 0, sizeof(vstats));

	log_assert(daemon->num > 0);
	/* gather all thread statistics in one place */
	for(i=0; i<daemon->num; i++) {
		server_stats_obtain(worker, daemon->workers[i], &s, vstats, reset);

		if(!print_thread_stats(ssl, i, &s))
			return;
		if(i == 0)
			total = s;
		else	server_stats_add(&total, &s);
	}
	/* print the thread statistics */
	total.mesh_time_median /= (double)daemon->num;
	if(!print_stats(ssl, "total", &total)) 
		return;
	if(!print_uptime(ssl, worker, reset))
		return;
	if(daemon->cfg->stat_extended) {
		if(!print_mem(ssl, worker, daemon, &total))
			return;
		if(!print_hist(ssl, &total))
			return;
		if(!print_ext(ssl, &total))
			return;
	}

	struct view_node *vn;

	i = 0;

	RBTREE_FOR(vn, struct view_node *, &vs->vtree) {
		do_view_stats(ssl, &vn->vinfo, &vstats[i++]);
	}
}

/** Sum the per-view stats across the threads - clear if indicated
 ** Note that we have to request the other threads to clear as there
 ** would be race conditions otherwise
 */
static void
view_stats_sum(struct view *v, struct view_stats *s)
{
	struct view_stats *vs = v->view_stats;
	unsigned int n = v->view_threads;
	unsigned int t;

	long long queries  = 0;
	long long ratelim  = 0;
	long long missed   = 0;
	long long prefetch = 0;
	long long expired  = 0;

	for (t = 0; t < n; t++, vs++) {
		queries  += vs->num_queries;
		ratelim  += vs->num_queries_ip_ratelimited;
		missed   += vs->num_queries_missed_cache;
		prefetch += vs->num_queries_prefetch;
		expired  += vs->ans_expired;
	}

	s->num_queries                = queries;
	s->num_queries_ip_ratelimited = ratelim;
	s->num_queries_missed_cache   = missed;
	s->num_queries_prefetch       = prefetch;
	s->ans_expired                = expired;
}

static void
do_stats(RES* ssl,
         struct worker* worker,
         struct view* v,
         int reset)
{
	if (view_is_server(v)) {
		// For the server, add the stats for each configure view

		do_server_stats(ssl, worker, reset);
	} else if (reset) {
		ssl_printf(ssl, "Clearing stats not supported for a view\n");
	} else {
		struct view_stats vs;

		view_stats_sum(v, &vs);
		do_view_stats(ssl, v, &vs);
	}
}

static void
do_stats_reset(RES* ssl,
               struct worker* worker,
               struct view* v,
               char* argp)
{
	// Only reset when statistics-cumulative is not set

	do_stats(ssl, worker, v, worker->env.cfg->stat_cumulative == 0);
}

static void
do_stats_noreset(RES* ssl,
                 struct worker* worker,
                 struct view* v,
                 char* argp)
{
	do_stats(ssl, worker, v, 0);
}

/** parse commandline argument domain name */
static int
parse_arg_name(RES* ssl, char* str, uint8_t** res, size_t* len, int* labs)
{
	uint8_t nm[LDNS_MAX_DOMAINLEN+1];
	size_t nmlen = sizeof(nm);
	int status;
	*res = NULL;
	*len = 0;
	*labs = 0;
	if(str[0] == '\0') {
		ssl_printf(ssl, "error: this option requires a domain name\n");
		return 0;
	}
	status = sldns_str2wire_dname_buf(str, nm, &nmlen);
	if(status != 0) {
		ssl_printf(ssl, "error cannot parse name %s at %d: %s\n", str,
			LDNS_WIREPARSE_OFFSET(status),
			sldns_get_errorstr_parse(status));
		return 0;
	}
	*res = memdup(nm, nmlen);
	if(!*res) {
		ssl_printf(ssl, "error out of memory\n");
		return 0;
	}
	*labs = dname_count_size_labels(*res, len);
	return 1;
}

/** skip whitespace, return new pointer into string */
static char*
skipwhite(char* str)
{
	/* EOS \0 is not a space */
	while( isspace((unsigned char)*str) ) 
		str++;
	return str;
}

// Scan the next argument in the string and NUL terminate. arg must point to
// the beginning of an argument or the end of a string. Save the start of the
// next argument in next
//
// Return 0 and send a diatgnostic if no argument was found; otherwise 
// If no argument found when expected, send a diagnostic

static int
find_arg2(RES* ssl, char* arg, char** next)
{
	if (arg[0] == '\0') {
		ssl_printf(ssl, "error could not find next argument\n");
		return (0);
	}

	char c;

	// Find the end of the token

	while ((c = *arg) != '\0' && c != ' ' && c != '\t') {
		arg++;
	}

	if (c != '\0') {
		*arg = '\0';

		arg = skipwhite(arg + 1);
	}

	*next = arg;
	return (1);
}

static struct view *
check_view_arg(RES* ssl,
               struct worker* worker,
               struct view* v,
               char** argp)
{
	char* vname = *argp;
	char* arg2;

	if (find_arg2(ssl, vname, &arg2)) {
		struct view* varg;

		varg = views_find_view(worker->daemon->views, vname, 0);

		// If we cannot find the specified view name, or the named view
		// differs from the command view and the command view is not the
		// default view, complain

		if (varg == NULL) {
			ssl_printf(ssl,"no view with name: %s\n", vname);
		} else if (varg != v && !view_is_server(v)) {
			lock_rw_unlock(&varg->lock);
			ssl_printf(ssl,"conflicting view specified: %s\n", vname);
		} else {
			lock_rw_unlock(&varg->lock);

			// Set the argument pointer to the rest of the command

			*argp = arg2;
			return (varg);
		}
	}

	return (NULL);
}

/** Add a new zone */
static int
perform_zone_add(RES* ssl, struct local_zones* zones, char* arg)
{
	uint8_t* nm;
	int nmlabs;
	size_t nmlen;
	char* arg2;
	enum localzone_type t;
	struct local_zone* z;
	if(!find_arg2(ssl, arg, &arg2))
		return 0;
	if(!parse_arg_name(ssl, arg, &nm, &nmlen, &nmlabs))
		return 0;
	if(!local_zone_str2type(arg2, &t)) {
		ssl_printf(ssl, "error not a zone type. %s\n", arg2);
		free(nm);
		return 0;
	}

	lock_rw_wrlock(&zones->lock);

	if((z=local_zones_find(zones, nm, nmlen, nmlabs, LDNS_RR_CLASS_IN))) {
		/* already present in tree */
		lock_rw_wrlock(&z->lock);
		z->type = t; /* update type anyway */
		lock_rw_unlock(&z->lock);
		free(nm);
		lock_rw_unlock(&zones->lock);
		return 1;
	}
	if(!local_zones_add_zone(zones, nm, nmlen, nmlabs, LDNS_RR_CLASS_IN, t)) {
		lock_rw_unlock(&zones->lock);
		ssl_printf(ssl, "error out of memory\n");
		return 0;
	}

	lock_rw_unlock(&zones->lock);
	return 1;
}

typedef int (*local_info_add)(RES *, struct view *, char *);

static void
local_from_client(RES* ssl, struct view* v, char *fmt, local_info_add fnc)
{
	char buf[2048];
	int num = 0;

	while (ssl_read_line(ssl, buf, sizeof(buf))) {
		if (buf[0] == 0x04 && buf[1] == 0)
			break; /* end of transmission */
		if ((*fnc)(ssl, v, buf)) {
			num++;
		} else if (!ssl_printf(ssl, "error for input line: %s\n", buf)) {
			return;
		}
	}

	(void)ssl_printf(ssl, fmt, num);
}

static int
zone_add(RES* ssl, struct view* v, char* zone_info)
{
	lock_rw_wrlock(&v->lock);

	if (v->local_zones == NULL) {
		if ((v->local_zones = local_zones_create()) == NULL) {
			lock_rw_unlock(&v->lock);
			ssl_printf(ssl, "error out of memory\n");
			return 0;
		}
		if (v->server_zones == NULL) {
			/* Global local-zone is not used for this view,
			 * therefore add defaults to this view-specic
			 * local-zone. */
			struct config_file lz_cfg;

			memset(&lz_cfg, 0, sizeof(lz_cfg));
			local_zone_enter_defaults(v->local_zones, &lz_cfg);
		}
	}

	int ok = perform_zone_add(ssl, v->local_zones, zone_info);

	lock_rw_unlock(&v->lock);

	return ok;
}

/** Do the local_zone command */
static void
do_zone_add(RES* ssl,
            struct worker* worker,
            struct view* v,
            char* argp)
{
	if (zone_add(ssl, v, argp)) {
		send_ok(ssl);
	}
}

/** Add a new zone to view */
static void
do_view_zone_add(RES* ssl,
                 struct worker* worker,
                 struct view* v,
                 char* argp)
{
	if ((v = check_view_arg(ssl, worker, v, &argp)) != NULL) {
		if (zone_add(ssl, v, argp)) {
			send_ok(ssl);
		}
	}
}

/** Do the local_zones command */
static void
do_zones_add(RES* ssl,
             struct worker* worker,
             struct view* v,
             char* argp)
{
	local_from_client(ssl, v, "added %d zones\n", zone_add);
}

/** Remove a zone */
static int
perform_zone_remove(RES* ssl, struct local_zones* zones, char* arg)
{
	uint8_t* nm;
	int nmlabs;
	size_t nmlen;
	struct local_zone* z;
	if(!parse_arg_name(ssl, arg, &nm, &nmlen, &nmlabs))
		return 0;
	lock_rw_wrlock(&zones->lock);
	if((z=local_zones_find(zones, nm, nmlen, 
		nmlabs, LDNS_RR_CLASS_IN))) {
		/* present in tree */
		local_zones_del_zone(zones, z);
	}
	lock_rw_unlock(&zones->lock);
	free(nm);
	return 1;
}

/** Do the local_zone_remove command */
static int
zone_remove(RES* ssl, struct view* v, char* zone_name)
{
	lock_rw_wrlock(&v->lock);

	int ok;

	ok = (v->local_zones == NULL) ||
	        perform_zone_remove(ssl, v->local_zones, zone_name);

	lock_rw_unlock(&v->lock);
	return (ok);
}

/** Do the local_zone_remove command */
static void
do_zone_remove(RES* ssl,
               struct worker* worker,
               struct view* v,
               char* argp)
{
	if (zone_remove(ssl, v, argp)) {
		send_ok(ssl);
	}
}

/** Remove a zone from view */
static void
do_view_zone_remove(RES* ssl,
                    struct worker* worker,
                    struct view* v,
                    char* argp)
{
	if ((v = check_view_arg(ssl, worker, v, &argp)) == NULL) {
		return;
	}
	if (zone_remove(ssl, v, argp)) {
		send_ok(ssl);
	}
}

/** Do the local_zones_remove command */
static void
do_zones_remove(RES* ssl,
                struct worker* worker,
                struct view* v,
                char* argp)
{
	local_from_client(ssl, v, "removed %d zones\n", zone_remove);
}

/** Add new RR data */
static int
perform_data_add(RES* ssl, struct local_zones* zones, char* arg)
{
	if (local_zones_add_RR(zones, arg)) {
		return 1;
	}

	ssl_printf(ssl,"error in syntax or out of memory, %s\n", arg);
	return 0;
}

static int
data_add(RES* ssl, struct view* v, char* zone_data)
{
	lock_rw_wrlock(&v->lock);

	if (v->local_zones == NULL) {
		if ((v->local_zones = local_zones_create()) == NULL) {
			lock_rw_unlock(&v->lock);
			ssl_printf(ssl,"error out of memory\n");
			return 0;
		}
	}

	int ok = perform_data_add(ssl, v->local_zones, zone_data);

	lock_rw_unlock(&v->lock);

	return ok;
}

/** Do the local_data command */
static void
do_data_add(RES* ssl,
            struct worker* worker,
            struct view* v,
            char* argp)
{
	if (data_add(ssl, v, argp)) {
		send_ok(ssl);
	}
}

/** Add new RR data to view */
static void
do_view_data_add(RES* ssl,
                 struct worker* worker,
                 struct view* v,
                 char* argp)
{
	if ((v = check_view_arg(ssl, worker, v, &argp)) == NULL) {
		return;
	}
	if (data_add(ssl, v, argp)) {
		send_ok(ssl);
	}
}

static void
datas_add(RES* ssl, struct view *v)
{
	local_from_client(ssl, v, "added %d datas\n", data_add);
}

/** Add new RR data from stdin to view */
static void
do_view_datas_add(RES* ssl,
                  struct worker* worker,
                  struct view* v,
                  char* argp)
{
	if ((v = check_view_arg(ssl, worker, v, &argp)) == NULL) {
		return;
	}

	datas_add(ssl, v);
	lock_rw_unlock(&v->lock);
}

/** Do the local_datas command */
static void
do_datas_add(RES* ssl,
             struct worker* worker,
             struct view* v,
             char* argp)
{
	datas_add(ssl, v);
}

/** Remove RR data */
static int
perform_data_remove(RES* ssl, struct local_zones* zones, char* arg)
{
	uint8_t* nm;
	int nmlabs;
	size_t nmlen;
	if(!parse_arg_name(ssl, arg, &nm, &nmlen, &nmlabs))
		return 0;
	local_zones_del_data(zones, nm,
		nmlen, nmlabs, LDNS_RR_CLASS_IN);
	free(nm);
	return 1;
}

static int
data_remove(RES* ssl, struct view* v, char* zone_data)
{
	lock_rw_wrlock(&v->lock);

	int ok;

	ok = (v->local_zones == NULL) ||
	        perform_data_remove(ssl, v->local_zones, zone_data);

	lock_rw_unlock(&v->lock);
	return (ok);
}

/** Do the local_data_remove command */
static void
do_data_remove(RES* ssl,
               struct worker* worker,
               struct view* v,
               char* argp)
{
	if (data_remove(ssl, v, argp)) {
		send_ok(ssl);
	}
}

static void
datas_remove(RES* ssl, struct view* v)
{
	local_from_client(ssl, v, "removed %d datas\n", data_remove);
}

/** Do the local_datas_remove command */
static void
do_datas_remove(RES* ssl,
                struct worker* worker,
                struct view* v,
                char* argp)
{
	datas_remove(ssl, v);
}

/** Remove RR data from view */
static void
do_view_data_remove(RES* ssl,
                    struct worker* worker,
                    struct view* v,
                    char* argp)
{
	if ((v = check_view_arg(ssl, worker, v, &argp)) == NULL) {
		return;
	}
	if (data_remove(ssl, v, argp)) {
		send_ok(ssl);
	}
}

/** Remove RR data from stdin from view */
static void
do_view_datas_remove(RES* ssl,
                     struct worker* worker,
                     struct view* v,
                     char* argp)
{
	if ((v = check_view_arg(ssl, worker, v, &argp)) == NULL) {
		return;
	}

	datas_remove(ssl, v);
}

/** cache lookup of nameservers */
static void
do_lookup(RES* ssl,
          struct worker* worker,
          struct view* v,
          char* argp)
{
	if (view_is_valid(ssl, v)) {
		size_t nmlen;
		uint8_t* nm;
		int nmlabs;

		if(parse_arg_name(ssl, argp, &nm, &nmlen, &nmlabs)) {
			(void)print_deleg_lookup(ssl, worker, v, nm, nmlen, nmlabs);
			free(nm);
		}
	}
}

/** flush something from rrset and msg caches */
static void
do_cache_remove(struct view* v,
                uint8_t* nm, size_t nmlen,
                uint16_t t, uint16_t c)
{
	hashvalue_type h;
	struct query_info k;
	rrset_cache_remove(v->rrset_cache, nm, nmlen, t, c, 0);
	if(t == LDNS_RR_TYPE_SOA)
		rrset_cache_remove(v->rrset_cache, nm, nmlen, t, c,
			PACKED_RRSET_SOA_NEG);
	k.qname = nm;
	k.qname_len = nmlen;
	k.qtype = t;
	k.qclass = c;
	k.local_alias = NULL;
	h = query_info_hash(&k, 0);
	slabhash_remove(v->msg_cache, h, &k);
	if(t == LDNS_RR_TYPE_AAAA) {
		/* for AAAA also flush dns64 bit_cd packet */
		h = query_info_hash(&k, BIT_CD);
		slabhash_remove(v->msg_cache, h, &k);
	}
}

/** flush a type */
static void
do_flush_type(RES* ssl,
              struct worker* worker,
              struct view* v,
              char* argp)
{
	if (view_has_caches(ssl, v)) {
		size_t nmlen;
		uint8_t* nm;
		int nmlabs;
		uint16_t t;
		char* arg2;

		if(!find_arg2(ssl, argp, &arg2))
			return;
		if(!parse_arg_name(ssl, argp, &nm, &nmlen, &nmlabs))
			return;
		t = sldns_get_rr_type_by_name(arg2);
		do_cache_remove(v, nm, nmlen, t, LDNS_RR_CLASS_IN);
		
		free(nm);
		send_ok(ssl);
	}
}

/** flush statistics */
static void
do_flush_stats(RES* ssl,
               struct worker* worker,
               struct view* v,
               char* argp)
{
	worker_stats_clear(worker);
	send_ok(ssl);
}

/**
 * Local info for deletion functions
 */
struct del_info {
	/** name to delete */
	uint8_t* name;
	/** length */
	size_t len;
	/** labels */
	int labs;
	/** time to invalidate to */
	time_t expired;
	/** number of rrsets removed */
	size_t num_rrsets;
	/** number of msgs removed */
	size_t num_msgs;
	/** number of key entries removed */
	size_t num_keys;
	/** length of addr */
	socklen_t addrlen;
	/** socket address for host deletion */
	struct sockaddr_storage addr;
};

/** callback to delete hosts in infra cache */
static void
infra_del_host(struct lruhash_entry* e, void* arg)
{
	/* entry is locked */
	struct del_info* inf = (struct del_info*)arg;
	struct infra_key* k = (struct infra_key*)e->key;
	if(sockaddr_cmp(&inf->addr, inf->addrlen, &k->addr, k->addrlen) == 0) {
		struct infra_data* d = (struct infra_data*)e->data;
		d->probedelay = 0;
		d->timeout_A = 0;
		d->timeout_AAAA = 0;
		d->timeout_other = 0;
		rtt_init(&d->rtt);
		if(d->ttl > inf->expired) {
			d->ttl = inf->expired;
			inf->num_keys++;
		}
	}
}

/** flush infra cache */
static void
do_flush_infra(RES* ssl,
               struct worker* worker,
               struct view* v,
               char* argp)
{
	if (!view_is_valid(ssl, v)) {
		return;
	}

	struct sockaddr_storage addr;
	socklen_t len;
	struct del_info inf;
	if(strcmp(argp, "all") == 0) {
		slabhash_clear(worker->env.infra_cache->hosts);
		send_ok(ssl);
		return;
	}
	if(!ipstrtoaddr(argp, UNBOUND_DNS_PORT, &addr, &len)) {
		(void)ssl_printf(ssl, "error parsing ip addr: '%s'\n", argp);
		return;
	}
	/* delete all entries from cache */
	/* what we do is to set them all expired */
	inf.name = 0;
	inf.len = 0;
	inf.labs = 0;
	inf.expired = *worker->env.now;
	inf.expired -= 3; /* handle 3 seconds skew between threads */
	inf.num_rrsets = 0;
	inf.num_msgs = 0;
	inf.num_keys = 0;
	inf.addrlen = len;
	memmove(&inf.addr, &addr, len);
	slabhash_traverse(worker->env.infra_cache->hosts, 1, &infra_del_host,
		&inf);
	send_ok(ssl);
}

/** flush requestlist */
static void
do_flush_requestlist(RES* ssl,
                     struct worker* worker,
                     struct view* v,
                     char* argp)
{
	if (view_is_valid(ssl, v)) {
		mesh_delete_all(worker->env.mesh);
		send_ok(ssl);
	}
}

/** callback to delete rrsets in a zone */
static void
zone_del_rrset(struct lruhash_entry* e, void* arg)
{
	/* entry is locked */
	struct del_info* inf = (struct del_info*)arg;
	struct ub_packed_rrset_key* k = (struct ub_packed_rrset_key*)e->key;
	if(dname_subdomain_c(k->rk.dname, inf->name)) {
		struct packed_rrset_data* d = 
			(struct packed_rrset_data*)e->data;
		if(d->ttl > inf->expired) {
			d->ttl = inf->expired;
			inf->num_rrsets++;
		}
	}
}

/** callback to delete messages in a zone */
static void
zone_del_msg(struct lruhash_entry* e, void* arg)
{
	/* entry is locked */
	struct del_info* inf = (struct del_info*)arg;
	struct msgreply_entry* k = (struct msgreply_entry*)e->key;
	if(dname_subdomain_c(k->key.qname, inf->name)) {
		struct reply_info* d = (struct reply_info*)e->data;
		if(d->ttl > inf->expired) {
			d->ttl = inf->expired;
			d->prefetch_ttl = inf->expired;
			d->serve_expired_ttl = inf->expired;
			inf->num_msgs++;
		}
	}
}

/** callback to delete keys in zone */
static void
zone_del_kcache(struct lruhash_entry* e, void* arg)
{
	/* entry is locked */
	struct del_info* inf = (struct del_info*)arg;
	struct key_entry_key* k = (struct key_entry_key*)e->key;
	if(dname_subdomain_c(k->name, inf->name)) {
		struct key_entry_data* d = (struct key_entry_data*)e->data;
		if(d->ttl > inf->expired) {
			d->ttl = inf->expired;
			inf->num_keys++;
		}
	}
}

/** remove all rrsets and keys from zone from cache */
static void
do_flush_zone(RES* ssl,
              struct worker* worker,
              struct view* v,
              char* argp)
{
	if (!view_has_caches(ssl, v)) {
		return;
	}

	uint8_t* nm;
	int nmlabs;
	size_t nmlen;
	struct del_info inf;
	if(!parse_arg_name(ssl, argp, &nm, &nmlen, &nmlabs))
		return;
	/* delete all RRs and key entries from zone */
	/* what we do is to set them all expired */
	inf.name = nm;
	inf.len = nmlen;
	inf.labs = nmlabs;
	inf.expired = *worker->env.now;
	inf.expired -= 3; /* handle 3 seconds skew between threads */
	inf.num_rrsets = 0;
	inf.num_msgs = 0;
	inf.num_keys = 0;

	slabhash_traverse(&v->rrset_cache->table, 1, &zone_del_rrset, &inf);
	slabhash_traverse(v->msg_cache, 1, &zone_del_msg, &inf);

	/* and validator cache, if not in a view */
	if(view_is_server(v) && worker->env.key_cache) {
		slabhash_traverse(worker->env.key_cache->slab,
		                  1, 
		                  &zone_del_kcache,
	                      &inf);
	}

	free(nm);

	(void)ssl_printf(ssl, "ok removed %lu rrsets, %lu messages "
		"and %lu key entries\n", (unsigned long)inf.num_rrsets, 
		(unsigned long)inf.num_msgs, (unsigned long)inf.num_keys);
}

/** callback to delete bogus rrsets */
static void
bogus_del_rrset(struct lruhash_entry* e, void* arg)
{
	/* entry is locked */
	struct del_info* inf = (struct del_info*)arg;
	struct packed_rrset_data* d = (struct packed_rrset_data*)e->data;
	if(d->security == sec_status_bogus) {
		d->ttl = inf->expired;
		inf->num_rrsets++;
	}
}

/** callback to delete bogus messages */
static void
bogus_del_msg(struct lruhash_entry* e, void* arg)
{
	/* entry is locked */
	struct del_info* inf = (struct del_info*)arg;
	struct reply_info* d = (struct reply_info*)e->data;
	if(d->security == sec_status_bogus) {
		d->ttl = inf->expired;
		inf->num_msgs++;
	}
}

/** callback to delete bogus keys */
static void
bogus_del_kcache(struct lruhash_entry* e, void* arg)
{
	/* entry is locked */
	struct del_info* inf = (struct del_info*)arg;
	struct key_entry_data* d = (struct key_entry_data*)e->data;
	if(d->isbad) {
		d->ttl = inf->expired;
		inf->num_keys++;
	}
}

/** remove all bogus rrsets, msgs and keys from cache */
static void
do_flush_bogus(RES* ssl,
               struct worker* worker,
               struct view* v,
               char* argp)
{
	if (!view_has_caches(ssl, v)) {
		return;
	}

	struct del_info inf;
	/* what we do is to set them all expired */
	inf.expired = *worker->env.now;
	inf.expired -= 3; /* handle 3 seconds skew between threads */
	inf.num_rrsets = 0;
	inf.num_msgs = 0;
	inf.num_keys = 0;

	slabhash_traverse(&v->rrset_cache->table, 1, &bogus_del_rrset, &inf);
	slabhash_traverse(v->msg_cache, 1, &bogus_del_msg, &inf);

	/* and validator cache */
	if(view_is_server(v) && worker->env.key_cache) {
		slabhash_traverse(worker->env.key_cache->slab, 1, 
			&bogus_del_kcache, &inf);
	}

	(void)ssl_printf(ssl, "ok removed %lu rrsets, %lu messages "
		"and %lu key entries\n", (unsigned long)inf.num_rrsets, 
		(unsigned long)inf.num_msgs, (unsigned long)inf.num_keys);
}

/** callback to delete negative and servfail rrsets */
static void
negative_del_rrset(struct lruhash_entry* e, void* arg)
{
	/* entry is locked */
	struct del_info* inf = (struct del_info*)arg;
	struct ub_packed_rrset_key* k = (struct ub_packed_rrset_key*)e->key;
	struct packed_rrset_data* d = (struct packed_rrset_data*)e->data;
	/* delete the parentside negative cache rrsets,
	 * these are nameserver rrsets that failed lookup, rdata empty */
	if((k->rk.flags & PACKED_RRSET_PARENT_SIDE) && d->count == 1 &&
		d->rrsig_count == 0 && d->rr_len[0] == 0) {
		d->ttl = inf->expired;
		inf->num_rrsets++;
	}
}

/** callback to delete negative and servfail messages */
static void
negative_del_msg(struct lruhash_entry* e, void* arg)
{
	/* entry is locked */
	struct del_info* inf = (struct del_info*)arg;
	struct reply_info* d = (struct reply_info*)e->data;
	/* rcode not NOERROR: NXDOMAIN, SERVFAIL, ..: an nxdomain or error
	 * or NOERROR rcode with ANCOUNT==0: a NODATA answer */
	if(FLAGS_GET_RCODE(d->flags) != 0 || d->an_numrrsets == 0) {
		d->ttl = inf->expired;
		inf->num_msgs++;
	}
}

/** callback to delete negative key entries */
static void
negative_del_kcache(struct lruhash_entry* e, void* arg)
{
	/* entry is locked */
	struct del_info* inf = (struct del_info*)arg;
	struct key_entry_data* d = (struct key_entry_data*)e->data;
	/* could be bad because of lookup failure on the DS, DNSKEY, which
	 * was nxdomain or servfail, and thus a result of negative lookups */
	if(d->isbad) {
		d->ttl = inf->expired;
		inf->num_keys++;
	}
}

/** remove all negative(NODATA,NXDOMAIN), and servfail messages from cache */
static void
do_flush_negative(RES* ssl,
                  struct worker* worker,
                  struct view* v,
                  char* argp)
{
	struct del_info inf;

	if (!view_has_caches(ssl, v)) {
		return;
	}

	/* what we do is to set them all expired */
	inf.expired = *worker->env.now;
	inf.expired -= 3; /* handle 3 seconds skew between threads */
	inf.num_rrsets = 0;
	inf.num_msgs = 0;
	inf.num_keys = 0;

	slabhash_traverse(&v->rrset_cache->table, 1, &negative_del_rrset, &inf);
	slabhash_traverse(v->msg_cache, 1, &negative_del_msg, &inf);

	/* and validator cache */
	if(view_is_server(v) && worker->env.key_cache) {
		slabhash_traverse(worker->env.key_cache->slab, 1, 
			&negative_del_kcache, &inf);
	}

	(void)ssl_printf(ssl, "ok removed %lu rrsets, %lu messages "
		"and %lu key entries\n", (unsigned long)inf.num_rrsets, 
		(unsigned long)inf.num_msgs, (unsigned long)inf.num_keys);
}

/** remove name rrset from cache */
static void
do_flush_name(RES* ssl,
              struct worker* worker,
              struct view* v,
              char* argp)
{
	uint8_t* nm;
	int nmlabs;
	size_t nmlen;
	if(!parse_arg_name(ssl, argp, &nm, &nmlen, &nmlabs))
		return;
	do_cache_remove(v, nm, nmlen, LDNS_RR_TYPE_A, LDNS_RR_CLASS_IN);
	do_cache_remove(v, nm, nmlen, LDNS_RR_TYPE_AAAA, LDNS_RR_CLASS_IN);
	do_cache_remove(v, nm, nmlen, LDNS_RR_TYPE_NS, LDNS_RR_CLASS_IN);
	do_cache_remove(v, nm, nmlen, LDNS_RR_TYPE_SOA, LDNS_RR_CLASS_IN);
	do_cache_remove(v, nm, nmlen, LDNS_RR_TYPE_CNAME, LDNS_RR_CLASS_IN);
	do_cache_remove(v, nm, nmlen, LDNS_RR_TYPE_DNAME, LDNS_RR_CLASS_IN);
	do_cache_remove(v, nm, nmlen, LDNS_RR_TYPE_MX, LDNS_RR_CLASS_IN);
	do_cache_remove(v, nm, nmlen, LDNS_RR_TYPE_PTR, LDNS_RR_CLASS_IN);
	do_cache_remove(v, nm, nmlen, LDNS_RR_TYPE_SRV, LDNS_RR_CLASS_IN);
	do_cache_remove(v, nm, nmlen, LDNS_RR_TYPE_NAPTR, LDNS_RR_CLASS_IN);
	free(nm);
	send_ok(ssl);
}

/** printout a delegation point info */
static int
ssl_print_name_dp(RES* ssl, const char* str, uint8_t* nm, uint16_t dclass,
	struct delegpt* dp)
{
	char buf[257];
	struct delegpt_ns* ns;
	struct delegpt_addr* a;
	int f = 0;
	if(str) { /* print header for forward, stub */
		char* c = sldns_wire2str_class(dclass);
		dname_str(nm, buf);
		if(!ssl_printf(ssl, "%s %s %s ", buf, (c?c:"CLASS??"), str)) {
			free(c);
			return 0;
		}
		free(c);
	}
	for(ns = dp->nslist; ns; ns = ns->next) {
		dname_str(ns->name, buf);
		if(!ssl_printf(ssl, "%s%s", (f?" ":""), buf))
			return 0;
		f = 1;
	}
	for(a = dp->target_list; a; a = a->next_target) {
		addr_to_str(&a->addr, a->addrlen, buf, sizeof(buf));
		if(!ssl_printf(ssl, "%s%s", (f?" ":""), buf))
			return 0;
		f = 1;
	}
	return ssl_printf(ssl, "\n");
}


/** print root forwards */
static int
print_root_fwds(RES* ssl, struct iter_forwards* fwds, uint8_t* root)
{
	struct delegpt* dp;
	dp = forwards_lookup(fwds, root, LDNS_RR_CLASS_IN);
	if(!dp)
		return ssl_printf(ssl, "off (using root hints)\n");
	/* if dp is returned it must be the root */
	log_assert(query_dname_compare(dp->name, root)==0);
	return ssl_print_name_dp(ssl, NULL, root, LDNS_RR_CLASS_IN, dp);
}

/** parse args into delegpt */
static struct delegpt*
parse_delegpt(RES* ssl, char* args, uint8_t* nm, int allow_names)
{
	/* parse args and add in */
	char* p = args;
	char* todo;
	struct delegpt* dp = delegpt_create_mlc(nm);
	struct sockaddr_storage addr;
	socklen_t addrlen;
	char* auth_name;
	if(!dp) {
		(void)ssl_printf(ssl, "error out of memory\n");
		return NULL;
	}
	while(p) {
		todo = p;
		p = strchr(p, ' '); /* find next spot, if any */
		if(p) {
			*p++ = 0;	/* end this spot */
			p = skipwhite(p); /* position at next spot */
		}
		/* parse address */
		if(!authextstrtoaddr(todo, &addr, &addrlen, &auth_name)) {
			if(allow_names) {
				uint8_t* n = NULL;
				size_t ln;
				int lb;
				if(!parse_arg_name(ssl, todo, &n, &ln, &lb)) {
					(void)ssl_printf(ssl, "error cannot "
						"parse IP address or name "
						"'%s'\n", todo);
					delegpt_free_mlc(dp);
					return NULL;
				}
				if(!delegpt_add_ns_mlc(dp, n, 0)) {
					(void)ssl_printf(ssl, "error out of memory\n");
					free(n);
					delegpt_free_mlc(dp);
					return NULL;
				}
				free(n);

			} else {
				(void)ssl_printf(ssl, "error cannot parse"
					" IP address '%s'\n", todo);
				delegpt_free_mlc(dp);
				return NULL;
			}
		} else {
#if ! defined(HAVE_SSL_SET1_HOST) && ! defined(HAVE_X509_VERIFY_PARAM_SET1_HOST)
			if(auth_name)
			  log_err("no name verification functionality in "
				"ssl library, ignored name for %s", todo);
#endif
			/* add address */
			if(!delegpt_add_addr_mlc(dp, &addr, addrlen, 0, 0,
				auth_name)) {
				(void)ssl_printf(ssl, "error out of memory\n");
				delegpt_free_mlc(dp);
				return NULL;
			}
		}
	}
	dp->has_parent_side_NS = 1;
	return dp;
}

/** do the insecure_add command */
static void
do_insecure_add(RES* ssl,
                struct worker* worker,
                struct view* v,
                char* argp)
{
	if (!view_is_valid(ssl, v)) {
		return;
	}

	size_t nmlen;
	int nmlabs;
	uint8_t* nm = NULL;
	if(!parse_arg_name(ssl, argp, &nm, &nmlen, &nmlabs))
		return;
	if(worker->env.anchors) {
		if(!anchors_add_insecure(worker->env.anchors,
			LDNS_RR_CLASS_IN, nm)) {
			(void)ssl_printf(ssl, "error out of memory\n");
			free(nm);
			return;
		}
	}
	free(nm);
	send_ok(ssl);
}

/** do the insecure_remove command */
static void
do_insecure_remove(RES* ssl,
                   struct worker* worker,
                   struct view* v,
                   char* argp)
{
	if (!view_is_valid(ssl, v)) {
		return;
	}

	size_t nmlen;
	int nmlabs;
	uint8_t* nm = NULL;
	if(!parse_arg_name(ssl, argp, &nm, &nmlen, &nmlabs))
		return;
	if(worker->env.anchors)
		anchors_delete_insecure(worker->env.anchors,
			LDNS_RR_CLASS_IN, nm);
	free(nm);
	send_ok(ssl);
}

static void
do_insecure_list(RES* ssl,
                 struct worker* worker,
                 struct view* v,
                 char* argp)
{
	if (!view_is_valid(ssl, v)) {
		return;
	}

	char buf[257];
	struct trust_anchor* a;
	if(worker->env.anchors) {
		RBTREE_FOR(a, struct trust_anchor*, worker->env.anchors->tree) {
			if(a->numDS == 0 && a->numDNSKEY == 0) {
				dname_str(a->name, buf);
				ssl_printf(ssl, "%s\n", buf);
			}
		}
	}
}

/** do the status command */
static void
do_status(RES* ssl,
          struct worker* worker,
          struct view* v,
          char* argp)
{
	if (!view_is_valid(ssl, v)) {
		return;
	}

	int i;
	time_t uptime;
	if(!ssl_printf(ssl, "version: %s\n", PACKAGE_VERSION))
		return;
	if(!ssl_printf(ssl, "verbosity: %d\n", verbosity))
		return;
	if(!ssl_printf(ssl, "threads: %d\n", worker->daemon->num))
		return;
	if(!ssl_printf(ssl, "modules: %d [", worker->daemon->mods.num))
		return;
	for(i=0; i<worker->daemon->mods.num; i++) {
		if(!ssl_printf(ssl, " %s", worker->daemon->mods.mod[i]->name))
			return;
	}
	if(!ssl_printf(ssl, " ]\n"))
		return;
	uptime = (time_t)time(NULL) - (time_t)worker->daemon->time_boot.tv_sec;
	if(!ssl_printf(ssl, "uptime: " ARG_LL "d seconds\n", (long long)uptime))
		return;
	if(!ssl_printf(ssl, "options:%s%s%s%s\n" , 
		(worker->daemon->reuseport?" reuseport":""),
		(worker->daemon->rc->accept_list?" control":""),
		(worker->daemon->rc->accept_list && worker->daemon->rc->use_cert?"(ssl)":""),
		(worker->daemon->rc->accept_list && worker->daemon->cfg->control_ifs.first && worker->daemon->cfg->control_ifs.first->str && worker->daemon->cfg->control_ifs.first->str[0] == '/'?"(namedpipe)":"")
		))
		return;
	if(!ssl_printf(ssl, "unbound (pid %d) is running...\n",
		(int)getpid()))
		return;
}

/** get age for the mesh state */
static void
get_mesh_age(struct mesh_state* m, char* buf, size_t len, 
	struct module_env* env)
{
	if(m->reply_list) {
		struct timeval d;
		struct mesh_reply* r = m->reply_list;
		/* last reply is the oldest */
		while(r && r->next)
			r = r->next;
		timeval_subtract(&d, env->now_tv, &r->start_time);
		snprintf(buf, len, ARG_LL "d.%6.6d",
			(long long)d.tv_sec, (int)d.tv_usec);
	} else {
		snprintf(buf, len, "-");
	}
}

/** get status of a mesh state */
static void
get_mesh_status(struct mesh_area* mesh, struct mesh_state* m, 
	char* buf, size_t len)
{
	enum module_ext_state s = m->s.ext_state[m->s.curmod];
	const char *modname = mesh->mods.mod[m->s.curmod]->name;
	size_t l;
	if(strcmp(modname, "iterator") == 0 && s == module_wait_reply &&
		m->s.minfo[m->s.curmod]) {
		/* break into iterator to find out who its waiting for */
		struct iter_qstate* qstate = (struct iter_qstate*)
			m->s.minfo[m->s.curmod];
		struct outbound_list* ol = &qstate->outlist;
		struct outbound_entry* e;
		snprintf(buf, len, "%s wait for", modname);
		l = strlen(buf);
		buf += l; len -= l;
		if(ol->first == NULL)
			snprintf(buf, len, " (empty_list)");
		for(e = ol->first; e; e = e->next) {
			snprintf(buf, len, " ");
			l = strlen(buf);
			buf += l; len -= l;
			addr_to_str(&e->qsent->addr, e->qsent->addrlen, 
				buf, len);
			l = strlen(buf);
			buf += l; len -= l;
		}
	} else if(s == module_wait_subquery) {
		/* look in subs from mesh state to see what */
		char nm[257];
		struct mesh_state_ref* sub;
		snprintf(buf, len, "%s wants", modname);
		l = strlen(buf);
		buf += l; len -= l;
		if(m->sub_set.count == 0)
			snprintf(buf, len, " (empty_list)");
		RBTREE_FOR(sub, struct mesh_state_ref*, &m->sub_set) {
			char* t = sldns_wire2str_type(sub->s->s.qinfo.qtype);
			char* c = sldns_wire2str_class(sub->s->s.qinfo.qclass);
			dname_str(sub->s->s.qinfo.qname, nm);
			snprintf(buf, len, " %s %s %s", (t?t:"TYPE??"),
				(c?c:"CLASS??"), nm);
			l = strlen(buf);
			buf += l; len -= l;
			free(t);
			free(c);
		}
	} else {
		snprintf(buf, len, "%s is %s", modname, strextstate(s));
	}
}

/** do the dump_requestlist command */
static void
do_dump_requestlist(RES* ssl,
                    struct worker* worker,
                    struct view* v,
                    char* argp)
{
	if (!view_is_valid(ssl, v)) {
		return;
	}

	struct mesh_area* mesh;
	struct mesh_state* m;
	int num = 0;
	char buf[257];
	char timebuf[32];
	char statbuf[10240];
	if(!ssl_printf(ssl, "thread #%d\n", worker->thread_num))
		return;
	if(!ssl_printf(ssl, "#   type cl name    seconds    module status\n"))
		return;
	/* show worker mesh contents */
	mesh = worker->env.mesh;
	if(!mesh) return;
	RBTREE_FOR(m, struct mesh_state*, &mesh->all) {
		char* t = sldns_wire2str_type(m->s.qinfo.qtype);
		char* c = sldns_wire2str_class(m->s.qinfo.qclass);
		dname_str(m->s.qinfo.qname, buf);
		get_mesh_age(m, timebuf, sizeof(timebuf), &worker->env);
		get_mesh_status(mesh, m, statbuf, sizeof(statbuf));
		if(!ssl_printf(ssl, "%3d %4s %2s %s %s %s\n", 
			num, (t?t:"TYPE??"), (c?c:"CLASS??"), buf, timebuf,
			statbuf)) {
			free(t);
			free(c);
			return;
		}
		num++;
		free(t);
		free(c);
	}
}

/** structure for argument data for dump infra host */
struct infra_arg {
	/** the infra cache */
	struct infra_cache* infra;
	/** the SSL connection */
	RES* ssl;
	/** the time now */
	time_t now;
	/** ssl failure? stop writing and skip the rest.  If the tcp
	 * connection is broken, and writes fail, we then stop writing. */
	int ssl_failed;
};

/** callback for every host element in the infra cache */
static void
dump_infra_host(struct lruhash_entry* e, void* arg)
{
	struct infra_arg* a = (struct infra_arg*)arg;
	struct infra_key* k = (struct infra_key*)e->key;
	struct infra_data* d = (struct infra_data*)e->data;
	char ip_str[1024];
	char name[257];
	int port;
	if(a->ssl_failed)
		return;
	addr_to_str(&k->addr, k->addrlen, ip_str, sizeof(ip_str));
	dname_str(k->zonename, name);
	port = (int)ntohs(((struct sockaddr_in*)&k->addr)->sin_port);
	if(port != UNBOUND_DNS_PORT) {
		snprintf(ip_str+strlen(ip_str), sizeof(ip_str)-strlen(ip_str),
			"@%d", port);
	}
	/* skip expired stuff (only backed off) */
	if(d->ttl < a->now) {
		if(d->rtt.rto >= USEFUL_SERVER_TOP_TIMEOUT) {
			if(!ssl_printf(a->ssl, "%s %s expired rto %d\n", ip_str,
				name, d->rtt.rto))  {
				a->ssl_failed = 1;
				return;
			}
		}
		return;
	}
	if(!ssl_printf(a->ssl, "%s %s ttl %lu ping %d var %d rtt %d rto %d "
		"tA %d tAAAA %d tother %d "
		"ednsknown %d edns %d delay %d lame dnssec %d rec %d A %d "
		"other %d\n", ip_str, name, (unsigned long)(d->ttl - a->now),
		d->rtt.srtt, d->rtt.rttvar, rtt_notimeout(&d->rtt), d->rtt.rto,
		d->timeout_A, d->timeout_AAAA, d->timeout_other,
		(int)d->edns_lame_known, (int)d->edns_version,
		(int)(a->now<d->probedelay?(d->probedelay - a->now):0),
		(int)d->isdnsseclame, (int)d->rec_lame, (int)d->lame_type_A,
		(int)d->lame_other)) {
		a->ssl_failed = 1;
		return;
	}
}

/** do the dump_infra command */
static void
do_dump_infra(RES* ssl,
              struct worker* worker,
              struct view* v,
              char* argp)
{
	if (!view_is_valid(ssl, v)) {
		return;
	}

	struct infra_arg arg;

	arg.infra = worker->env.infra_cache;
	arg.ssl = ssl;
	arg.now = *worker->env.now;
	arg.ssl_failed = 0;
	slabhash_traverse(arg.infra->hosts, 0, &dump_infra_host, (void*)&arg);
}

/** do the log_reopen command */
static void
do_log_reopen(RES* ssl,
              struct worker* worker,
              struct view* v,
              char* argp)
{
	if (view_is_valid(ssl, v)) {
		send_ok(ssl);

		struct config_file* cfg = worker->env.cfg;

		log_init(cfg->logfile, cfg->use_syslog, cfg->chrootdir);
	}
}

/** do the auth_zone_reload command */
static void
do_auth_zone_reload(RES* ssl,
                    struct worker* worker,
                    struct view* v,
                    char* argp)
{
	if (!view_is_valid(ssl, v)) {
		return;
	}

	size_t nmlen;
	int nmlabs;
	uint8_t* nm = NULL;
	struct auth_zones* az = worker->env.auth_zones;
	struct auth_zone* z = NULL;
	if(!parse_arg_name(ssl, argp, &nm, &nmlen, &nmlabs))
		return;
	if(az) {
		lock_rw_rdlock(&az->lock);
		z = auth_zone_find(az, nm, nmlen, LDNS_RR_CLASS_IN);
		if(z) {
			lock_rw_wrlock(&z->lock);
		}
		lock_rw_unlock(&az->lock);
	}
	free(nm);
	if(!z) {
		(void)ssl_printf(ssl, "error no auth-zone %s\n", argp);
		return;
	}
	if(!auth_zone_read_zonefile(z, worker->env.cfg)) {
		lock_rw_unlock(&z->lock);
		(void)ssl_printf(ssl, "error failed to read %s\n", argp);
		return;
	}
	lock_rw_unlock(&z->lock);
	send_ok(ssl);
}

/** do the auth_zone_transfer command */
static void
do_auth_zone_transfer(RES* ssl,
                      struct worker* worker,
                      struct view* v,
                      char* argp)
{
	if (!view_is_valid(ssl, v)) {
		return;
	}

	size_t nmlen;
	int nmlabs;
	uint8_t* nm = NULL;
	struct auth_zones* az = worker->env.auth_zones;
	if(!parse_arg_name(ssl, argp, &nm, &nmlen, &nmlabs))
		return;
	if(!az || !auth_zones_startprobesequence(az, &worker->env, nm, nmlen,
		LDNS_RR_CLASS_IN)) {
		(void)ssl_printf(ssl, "error zone xfr task not found %s\n", argp);
		free(nm);
		return;
	}
	free(nm);
	send_ok(ssl);
}
	
/** do the set_option command */
static void
do_set_option(RES* ssl,
              struct worker* worker,
              struct view* v,
              char* argp)
{
	if (!view_is_valid(ssl, v)) {
		return;
	}

	char* arg2;
	if(!find_arg2(ssl, argp, &arg2))
		return;
	if(!config_set_option(worker->env.cfg, argp, arg2)) {
		(void)ssl_printf(ssl, "error setting option\n");
		return;
	}
	/* effectuate some arguments */
	if(strcmp(argp, "val-override-date:") == 0) {
		int m = modstack_find(&worker->env.mesh->mods, "validator");
		struct val_env* val_env = NULL;
		if(m != -1) val_env = (struct val_env*)worker->env.modinfo[m];
		if(val_env)
			val_env->date_override = worker->env.cfg->val_date_override;
	}
	send_ok(ssl);
}

/* routine to printout option values over SSL */
void remote_get_opt_ssl(char* line, void* arg)
{
	RES* ssl = (RES*)arg;
	(void)ssl_printf(ssl, "%s\n", line);
}

/** do the get_option command */
static void
do_get_option(RES* ssl,
              struct worker* worker,
              struct view* v,
              char* argp)
{
	if (view_is_valid(ssl, v)) {
		int r = config_get_option(worker->env.cfg,
		                          argp,
		                          remote_get_opt_ssl,
		                          ssl);

		if (r == 0) {
			(void)ssl_printf(ssl, "error unknown option\n");
			return;
		}
	}
}

/** do the list_forwards command */
static void
do_list_forwards(RES* ssl,
                 struct worker* worker,
                 struct view* v,
                 char* argp)
{
	if (!view_has_forwards(ssl, v)) {
		return;
	}

	struct val_anchors *val = view_is_server(v) ? worker->env.anchors : NULL;
	struct iter_forwards* fwds = v->fwds;
	struct iter_forward_zone* z;
	struct trust_anchor* a;
	int insecure;

	lock_rw_rdlock(&v->lock);

	RBTREE_FOR(z, struct iter_forward_zone*, fwds->tree) {
		if(!z->dp) continue; /* skip empty marker for stub */

		/* see if it is insecure */
		insecure = 0;

		if (val != NULL &&
			(a = anchor_find(val, z->name,
		                          z->namelabs, z->namelen,
		                          z->dclass))) {
			if (!a->keylist && !a->numDS && !a->numDNSKEY) {
				insecure = 1;
			}

			lock_basic_unlock(&a->lock);
		}
		if(!ssl_print_name_dp(ssl, (insecure?"forward +i":"forward"),
			z->name, z->dclass, z->dp))
			break;
	}

	lock_rw_rdlock(&v->lock);
}

/** do the list_stubs command */
static void
do_list_stubs(RES* ssl,
              struct worker* worker,
              struct view* v,
              char* argp)
{
	if (!view_has_hints(ssl, v)) {
		return;
	}

	struct val_anchors *val = view_is_server(v) ? worker->env.anchors : NULL;
	struct iter_hints_stub* z;
	struct trust_anchor* a;
	int insecure;
	char str[32];

	lock_rw_rdlock(&v->lock);

	RBTREE_FOR(z, struct iter_hints_stub*, &v->hints->tree) {
		/* see if it is insecure */
		insecure = 0;

		if (val != NULL &&
			(a = anchor_find(val, z->node.name,
		                          z->node.labs, z->node.len,
		                          z->node.dclass))) {
			if (!a->keylist && !a->numDS && !a->numDNSKEY) {
				insecure = 1;
			}

			lock_basic_unlock(&a->lock);
		}

		snprintf(str, sizeof(str), "stub %sprime%s",
			(z->noprime?"no":""), (insecure?" +i":""));
		if(!ssl_print_name_dp(ssl, str, z->node.name,
			z->node.dclass, z->dp))
			break;
	}

	lock_rw_unlock(&v->lock);
}

/** do the list_auth_zones command */
static void
do_list_auth_zones(RES* ssl,
                   struct worker* worker,
                   struct view* v,
                   char* argp)
{
	if (!view_is_valid(ssl, v)) {
		return;
	}

	struct auth_zones* az = worker->env.auth_zones;
	struct auth_zone* z;
	char buf[257], buf2[256];
	lock_rw_rdlock(&az->lock);
	RBTREE_FOR(z, struct auth_zone*, &az->ztree) {
		lock_rw_rdlock(&z->lock);
		dname_str(z->name, buf);
		if(z->zone_expired)
			snprintf(buf2, sizeof(buf2), "expired");
		else {
			uint32_t serial = 0;
			if(auth_zone_get_serial(z, &serial))
				snprintf(buf2, sizeof(buf2), "serial %u",
					(unsigned)serial);
			else	snprintf(buf2, sizeof(buf2), "no serial");
		}
		if(!ssl_printf(ssl, "%s\t%s\n", buf, buf2)) {
			/* failure to print */
			lock_rw_unlock(&z->lock);
			lock_rw_unlock(&az->lock);
			return;
		}
		lock_rw_unlock(&z->lock);
	}
	lock_rw_unlock(&az->lock);
}

static void
list_local_zones(RES* ssl, struct local_zones* zones)
{
	struct local_zone* z;
	char buf[257];
	lock_rw_rdlock(&zones->lock);
	RBTREE_FOR(z, struct local_zone*, &zones->ztree) {
		lock_rw_rdlock(&z->lock);
		dname_str(z->name, buf);
		if(!ssl_printf(ssl, "%s %s\n", buf, 
			local_zone_type2str(z->type))) {
			/* failure to print */
			lock_rw_unlock(&z->lock);
			lock_rw_unlock(&zones->lock);
			return;
		}
		lock_rw_unlock(&z->lock);
	}
	lock_rw_unlock(&zones->lock);
}

/** do the list_local_zones command */
static void
do_list_local_zones(RES* ssl,
                    struct worker* worker,
                    struct view* v,
                    char* argp)
{
	lock_rw_rdlock(&v->lock);

	if (v->local_zones != NULL) {
		list_local_zones(ssl, v->local_zones);
	}

	lock_rw_unlock(&v->lock);
}

/** do the view_list_local_zones command */
static void
do_view_list_local_zones(RES* ssl,
                         struct worker* worker,
                         struct view* v,
                         char* argp)
{
	if ((v = check_view_arg(ssl, worker, v, &argp)) != NULL) {
		do_list_local_zones(ssl, worker, v, argp);
	}
}

static void
list_local_data(RES* ssl, struct worker* worker, struct local_zones* zones)
{
	struct local_zone* z;
	struct local_data* d;
	struct local_rrset* p;
	char* s = (char*)sldns_buffer_begin(worker->env.scratch_buffer);
	size_t slen = sldns_buffer_capacity(worker->env.scratch_buffer);
	lock_rw_rdlock(&zones->lock);
	RBTREE_FOR(z, struct local_zone*, &zones->ztree) {
		lock_rw_rdlock(&z->lock);
		RBTREE_FOR(d, struct local_data*, &z->data) {
			for(p = d->rrsets; p; p = p->next) {
				struct packed_rrset_data* d =
					(struct packed_rrset_data*)p->rrset->entry.data;
				size_t i;
				for(i=0; i<d->count + d->rrsig_count; i++) {
					if(!packed_rr_to_string(p->rrset, i,
						0, s, slen)) {
						if(!ssl_printf(ssl, "BADRR\n")) {
							lock_rw_unlock(&z->lock);
							lock_rw_unlock(&zones->lock);
							return;
						}
					}
				        if(!ssl_printf(ssl, "%s\n", s)) {
						lock_rw_unlock(&z->lock);
						lock_rw_unlock(&zones->lock);
						return;
					}
				}
			}
		}
		lock_rw_unlock(&z->lock);
	}
	lock_rw_unlock(&zones->lock);
}

/** do the list_local_data command */
static void
do_list_local_data(RES* ssl,
                   struct worker* worker,
                   struct view* v,
                   char* argp)
{
	lock_rw_rdlock(&v->lock);

	if (v->local_zones != NULL) {
		list_local_data(ssl, worker, v->local_zones);
	}

	lock_rw_unlock(&v->lock);
}
	
/** do the view_list_local_data command */
static void
do_view_list_local_data(RES* ssl,
                        struct worker* worker,
                        struct view* v,
                        char* argp)
{
	if ((v = check_view_arg(ssl, worker, v, &argp)) != NULL) {
		do_list_local_data(ssl, worker, v, argp);
	}
}

static void
do_list_views(RES* ssl,
              struct worker* worker,
              struct view* v,
              char* argp)
{
	if (view_is_valid(ssl, v)) {
		struct view_node *vn;

		RBTREE_FOR(vn, struct view_node *, &worker->daemon->views->vtree) {
			ssl_printf(ssl, "%s\n", vn->vinfo.name);
		}
	}
}

/** struct for user arg ratelimit list */
struct ratelimit_list_arg {
	/** the infra cache */
	struct infra_cache* infra;
	/** the SSL to print to */
	RES* ssl;
	/** all or only ratelimited */
	int all;
	/** current time */
	time_t now;
};

#define ip_ratelimit_list_arg ratelimit_list_arg

/** list items in the ratelimit table */
static void
rate_list(struct lruhash_entry* e, void* arg)
{
	struct ratelimit_list_arg* a = (struct ratelimit_list_arg*)arg;
	struct rate_key* k = (struct rate_key*)e->key;
	struct rate_data* d = (struct rate_data*)e->data;
	char buf[257];
	int lim = infra_find_ratelimit(a->infra, k->name, k->namelen);
	int max = infra_rate_max(d, a->now);
	if(a->all == 0) {
		if(max < lim)
			return;
	}
	dname_str(k->name, buf);
	ssl_printf(a->ssl, "%s %d limit %d\n", buf, max, lim);
}

/** list items in the ip_ratelimit table */
static void
ip_rate_list(struct lruhash_entry* e, void* arg)
{
	char ip[128];
	struct ip_ratelimit_list_arg* a = (struct ip_ratelimit_list_arg*)arg;
	struct ip_rate_key* k = (struct ip_rate_key*)e->key;
	struct ip_rate_data* d = (struct ip_rate_data*)e->data;
	int lim = infra_ip_ratelimit;
	int max = infra_rate_max(d, a->now);
	if(a->all == 0) {
		if(max < lim)
			return;
	}
	addr_to_str(&k->addr, k->addrlen, ip, sizeof(ip));
	ssl_printf(a->ssl, "%s %d limit %d\n", ip, max, lim);
}

/** do the ratelimit_list command */
static void
do_ratelimit_list(RES* ssl,
                  struct worker* worker,
                  struct view* v,
                  char* argp)
{
	if (!view_is_valid(ssl, v)) {
		return;
	}

	struct ratelimit_list_arg a;
	a.all = 0;
	a.infra = worker->env.infra_cache;
	a.now = *worker->env.now;
	a.ssl = ssl;
	argp = skipwhite(argp);
	if(strcmp(argp, "+a") == 0)
		a.all = 1;
	if(a.infra->domain_rates==NULL ||
		(a.all == 0 && infra_dp_ratelimit == 0))
		return;
	slabhash_traverse(a.infra->domain_rates, 0, rate_list, &a);
}

/** do the ip_ratelimit_list command */
static void
do_ip_ratelimit_list(RES* ssl,
                     struct worker* worker,
                     struct view* v,
                     char* argp)
{
	if (!view_is_valid(ssl, v)) {
		return;
	}

	struct ip_ratelimit_list_arg a;
	a.all = 0;
	a.infra = worker->env.infra_cache;
	a.now = *worker->env.now;
	a.ssl = ssl;
	argp = skipwhite(argp);
	if(strcmp(argp, "+a") == 0)
		a.all = 1;
	if(a.infra->client_ip_rates==NULL ||
		(a.all == 0 && infra_ip_ratelimit == 0))
		return;
	slabhash_traverse(a.infra->client_ip_rates, 0, ip_rate_list, &a);
}

/** do the rpz_enable/disable command */
static void
rpz_control(RES* ssl,
            struct worker* worker,
            struct view* v,
            char* arg,
            int enable)
{
	if (!view_is_valid(ssl, v)) {
		return;
	}

    size_t nmlen;
    int nmlabs;
    uint8_t *nm = NULL;
    struct auth_zones *az = worker->env.auth_zones;
    struct auth_zone *z = NULL;
    if (!parse_arg_name(ssl, arg, &nm, &nmlen, &nmlabs))
        return;
    if (az) {
        lock_rw_rdlock(&az->lock);
        z = auth_zone_find(az, nm, nmlen, LDNS_RR_CLASS_IN);
        if (z) {
            lock_rw_wrlock(&z->lock);
        }
        lock_rw_unlock(&az->lock);
    }
    free(nm);
    if (!z) {
        (void) ssl_printf(ssl, "error no auth-zone %s\n", arg);
        return;
    }
    if (!z->rpz) {
        (void) ssl_printf(ssl, "error auth-zone %s not RPZ\n", arg);
        lock_rw_unlock(&z->lock);
        return;
    }
    if (enable) {
        rpz_enable(z->rpz);
    } else {
        rpz_disable(z->rpz);
    }
    lock_rw_unlock(&z->lock);
    send_ok(ssl);
}

/** do the rpz_enable command */
static void
do_rpz_enable(RES* ssl,
              struct worker* worker,
              struct view* v,
              char* argp)
{
    rpz_control(ssl, worker, v, argp, 1);
}

/** do the rpz_disable command */
static void
do_rpz_disable(RES* ssl,
               struct worker* worker,
               struct view* v,
               char* argp)
{
    rpz_control(ssl, worker, v, argp, 0);
}

/* basic connectivity test */
static void
do_echo(RES* ssl,
        struct worker* worker,
        struct view* v,
        char* argp)
{
	ssl_printf(ssl, "echo");

	if (!view_is_server(v)) {
		ssl_printf(ssl, ".%s", v->name);
	}

	while (*argp != '\0') {
		char *next;

		find_arg2(ssl, argp, &next);
		ssl_printf(ssl, " %s", argp);
		argp = next;
	}

	ssl_printf(ssl, "\n");
}
/** tell other processes to execute the command */
static void
distribute_cmd(struct daemon_remote* rc, RES* ssl, char* cmd)
{
	int i;
	if(!cmd || !ssl) 
		return;
	/* skip i=0 which is me */
	for(i=1; i<rc->worker->daemon->num; i++) {
		worker_send_cmd(rc->worker->daemon->workers[i],
			worker_cmd_remote);
		if(!tube_write_msg(rc->worker->daemon->workers[i]->cmd,
			(uint8_t*)cmd, strlen(cmd)+1, 0)) {
			ssl_printf(ssl, "error could not distribute cmd\n");
			return;
		}
	}
}

static void
do_dump_cache(RES* ssl,
              struct worker* worker,
              struct view* v,
              char* argp)
{
	if (view_has_caches(ssl, v)) {
		dump_cache(ssl, worker, v);
	}
}

static void
do_load_cache(RES* ssl,
              struct worker* worker,
              struct view* v,
              char* argp)
{
	if (view_is_valid(ssl, v)) {
		if (load_cache(ssl, worker)) {
			send_ok(ssl);
		}
	}
}

#include "remote-lookup.c"

/** execute a remote control command */
static void
execute_cmd(struct daemon_remote* rc,
            RES* ssl,
            char* cmd,
            struct worker* worker)
{
	char* p = skipwhite(cmd);
	cmdHashType h = HASH_INIT;
	char *cp = p, c;

	// Skip to the next whitespace, '.'. or end of the string to compute
	// both the length and the hash.

	while ((c = *cp) != '\0' && c != ' ' && c != '\t' && c != '.') {
		h = _hashFunc(h, c);
		cp++;
	}

	size_t len;

	if ((len = cp - p) > 0) {
		struct view *v;

		// Look to see if a view is specified

		if (c != '.') {
			v = &worker->daemon->views->server_view;
		} else {
			char *vn = ++cp;

			while ((c = *cp) != '\0' && c != ' ' && c != '\t') {
				cp++;
			}
	
			if (c != '\0') {
				*cp = '\0';
			}
			if ((v = views_find_view(worker->daemon->views, vn, 0)) == NULL) {
				ssl_printf(ssl, "no view with name: %s\n", vn);
				return;
			}

			// Restore the string and unlock the view

			lock_rw_unlock(&v->lock);

			if (c != '\0') {
				*cp = c;
			}
		}

		uint32_t hval = _hashFold(h);

		cmdHash const *hp = cmdTable[HASH_TABLE_NDX(hval)];

		while (hp != NULL) {
			if (hp->hash == hval &&
			    hp->len == len &&
			        strncmp(p, hp->cmd, len) == 0) {
				// If this command needs to be distributed, do so now
				// before the service function can change the command line

				if (rc != NULL && hp->dist) {
					distribute_cmd(rc, ssl, cmd);
				}

				(*hp->fnc)(ssl, worker, v, skipwhite(cp));
				return;
			}

			hp = hp->next;
		}
	}

	(void) ssl_printf(ssl, "error unknown command '%s'\n", p);
}

void 
daemon_remote_exec(struct worker* worker)
{
	/* read the cmd string */
	uint8_t* msg = NULL;
	uint32_t len = 0;
	if(!tube_read_msg(worker->cmd, &msg, &len, 0)) {
		log_err("daemon_remote_exec: tube_read_msg failed");
		return;
	}
	verbose(VERB_ALGO, "remote exec distributed: %s", (char*)msg);
	execute_cmd(NULL, NULL, (char*)msg, worker);
	free(msg);
}

#define	MAKE_STR(v)   #v
#define	VAR_TO_STR(v) MAKE_STR(v)

// All commands must begin with this character sequence

static char const UBCT[] = "UBCT" VAR_TO_STR(UNBOUND_CONTROL_VERSION) " "; 

#define	MAJIK_LEN	(sizeof(UBCT) - 1)

/** handle remote control request */
static void
handle_req(struct daemon_remote* rc, struct rc_state* s, RES* res)
{
	char magic[sizeof(UBCT)];
	char buf[1024];
	int r;
#ifdef USE_WINSOCK
	/* makes it possible to set the socket blocking again. */
	/* basically removes it from winsock_event ... */
	WSAEventSelect(s->c->fd, NULL, 0);
#endif
	fd_set_block(s->c->fd);

	/* try to read magic UBCT[version]_space_ string */
	if(res->ssl) {
		ERR_clear_error();
		if((r = SSL_read(res->ssl, magic, MAJIK_LEN)) <= 0) {
			if(SSL_get_error(res->ssl, r) != SSL_ERROR_ZERO_RETURN) {
				log_crypto_err("could not SSL_read");
			}

			return;
		}
	} else {
		ssize_t rr;

		do {
			if ((rr = recv(res->fd, magic, MAJIK_LEN, 0)) > 0) {
				break;
			}
			if (rr == 0) {
				return;
			}
			if (errno != EINTR && errno != EAGAIN) {
				log_err("could not recv: %s", sock_strerror(errno));
				return;
			}
		} while (1);

		r = (int) rr;
	}

	if (r != MAJIK_LEN || strncmp(magic, UBCT, 4) != 0) {
		/* probably wrong tool connected, ignore it completely */

		verbose(VERB_QUERY, "control connection has bad magic string");
	} else if (ssl_read_line(res, buf, sizeof(buf))) {
		magic[MAJIK_LEN] = '\0';
	
		if(strcmp(magic, UBCT) == 0) {
			verbose(VERB_DETAIL, "control cmd: %s", buf);
			execute_cmd(rc, res, buf, rc->worker);
		} else {
			verbose(VERB_QUERY,
			        "control connection had bad version %s, cmd: %s",
			        magic,
			        buf);
			ssl_printf(res, "error version mismatch\n");
		}
	}
}

/** handle SSL_do_handshake changes to the file descriptor to wait for later */
static int
remote_handshake_later(struct daemon_remote* rc, struct rc_state* s,
	struct comm_point* c, int r, int r2)
{
	if(r2 == SSL_ERROR_WANT_READ) {
		if(s->shake_state == rc_hs_read) {
			/* try again later */
			return 0;
		}
		s->shake_state = rc_hs_read;
		comm_point_listen_for_rw(c, 1, 0);
		return 0;
	} else if(r2 == SSL_ERROR_WANT_WRITE) {
		if(s->shake_state == rc_hs_write) {
			/* try again later */
			return 0;
		}
		s->shake_state = rc_hs_write;
		comm_point_listen_for_rw(c, 0, 1);
		return 0;
	} else {
		if(r == 0)
			log_err("remote control connection closed prematurely");
		log_addr(VERB_OPS, "failed connection from",
			&s->c->repinfo.addr, s->c->repinfo.addrlen);
		log_crypto_err("remote control failed ssl");
		clean_point(rc, s);
	}
	return 0;
}

int remote_control_callback(struct comm_point* c, void* arg, int err, 
	struct comm_reply* ATTR_UNUSED(rep))
{
	RES res;
	struct rc_state* s = (struct rc_state*)arg;
	struct daemon_remote* rc = s->rc;
	int r;
	if(err != NETEVENT_NOERROR) {
		if(err==NETEVENT_TIMEOUT) 
			log_err("remote control timed out");
		clean_point(rc, s);
		return 0;
	}
	if(s->ssl) {
		/* (continue to) setup the SSL connection */
		ERR_clear_error();
		r = SSL_do_handshake(s->ssl);
		if(r != 1) {
			int r2 = SSL_get_error(s->ssl, r);
			return remote_handshake_later(rc, s, c, r, r2);
		}
		s->shake_state = rc_none;
	}

	/* once handshake has completed, check authentication */
	if (!rc->use_cert) {
		verbose(VERB_ALGO, "unauthenticated remote control connection");
	} else if(SSL_get_verify_result(s->ssl) == X509_V_OK) {
		X509* x = SSL_get_peer_certificate(s->ssl);
		if(!x) {
			verbose(VERB_DETAIL, "remote control connection "
				"provided no client certificate");
			clean_point(rc, s);
			return 0;
		}
		verbose(VERB_ALGO, "remote control connection authenticated");
		X509_free(x);
	} else {
		verbose(VERB_DETAIL, "remote control connection failed to "
			"authenticate with client certificate");
		clean_point(rc, s);
		return 0;
	}

	/* if OK start to actually handle the request */
	res.ssl = s->ssl;
	res.fd = c->fd;
	handle_req(rc, s, &res);

	verbose(VERB_ALGO, "remote control operation completed");
	clean_point(rc, s);
	return 0;
}
