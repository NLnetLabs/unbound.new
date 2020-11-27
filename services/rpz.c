/*
 * services/rpz.c - rpz service
 *
 * Copyright (c) 2019, NLnet Labs. All rights reserved.
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
 * This file contains functions to enable RPZ service.
 */

#include "config.h"
#include "services/rpz.h"
#include "util/config_file.h"
#include "sldns/wire2str.h"
#include "sldns/str2wire.h"
#include "util/data/dname.h"
#include "util/net_help.h"
#include "util/log.h"
#include "util/data/dname.h"
#include "util/locks.h"
#include "util/regional.h"
#include "util/data/msgencode.h"
#include "services/cache/dns.h"
#include "iterator/iterator.h"
#include "iterator/iter_delegpt.h"

typedef struct resp_addr rpz_aclnode_type;

struct matched_delegation_point {
	uint8_t* dname;
	size_t dname_len;
};

/** string for RPZ action enum */
const char*
rpz_action_to_string(enum rpz_action a)
{
	switch(a) {
	case RPZ_NXDOMAIN_ACTION: return "rpz-nxdomain";
	case RPZ_NODATA_ACTION: return "rpz-nodata";
	case RPZ_PASSTHRU_ACTION: return "rpz-passthru";
	case RPZ_DROP_ACTION: return "rpz-drop";
	case RPZ_TCP_ONLY_ACTION: return "rpz-tcp-only";
	case RPZ_INVALID_ACTION: return "rpz-invalid";
	case RPZ_LOCAL_DATA_ACTION: return "rpz-local-data";
	case RPZ_DISABLED_ACTION: return "rpz-disabled";
	case RPZ_CNAME_OVERRIDE_ACTION: return "rpz-cname-override";
	case RPZ_NO_OVERRIDE_ACTION: return "rpz-no-override";
	default: return "rpz-unkown-action";
	}
}

/** RPZ action enum for config string */
static enum rpz_action
rpz_config_to_action(char* a)
{
	if(strcmp(a, "nxdomain") == 0) return RPZ_NXDOMAIN_ACTION;
	else if(strcmp(a, "nodata") == 0) return RPZ_NODATA_ACTION;
	else if(strcmp(a, "passthru") == 0) return RPZ_PASSTHRU_ACTION;
	else if(strcmp(a, "drop") == 0) return RPZ_DROP_ACTION;
	else if(strcmp(a, "tcp_only") == 0) return RPZ_TCP_ONLY_ACTION;
	else if(strcmp(a, "cname") == 0) return RPZ_CNAME_OVERRIDE_ACTION;
	else if(strcmp(a, "disabled") == 0) return RPZ_DISABLED_ACTION;
	else return RPZ_INVALID_ACTION;
}

/** string for RPZ trigger enum */
static const char*
rpz_trigger_to_string(enum rpz_trigger r)
{
	switch(r) {
	case RPZ_QNAME_TRIGGER: return "rpz-qname";
	case RPZ_CLIENT_IP_TRIGGER: return "rpz-client-ip";
	case RPZ_RESPONSE_IP_TRIGGER: return "rpz-response-ip";
	case RPZ_NSDNAME_TRIGGER: return "rpz-nsdname";
	case RPZ_NSIP_TRIGGER: return "rpz-nsip";
	case RPZ_INVALID_TRIGGER: return "rpz-invalid";
	default: return "rpz-unknown-trigger";
	}
}

/**
 * Get the label that is just before the root label.
 * @param dname: dname to work on
 * @param maxdnamelen: maximum length of the dname
 * @return: pointer to TLD label, NULL if not found or invalid dname
 */
static uint8_t*
get_tld_label(uint8_t* dname, size_t maxdnamelen)
{
	uint8_t* prevlab = dname;
	size_t dnamelen = 0;

	/* one byte needed for label length */
	if(dnamelen+1 > maxdnamelen)
		return NULL;

	/* only root label */
	if(*dname == 0)
		return NULL;

	while(*dname) {
		dnamelen += ((size_t)*dname)+1;
		if(dnamelen+1 > maxdnamelen)
			return NULL;
		dname = dname+((size_t)*dname)+1;
		if(*dname != 0)
			prevlab = dname;
	}
	return prevlab;
}

/**
 * Classify RPZ action for RR type/rdata
 * @param rr_type: the RR type
 * @param rdatawl: RDATA with 2 bytes length
 * @param rdatalen: the length of rdatawl (including its 2 bytes length)
 * @return: the RPZ action
 */
static enum rpz_action
rpz_rr_to_action(uint16_t rr_type, uint8_t* rdatawl, size_t rdatalen)
{
	char* endptr;
	uint8_t* rdata;
	int rdatalabs;
	uint8_t* tldlab = NULL;

	switch(rr_type) {
		case LDNS_RR_TYPE_SOA:
		case LDNS_RR_TYPE_NS:
		case LDNS_RR_TYPE_DNAME:
		/* all DNSSEC-related RRs must be ignored */
		case LDNS_RR_TYPE_DNSKEY:
		case LDNS_RR_TYPE_DS:
		case LDNS_RR_TYPE_RRSIG:
		case LDNS_RR_TYPE_NSEC:
		case LDNS_RR_TYPE_NSEC3:
			return RPZ_INVALID_ACTION;
		case LDNS_RR_TYPE_CNAME:
			break;
		default:
			return RPZ_LOCAL_DATA_ACTION;
	}

	/* use CNAME target to determine RPZ action */
	log_assert(rr_type == LDNS_RR_TYPE_CNAME);
	if(rdatalen < 3)
		return RPZ_INVALID_ACTION;

	rdata = rdatawl + 2; /* 2 bytes of rdata length */
	if(dname_valid(rdata, rdatalen-2) != rdatalen-2)
		return RPZ_INVALID_ACTION;

	rdatalabs = dname_count_labels(rdata);
	if(rdatalabs == 1)
		return RPZ_NXDOMAIN_ACTION;
	else if(rdatalabs == 2) {
		if(dname_subdomain_c(rdata, (uint8_t*)&"\001*\000"))
			return RPZ_NODATA_ACTION;
		else if(dname_subdomain_c(rdata,
			(uint8_t*)&"\014rpz-passthru\000"))
			return RPZ_PASSTHRU_ACTION;
		else if(dname_subdomain_c(rdata, (uint8_t*)&"\010rpz-drop\000"))
			return RPZ_DROP_ACTION;
		else if(dname_subdomain_c(rdata,
			(uint8_t*)&"\014rpz-tcp-only\000"))
			return RPZ_TCP_ONLY_ACTION;
	}

	/* all other TLDs starting with "rpz-" are invalid */
	tldlab = get_tld_label(rdata, rdatalen-2);
	if(tldlab && dname_lab_startswith(tldlab, "rpz-", &endptr))
		return RPZ_INVALID_ACTION;

	/* no special label found */
	return RPZ_LOCAL_DATA_ACTION;
}

static enum localzone_type 
rpz_action_to_localzone_type(enum rpz_action a)
{
	switch(a) {
	case RPZ_NXDOMAIN_ACTION: return local_zone_always_nxdomain;
	case RPZ_NODATA_ACTION: return local_zone_always_nodata;
	case RPZ_DROP_ACTION: return local_zone_always_deny;
	case RPZ_PASSTHRU_ACTION: return local_zone_always_transparent;
	case RPZ_LOCAL_DATA_ACTION:	/* fallthrough */
	case RPZ_CNAME_OVERRIDE_ACTION: return local_zone_redirect;
	case RPZ_TCP_ONLY_ACTION: return local_zone_truncate;
	case RPZ_INVALID_ACTION: /* fallthrough */
	default: return local_zone_invalid;
	}
}

enum respip_action
rpz_action_to_respip_action(enum rpz_action a)
{
	switch(a) {
	case RPZ_NXDOMAIN_ACTION: return respip_always_nxdomain;
	case RPZ_NODATA_ACTION: return respip_always_nodata;
	case RPZ_DROP_ACTION: return respip_always_deny;
	case RPZ_PASSTHRU_ACTION: return respip_always_transparent;
	case RPZ_LOCAL_DATA_ACTION: /* fallthrough */
	case RPZ_CNAME_OVERRIDE_ACTION: return respip_redirect;
	case RPZ_TCP_ONLY_ACTION: return respip_truncate;
	case RPZ_INVALID_ACTION: /* fallthrough */
	default: return respip_invalid;
	}
}

static enum rpz_action
localzone_type_to_rpz_action(enum localzone_type lzt)
{
	switch(lzt) {
	case local_zone_always_nxdomain: return RPZ_NXDOMAIN_ACTION;
	case local_zone_always_nodata: return RPZ_NODATA_ACTION;
	case local_zone_always_deny: return RPZ_DROP_ACTION;
	case local_zone_always_transparent: return RPZ_PASSTHRU_ACTION;
	case local_zone_redirect: return RPZ_LOCAL_DATA_ACTION;
	case local_zone_truncate: return RPZ_TCP_ONLY_ACTION;
	case local_zone_invalid: /* fallthrough */
	default: return RPZ_INVALID_ACTION;
	}
}

enum rpz_action
respip_action_to_rpz_action(enum respip_action a)
{
	switch(a) {
	case respip_always_nxdomain: return RPZ_NXDOMAIN_ACTION;
	case respip_always_nodata: return RPZ_NODATA_ACTION;
	case respip_always_deny: return RPZ_DROP_ACTION;
	case respip_always_transparent: return RPZ_PASSTHRU_ACTION;
	case respip_redirect: return RPZ_LOCAL_DATA_ACTION;
	case respip_truncate: return RPZ_TCP_ONLY_ACTION;
	case respip_invalid: /* fallthrough */
	default: return RPZ_INVALID_ACTION;
	}
}

/**
 * Get RPZ trigger for dname
 * @param dname: dname containing RPZ trigger
 * @param dname_len: length of the dname
 * @return: RPZ trigger enum
 */
static enum rpz_trigger
rpz_dname_to_trigger(uint8_t* dname, size_t dname_len)
{
	uint8_t* tldlab;
	char* endptr;

	if(dname_valid(dname, dname_len) != dname_len)
		return RPZ_INVALID_TRIGGER;

	tldlab = get_tld_label(dname, dname_len);
	if(!tldlab || !dname_lab_startswith(tldlab, "rpz-", &endptr))
		return RPZ_QNAME_TRIGGER;

	if(dname_subdomain_c(tldlab,
		(uint8_t*)&"\015rpz-client-ip\000"))
		return RPZ_CLIENT_IP_TRIGGER;
	else if(dname_subdomain_c(tldlab, (uint8_t*)&"\006rpz-ip\000"))
		return RPZ_RESPONSE_IP_TRIGGER;
	else if(dname_subdomain_c(tldlab, (uint8_t*)&"\013rpz-nsdname\000"))
		return RPZ_NSDNAME_TRIGGER;
	else if(dname_subdomain_c(tldlab, (uint8_t*)&"\010rpz-nsip\000"))
		return RPZ_NSIP_TRIGGER;

	return RPZ_QNAME_TRIGGER;
}

static inline struct clientip_synthesized_rrset*
rpz_clientip_synthesized_set_create(void)
{
	struct clientip_synthesized_rrset* set = calloc(1, sizeof(*set));
	if(set == NULL) {
		return NULL;
	}
	set->region = regional_create();
	if(set->region == NULL) {
		free(set);
		return NULL;
	}
	addr_tree_init(&set->entries);
	lock_rw_init(&set->lock);
	return set;
}

static void
rpz_clientip_synthesized_rr_delete(rbnode_type* n, void* ATTR_UNUSED(arg))
{
	struct clientip_synthesized_rr* r = (struct clientip_synthesized_rr*)n->key;
	lock_rw_destroy(&r->lock);
#ifdef THREADS_DISABLED
	(void)r;
#endif
}

static inline void
rpz_clientip_synthesized_set_delete(struct clientip_synthesized_rrset* set)
{
	if(set == NULL) {
		return;
	}
	lock_rw_destroy(&set->lock);
	traverse_postorder(&set->entries, rpz_clientip_synthesized_rr_delete, NULL);
	regional_destroy(set->region);
	free(set);
}

void
rpz_delete(struct rpz* r)
{
	if(!r)
		return;
	local_zones_delete(r->local_zones);
	local_zones_delete(r->nsdname_zones);
	respip_set_delete(r->respip_set);
	rpz_clientip_synthesized_set_delete(r->client_set);
	rpz_clientip_synthesized_set_delete(r->ns_set);
	regional_destroy(r->region);
	free(r->taglist);
	free(r->log_name);
	free(r);
}

int
rpz_clear(struct rpz* r)
{
	/* must hold write lock on auth_zone */
	local_zones_delete(r->local_zones);
	local_zones_delete(r->nsdname_zones);
	respip_set_delete(r->respip_set);
	rpz_clientip_synthesized_set_delete(r->client_set);
	rpz_clientip_synthesized_set_delete(r->ns_set);
	// XXX: this approach seems to leak memory
	if(!(r->local_zones = local_zones_create())){
		return 0;
	}
	r->nsdname_zones = local_zones_create();
	if(r->nsdname_zones == NULL) {
		return 0;
	}
	if(!(r->respip_set = respip_set_create())) {
		return 0;
	}
	if(!(r->client_set = rpz_clientip_synthesized_set_create())) {
		return 0;
	}
	if(!(r->ns_set = rpz_clientip_synthesized_set_create())) {
		return 0;
	}
	return 1;
}

void
rpz_finish_config(struct rpz* r)
{
	lock_rw_wrlock(&r->respip_set->lock);
	addr_tree_init_parents(&r->respip_set->ip_tree);
	lock_rw_unlock(&r->respip_set->lock);

	lock_rw_wrlock(&r->client_set->lock);
	addr_tree_init_parents(&r->client_set->entries);
	lock_rw_unlock(&r->client_set->lock);

	lock_rw_wrlock(&r->ns_set->lock);
	addr_tree_init_parents(&r->ns_set->entries);
	lock_rw_unlock(&r->ns_set->lock);
}

/** new rrset containing CNAME override, does not yet contain a dname */
static struct ub_packed_rrset_key*
new_cname_override(struct regional* region, uint8_t* ct, size_t ctlen)
{
	struct ub_packed_rrset_key* rrset;
	struct packed_rrset_data* pd;
	uint16_t rdlength = htons(ctlen);
	rrset = (struct ub_packed_rrset_key*)regional_alloc_zero(region,
		sizeof(*rrset));
	if(!rrset) {
		log_err("out of memory");
		return NULL;
	}
	rrset->entry.key = rrset;
	pd = (struct packed_rrset_data*)regional_alloc_zero(region, sizeof(*pd));
	if(!pd) {
		log_err("out of memory");
		return NULL;
	}
	pd->trust = rrset_trust_prim_noglue;
	pd->security = sec_status_insecure;

	pd->count = 1;
	pd->rr_len = regional_alloc_zero(region, sizeof(*pd->rr_len));
	pd->rr_ttl = regional_alloc_zero(region, sizeof(*pd->rr_ttl));
	pd->rr_data = regional_alloc_zero(region, sizeof(*pd->rr_data));
	if(!pd->rr_len || !pd->rr_ttl || !pd->rr_data) {
		log_err("out of memory");
		return NULL;
	}
	pd->rr_len[0] = ctlen+2;
	pd->rr_ttl[0] = 3600;
	pd->rr_data[0] = regional_alloc_zero(region, 2 /* rdlength */ + ctlen);
	if(!pd->rr_data[0]) {
		log_err("out of memory");
		return NULL;
	}
	memmove(pd->rr_data[0], &rdlength, 2);
	memmove(pd->rr_data[0]+2, ct, ctlen);

	rrset->entry.data = pd;
	rrset->rk.type = htons(LDNS_RR_TYPE_CNAME);
	rrset->rk.rrset_class = htons(LDNS_RR_CLASS_IN);
	return rrset;
}

struct rpz*
rpz_create(struct config_auth* p)
{
	struct rpz* r = calloc(1, sizeof(*r));
	if(!r)
		goto err;

	r->region = regional_create_custom(sizeof(struct regional));
	if(!r->region) {
		goto err;
	}

	if(!(r->local_zones = local_zones_create())){
		goto err;
	}

	r->nsdname_zones = local_zones_create();
	if(r->local_zones == NULL){
		goto err;
	}

	if(!(r->respip_set = respip_set_create())) {
		goto err;
	}

	r->client_set = rpz_clientip_synthesized_set_create();
	if(r->client_set == NULL) {
		goto err;
	}

	r->ns_set = rpz_clientip_synthesized_set_create();
	if(r->ns_set == NULL) {
		goto err;
	}

	r->taglistlen = p->rpz_taglistlen;
	r->taglist = memdup(p->rpz_taglist, r->taglistlen);
	if(p->rpz_action_override) {
		r->action_override = rpz_config_to_action(p->rpz_action_override);
	}
	else
		r->action_override = RPZ_NO_OVERRIDE_ACTION;

	if(r->action_override == RPZ_CNAME_OVERRIDE_ACTION) {
		uint8_t nm[LDNS_MAX_DOMAINLEN+1];
		size_t nmlen = sizeof(nm);

		if(!p->rpz_cname) {
			log_err("RPZ override with cname action found, but no "
				"rpz-cname-override configured");
			goto err;
		}

		if(sldns_str2wire_dname_buf(p->rpz_cname, nm, &nmlen) != 0) {
			log_err("cannot parse RPZ cname override: %s",
				p->rpz_cname);
			goto err;
		}
		r->cname_override = new_cname_override(r->region, nm, nmlen);
		if(!r->cname_override) {
			goto err;
		}
	}
	r->log = p->rpz_log;
	if(p->rpz_log_name) {
		if(!(r->log_name = strdup(p->rpz_log_name))) {
			log_err("malloc failure on RPZ log_name strdup");
			goto err;
		}
	}
	return r;
err:
	if(r) {
		if(r->local_zones)
			local_zones_delete(r->local_zones);
		if(r->nsdname_zones)
			local_zones_delete(r->nsdname_zones);
		if(r->respip_set)
			respip_set_delete(r->respip_set);
		if(r->client_set != NULL)
			rpz_clientip_synthesized_set_delete(r->client_set);
		if(r->ns_set != NULL)
			rpz_clientip_synthesized_set_delete(r->ns_set);
		if(r->taglist)
			free(r->taglist);
		if(r->region)
			regional_destroy(r->region);
		free(r);
	}
	return NULL;
}

/**
 * Remove RPZ zone name from dname
 * Copy dname to newdname, without the originlen number of trailing bytes
 */
static size_t
strip_dname_origin(uint8_t* dname, size_t dnamelen, size_t originlen,
	uint8_t* newdname, size_t maxnewdnamelen)
{
	size_t newdnamelen;
	if(dnamelen < originlen)
		return 0;
	newdnamelen = dnamelen - originlen;
	if(newdnamelen+1 > maxnewdnamelen)
		return 0;
	memmove(newdname, dname, newdnamelen);
	newdname[newdnamelen] = 0;
	return newdnamelen + 1;	/* + 1 for root label */
}

static void
rpz_insert_local_zones_trigger(struct local_zones* lz, uint8_t* dname,
	size_t dnamelen, enum rpz_action a, uint16_t rrtype, uint16_t rrclass,
	uint32_t ttl, uint8_t* rdata, size_t rdata_len, uint8_t* rr, size_t rr_len)
{
	struct local_zone* z;
	enum localzone_type tp = local_zone_always_transparent;
	int dnamelabs = dname_count_labels(dname);

	int newzone = 0;

	lock_rw_wrlock(&lz->lock);
	/* exact match */
	z = local_zones_find(lz, dname, dnamelen, dnamelabs, LDNS_RR_CLASS_IN);
	if(z != NULL && a != RPZ_LOCAL_DATA_ACTION) {
		char* rrstr = sldns_wire2str_rr(rr, rr_len);
		if(rrstr == NULL) {
			log_err("malloc error while inserting rpz nsdname trigger");
			free(dname);
			lock_rw_unlock(&lz->lock);
			return;
		}
		verbose(VERB_ALGO, "rpz: skipping duplicate record: '%s'", rrstr);
		free(rrstr);
		free(dname);
		lock_rw_unlock(&lz->lock);
		return;
	}
	if(z == NULL) {
		tp = rpz_action_to_localzone_type(a);
		z = local_zones_add_zone(lz, dname, dnamelen,
					 dnamelabs, rrclass, tp);
		if(z == NULL) {
			log_warn("rpz: create failed");
			lock_rw_unlock(&lz->lock);
			/* dname will be free'd in failed local_zone_create() */
			return;
		}
		newzone = 1;
	}
	if(a == RPZ_LOCAL_DATA_ACTION) {
		char* rrstr = sldns_wire2str_rr(rr, rr_len);
		if(rrstr == NULL) {
			log_err("malloc error while inserting rpz nsdname trigger");
			free(dname);
			lock_rw_unlock(&lz->lock);
			return;
		}
		lock_rw_wrlock(&z->lock);
		local_zone_enter_rr(z, dname, dnamelen, dnamelabs, rrtype,
				    rrclass, ttl, rdata, rdata_len, rrstr);
		lock_rw_unlock(&z->lock);
		free(rrstr);
	}
	if(!newzone) {
		free(dname);
	}
	lock_rw_unlock(&lz->lock);
}

static void
rpz_log_dname(char const* msg, uint8_t* dname, size_t dname_len)
{
	char buf[LDNS_MAX_DOMAINLEN+1];
	(void)dname_len;
	dname_str(dname, buf);
	verbose(VERB_ALGO, "rpz: %s: <%s>", msg, buf);
}

static void
rpz_insert_qname_trigger(struct rpz* r, uint8_t* dname, size_t dnamelen,
	enum rpz_action a, uint16_t rrtype, uint16_t rrclass, uint32_t ttl,
	uint8_t* rdata, size_t rdata_len, uint8_t* rr, size_t rr_len)
{
	verbose(VERB_ALGO, "rpz: insert qname trigger: %s", rpz_action_to_string(a));

	rpz_log_dname("insert qname trigger", dname, dnamelen);

	if(a == RPZ_INVALID_ACTION) {
		verbose(VERB_ALGO, "rpz: skipping invalid action");
		free(dname);
		return;
	}

	rpz_insert_local_zones_trigger(r->local_zones, dname, dnamelen, a, rrtype,
				       rrclass, ttl, rdata, rdata_len, rr, rr_len);
}

static void
rpz_strip_nsdname_suffix(uint8_t* dname, size_t maxdnamelen)
{
	uint8_t* stripped = get_tld_label(dname, maxdnamelen);
	if(stripped == NULL) { return; }
	*stripped = 0;
}

static void
rpz_insert_nsdname_trigger(struct rpz* r, uint8_t* dname, size_t dnamelen,
	enum rpz_action a, uint16_t rrtype, uint16_t rrclass, uint32_t ttl,
	uint8_t* rdata, size_t rdata_len, uint8_t* rr, size_t rr_len)
{
	verbose(VERB_ALGO, "rpz: insert nsdname trigger: %s", rpz_action_to_string(a));

	rpz_log_dname("insert nsdname trigger", dname, dnamelen);
	rpz_strip_nsdname_suffix(dname, dnamelen);
	rpz_log_dname("insert nsdname trigger (stripped)", dname, dnamelen);

	if(a == RPZ_INVALID_ACTION) {
		verbose(VERB_ALGO, "rpz: skipping invalid action");
		free(dname);
		return;
	}

	rpz_insert_local_zones_trigger(r->nsdname_zones, dname, dnamelen, a, rrtype,
				       rrclass, ttl, rdata, rdata_len, rr, rr_len);
}

static int
rpz_insert_ipaddr_based_trigger(struct respip_set* set, struct sockaddr_storage* addr,
	socklen_t addrlen, int net, enum rpz_action a, uint16_t rrtype,
	uint16_t rrclass, uint32_t ttl, uint8_t* rdata, size_t rdata_len,
	uint8_t* rr, size_t rr_len)
{
	struct resp_addr* node;
	char* rrstr;
	enum respip_action respa = rpz_action_to_respip_action(a);

	lock_rw_wrlock(&set->lock);

	rrstr = sldns_wire2str_rr(rr, rr_len);
	if(rrstr == NULL) {
		log_err("malloc error while inserting rpz ipaddr based trigger");
		lock_rw_unlock(&set->lock);
		return 0;
	}

	node = respip_sockaddr_find_or_create(set, addr, addrlen, net, 1, rrstr);
	if(node == NULL) {
		lock_rw_unlock(&set->lock);
		free(rrstr);
		return 0;
	}

	lock_rw_wrlock(&node->lock);
	lock_rw_unlock(&set->lock);

	node->action = respa;

	if(a == RPZ_LOCAL_DATA_ACTION) {
		respip_enter_rr(set->region, node, rrtype,
				rrclass, ttl, rdata, rdata_len, rrstr, "");
	}

	lock_rw_unlock(&node->lock);
	free(rrstr);
	return 1;
}

static inline struct clientip_synthesized_rr*
rpz_clientip_ensure_entry(struct clientip_synthesized_rrset* set,
	struct sockaddr_storage* addr, socklen_t addrlen, int net)
{
	int insert_ok;
	struct clientip_synthesized_rr* node =
		(struct clientip_synthesized_rr*)addr_tree_find(&set->entries,
								addr, addrlen, net);

	if(node != NULL) { return node; }

	/* node does not yet exist => allocate one */
	node = regional_alloc_zero(set->region, sizeof(*node));
	if(node == NULL) {
		log_err("out of memory");
		return NULL;
	}

	lock_rw_init(&node->lock);
	node->action = RPZ_INVALID_ACTION;
	insert_ok = addr_tree_insert(&set->entries, &node->node,
				     addr, addrlen, net);
	if (!insert_ok) {
		log_warn("rpz: unexpected: unable to insert clientip address node");
		/* we can not free the just allocated node.
		 * theoretically a memleak */
		return NULL;
	}

	return node;
}

static void
rpz_report_rrset_error(const char* msg, uint8_t* rr, size_t rr_len) {
	char* rrstr = sldns_wire2str_rr(rr, rr_len);
	if(rrstr == NULL) {
		log_err("malloc error while inserting rpz clientip based record");
		return;
	}
	log_err("rpz: unexpected: unable to insert %s: %s", msg, rrstr);
	free(rrstr);
}

/* from localzone.c; difference is we don't have a dname */
struct local_rrset*
rpz_clientip_new_rrset(struct regional* region,
	struct clientip_synthesized_rr* raddr, uint16_t rrtype, uint16_t rrclass)
{
	struct packed_rrset_data* pd;
	struct local_rrset* rrset = (struct local_rrset*)
		regional_alloc_zero(region, sizeof(*rrset));
	if(rrset == NULL) {
		log_err("out of memory");
		return NULL;
	}
	rrset->next = raddr->data;
	raddr->data = rrset;
	rrset->rrset = (struct ub_packed_rrset_key*)
		regional_alloc_zero(region, sizeof(*rrset->rrset));
	if(rrset->rrset == NULL) {
		log_err("out of memory");
		return NULL;
	}
	rrset->rrset->entry.key = rrset->rrset;
	pd = (struct packed_rrset_data*)regional_alloc_zero(region, sizeof(*pd));
	if(pd == NULL) {
		log_err("out of memory");
		return NULL;
	}
	pd->trust = rrset_trust_prim_noglue;
	pd->security = sec_status_insecure;
	rrset->rrset->entry.data = pd;
	rrset->rrset->rk.type = htons(rrtype);
	rrset->rrset->rk.rrset_class = htons(rrclass);
	rrset->rrset->rk.dname = regional_alloc_zero(region, 1);
	if(rrset->rrset->rk.dname == NULL) {
		log_err("out of memory");
		return NULL;
	}
	rrset->rrset->rk.dname_len = 1;
	return rrset;
}

static int
rpz_clientip_enter_rr(struct regional* region, struct clientip_synthesized_rr* raddr,
	uint16_t rrtype, uint16_t rrclass, time_t ttl, uint8_t* rdata,
	size_t rdata_len)
{
	struct local_rrset* rrset;
	struct sockaddr* sa;
	sa = (struct sockaddr*)&raddr->node.addr;
	if (rrtype == LDNS_RR_TYPE_CNAME && raddr->data != NULL) {
		log_err("CNAME response-ip data can not co-exist with other "
			"client-ip data");
		return 0;
	}

	rrset = rpz_clientip_new_rrset(region, raddr, rrtype, rrclass);
	if(raddr->data == NULL) {
		return 0;
	}

	return rrset_insert_rr(region, rrset->rrset->entry.data, rdata, rdata_len, ttl, "");
}

static int
rpz_clientip_insert_trigger_rr(struct clientip_synthesized_rrset* set, struct sockaddr_storage* addr,
	socklen_t addrlen, int net, enum rpz_action a, uint16_t rrtype,
	uint16_t rrclass, uint32_t ttl, uint8_t* rdata, size_t rdata_len,
	uint8_t* rr, size_t rr_len)
{
	struct clientip_synthesized_rr* node;

	lock_rw_wrlock(&set->lock);

	node = rpz_clientip_ensure_entry(set, addr, addrlen, net);
	if(node == NULL) {
		lock_rw_unlock(&set->lock);
		rpz_report_rrset_error("client ip address", rr, rr_len);
		return 0;
	}

	lock_rw_wrlock(&node->lock);
	lock_rw_unlock(&set->lock);

	node->action = a;
	if(a == RPZ_LOCAL_DATA_ACTION) {
		if(!rpz_clientip_enter_rr(set->region, node, rrtype,
			rrclass, ttl, rdata, rdata_len)) {
			verbose(VERB_ALGO, "rpz: unable to insert clientip rr");
			lock_rw_unlock(&node->lock);
			return 0;
		}

	}

	lock_rw_unlock(&node->lock);

	return 1;
}

static int
rpz_insert_clientip_trigger(struct rpz* r, uint8_t* dname, size_t dnamelen,
	enum rpz_action a, uint16_t rrtype, uint16_t rrclass, uint32_t ttl,
	uint8_t* rdata, size_t rdata_len, uint8_t* rr, size_t rr_len)
{
	struct sockaddr_storage addr;
	socklen_t addrlen;
	int net, af;

	verbose(VERB_ALGO, "rpz: insert clientip trigger: %s", rpz_action_to_string(a));
	if(a == RPZ_INVALID_ACTION) {
		return 0;
	}

	if(!netblockdnametoaddr(dname, dnamelen, &addr, &addrlen, &net, &af)) {
		verbose(VERB_ALGO, "rpz: unable to parse client ip");
		return 0;
	}

	return rpz_clientip_insert_trigger_rr(r->client_set, &addr, addrlen, net,
			a, rrtype, rrclass, ttl, rdata, rdata_len, rr, rr_len);
}

static int
rpz_insert_nsip_trigger(struct rpz* r, uint8_t* dname, size_t dnamelen,
	enum rpz_action a, uint16_t rrtype, uint16_t rrclass, uint32_t ttl,
	uint8_t* rdata, size_t rdata_len, uint8_t* rr, size_t rr_len)
{
	struct sockaddr_storage addr;
	socklen_t addrlen;
	int net, af;

	verbose(VERB_ALGO, "rpz: insert nsip trigger: %s", rpz_action_to_string(a));
	if(a == RPZ_INVALID_ACTION) {
		return 0;
	}

	if(!netblockdnametoaddr(dname, dnamelen, &addr, &addrlen, &net, &af)) {
		verbose(VERB_ALGO, "rpz: unable to parse ns ip");
		return 0;
	}

	return rpz_clientip_insert_trigger_rr(r->ns_set, &addr, addrlen, net,
			a, rrtype, rrclass, ttl, rdata, rdata_len, rr, rr_len);
}

/** Insert RR into RPZ's respip_set */
static int
rpz_insert_response_ip_trigger(struct rpz* r, uint8_t* dname, size_t dnamelen,
	enum rpz_action a, uint16_t rrtype, uint16_t rrclass, uint32_t ttl,
	uint8_t* rdata, size_t rdata_len, uint8_t* rr, size_t rr_len)
{
	struct sockaddr_storage addr;
	socklen_t addrlen;
	int net, af;

	verbose(VERB_ALGO, "rpz: insert response ip trigger: %s", rpz_action_to_string(a));
	if(a == RPZ_INVALID_ACTION) {
		return 0;
	}

	if(!netblockdnametoaddr(dname, dnamelen, &addr, &addrlen, &net, &af)) {
		verbose(VERB_ALGO, "rpz: unable to parse response ip");
		return 0;
	}

	return rpz_insert_ipaddr_based_trigger(r->respip_set, &addr, addrlen, net,
			a, rrtype, rrclass, ttl, rdata, rdata_len, rr, rr_len);
}

int
rpz_insert_rr(struct rpz* r, uint8_t* azname, size_t aznamelen, uint8_t* dname,
	size_t dnamelen, uint16_t rr_type, uint16_t rr_class, uint32_t rr_ttl,
	uint8_t* rdatawl, size_t rdatalen, uint8_t* rr, size_t rr_len)
{
	size_t policydnamelen;
	/* name is free'd in local_zone delete */
	enum rpz_trigger t;
	enum rpz_action a;
	uint8_t* policydname;

	if(!dname_subdomain_c(dname, azname)) {
		char* dname_str = sldns_wire2str_dname(dname, dnamelen);
		char* azname_str = sldns_wire2str_dname(azname, aznamelen);
		if(dname_str && azname_str) {
			log_err("rpz: name of record (%s) to insert into RPZ is not a "
				"subdomain of the configured name of the RPZ zone (%s)",
				dname_str, azname_str);
		} else {
			log_err("rpz: name of record to insert into RPZ is not a "
				"subdomain of the configured name of the RPZ zone");
		}
		free(dname_str);
		free(azname_str);
		return 0;
	}

	log_assert(dnamelen >= aznamelen);
	if(!(policydname = calloc(1, (dnamelen-aznamelen)+1))) {
		log_err("malloc error while inserting RPZ RR");
		return 0;
	}

	a = rpz_rr_to_action(rr_type, rdatawl, rdatalen);
	if(!(policydnamelen = strip_dname_origin(dname, dnamelen, aznamelen,
		policydname, (dnamelen-aznamelen)+1))) {
		free(policydname);
		return 0;
	}
	t = rpz_dname_to_trigger(policydname, policydnamelen);
	if(t == RPZ_INVALID_TRIGGER) {
		free(policydname);
		verbose(VERB_ALGO, "rpz: skipping invalid trigger");
		return 1;
	}
	if(t == RPZ_QNAME_TRIGGER) {
		// policydname will be consumed, no free
		rpz_insert_qname_trigger(r, policydname, policydnamelen,
			a, rr_type, rr_class, rr_ttl, rdatawl, rdatalen, rr,
			rr_len);
	} else if(t == RPZ_RESPONSE_IP_TRIGGER) {
		rpz_insert_response_ip_trigger(r, policydname, policydnamelen,
			a, rr_type, rr_class, rr_ttl, rdatawl, rdatalen, rr,
			rr_len);
		free(policydname);
	} else if(t == RPZ_CLIENT_IP_TRIGGER) {
		rpz_insert_clientip_trigger(r, policydname, policydnamelen,
			a, rr_type, rr_class, rr_ttl, rdatawl, rdatalen, rr,
			rr_len);
		free(policydname);
	} else if(t == RPZ_NSIP_TRIGGER) {
		rpz_insert_nsip_trigger(r, policydname, policydnamelen,
			a, rr_type, rr_class, rr_ttl, rdatawl, rdatalen, rr,
			rr_len);
		free(policydname);
	} else if(t == RPZ_NSDNAME_TRIGGER) {
		// policydname will be consumed, no free
		rpz_insert_nsdname_trigger(r, policydname, policydnamelen,
			a, rr_type, rr_class, rr_ttl, rdatawl, rdatalen, rr,
			rr_len);
	} else {
		free(policydname);
		verbose(VERB_ALGO, "rpz: skipping unsupported trigger: %s",
			rpz_trigger_to_string(t));
	}
	return 1;
}

/**
 * Find RPZ local-zone by qname.
 * @param r: rpz containing local-zone tree
 * @param qname: qname
 * @param qname_len: length of qname
 * @param qclass: qclass
 * @param only_exact: if 1 only excact (non wildcard) matches are returned
 * @param wr: get write lock for local-zone if 1, read lock if 0
 * @param zones_keep_lock: if set do not release the r->local_zones lock, this
 * 	  makes the caller of this function responsible for releasing the lock.
 * @return: NULL or local-zone holding rd or wr lock
 */
static struct local_zone*
rpz_find_zone(struct local_zones* zones, uint8_t* qname, size_t qname_len, uint16_t qclass,
	int only_exact, int wr, int zones_keep_lock)
{
	uint8_t* ce;
	size_t ce_len, ce_labs;
	uint8_t wc[LDNS_MAX_DOMAINLEN+1];
	int exact;
	struct local_zone* z = NULL;

	if(wr) {
		lock_rw_wrlock(&zones->lock);
	} else {
		lock_rw_rdlock(&zones->lock);
	}
	z = local_zones_find_le(zones, qname, qname_len,
		dname_count_labels(qname),
		LDNS_RR_CLASS_IN, &exact);
	if(!z || (only_exact && !exact)) {
		lock_rw_unlock(&zones->lock);
		return NULL;
	}
	if(wr) {
		lock_rw_wrlock(&z->lock);
	} else {
		lock_rw_rdlock(&z->lock);
	}
	if(!zones_keep_lock) {
		lock_rw_unlock(&zones->lock);
	}

	if(exact)
		return z;

	/* No exact match found, lookup wildcard. closest encloser must
	 * be the shared parent between the qname and the best local
	 * zone match, append '*' to that and do another lookup. */

	ce = dname_get_shared_topdomain(z->name, qname);
	if(!ce /* should not happen */ || !*ce /* root */) {
		lock_rw_unlock(&z->lock);
		if(zones_keep_lock) {
			lock_rw_unlock(&zones->lock);
		}
		return NULL;
	}
	ce_labs = dname_count_size_labels(ce, &ce_len);
	if(ce_len+2 > sizeof(wc)) {
		lock_rw_unlock(&z->lock);
		if(zones_keep_lock) {
			lock_rw_unlock(&zones->lock);
		}
		return NULL;
	}
	wc[0] = 1; /* length of wildcard label */
	wc[1] = (uint8_t)'*'; /* wildcard label */
	memmove(wc+2, ce, ce_len);
	lock_rw_unlock(&z->lock);

	if(!zones_keep_lock) {
		if(wr) {
			lock_rw_wrlock(&zones->lock);
		} else {
			lock_rw_rdlock(&zones->lock);
		}
	}
	z = local_zones_find_le(zones, wc,
		ce_len+2, ce_labs+1, qclass, &exact);
	if(!z || !exact) {
		lock_rw_unlock(&zones->lock);
		return NULL;
	}
	if(wr) {
		lock_rw_wrlock(&z->lock);
	} else {
		lock_rw_rdlock(&z->lock);
	}
	if(!zones_keep_lock) {
		lock_rw_unlock(&zones->lock);
	}
	return z;
}

/**
 * Remove RR from RPZ's local-data
 * @param z: local-zone for RPZ, holding write lock
 * @param policydname: dname of RR to remove
 * @param policydnamelen: lenth of policydname
 * @param rr_type: RR type of RR to remove
 * @param rdata: rdata of RR to remove
 * @param rdatalen: length of rdata
 * @return: 1 if zone must be removed after RR deletion
 */
static int
rpz_data_delete_rr(struct local_zone* z, uint8_t* policydname,
	size_t policydnamelen, uint16_t rr_type, uint8_t* rdata,
	size_t rdatalen)
{
	struct local_data* ld;
	struct packed_rrset_data* d;
	size_t index;
	ld = local_zone_find_data(z, policydname, policydnamelen,
		dname_count_labels(policydname));
	if(ld) {
		struct local_rrset* prev=NULL, *p=ld->rrsets;
		while(p && ntohs(p->rrset->rk.type) != rr_type) {
			prev = p;
			p = p->next;
		}
		if(!p)
			return 0;
		d = (struct packed_rrset_data*)p->rrset->entry.data;
		if(packed_rrset_find_rr(d, rdata, rdatalen, &index)) {
			if(d->count == 1) {
				/* no memory recycling for zone deletions ... */
				if(prev) prev->next = p->next;
				else ld->rrsets = p->next;
			}
			if(d->count > 1) {
				if(!local_rrset_remove_rr(d, index))
					return 0;
			}
		}
	}
	if(ld && ld->rrsets)
		return 0;
	return 1;
}

/**
 * Remove RR from RPZ's respip set
 * @param raddr: respip node
 * @param rr_type: RR type of RR to remove
 * @param rdata: rdata of RR to remove
 * @param rdatalen: length of rdata
 * @return: 1 if zone must be removed after RR deletion
 */
static int
rpz_rrset_delete_rr(struct resp_addr* raddr, uint16_t rr_type, uint8_t* rdata,
	size_t rdatalen)
{
	size_t index;
	struct packed_rrset_data* d;
	if(!raddr->data)
		return 1;
	d = raddr->data->entry.data;
	if(ntohs(raddr->data->rk.type) != rr_type) {
		return 0;
	}
	if(packed_rrset_find_rr(d, rdata, rdatalen, &index)) {
		if(d->count == 1) {
			/* regional alloc'd */
			raddr->data->entry.data = NULL; 
			raddr->data = NULL;
			return 1;
		}
		if(d->count > 1) {
			if(!local_rrset_remove_rr(d, index))
				return 0;
		}
	}
	return 0;

}

/** Remove RR from RPZ's local-zone */
static void
rpz_remove_qname_trigger(struct rpz* r, uint8_t* dname, size_t dnamelen,
	enum rpz_action a, uint16_t rr_type, uint16_t rr_class,
	uint8_t* rdatawl, size_t rdatalen)
{
	struct local_zone* z;
	int delete_zone = 1;
	z = rpz_find_zone(r->local_zones, dname, dnamelen, rr_class,
		1 /* only exact */, 1 /* wr lock */, 1 /* keep lock*/);
	if(!z) {
		verbose(VERB_ALGO, "rpz: cannot remove RR from IXFR, "
			"RPZ domain not found");
		return;
	}
	if(a == RPZ_LOCAL_DATA_ACTION)
		delete_zone = rpz_data_delete_rr(z, dname,
			dnamelen, rr_type, rdatawl, rdatalen);
	else if(a != localzone_type_to_rpz_action(z->type)) {
		lock_rw_unlock(&z->lock);
		lock_rw_unlock(&r->local_zones->lock);
		return;
	}
	lock_rw_unlock(&z->lock); 
	if(delete_zone) {
		local_zones_del_zone(r->local_zones, z);
	}
	lock_rw_unlock(&r->local_zones->lock); 
	return;
}

static void
rpz_remove_response_ip_trigger(struct rpz* r, uint8_t* dname, size_t dnamelen,
	enum rpz_action a, uint16_t rr_type, uint8_t* rdatawl, size_t rdatalen)
{
	struct resp_addr* node;
	struct sockaddr_storage addr;
	socklen_t addrlen;
	int net, af;
	int delete_respip = 1;

	if(!netblockdnametoaddr(dname, dnamelen, &addr, &addrlen, &net, &af))
		return;

	lock_rw_wrlock(&r->respip_set->lock);
	if(!(node = (struct resp_addr*)addr_tree_find(
		&r->respip_set->ip_tree, &addr, addrlen, net))) {
		verbose(VERB_ALGO, "rpz: cannot remove RR from IXFR, "
			"RPZ domain not found");
		lock_rw_unlock(&r->respip_set->lock);
		return;
	}

	lock_rw_wrlock(&node->lock);
	if(a == RPZ_LOCAL_DATA_ACTION) {
		/* remove RR, signal whether RR can be removed */
		delete_respip = rpz_rrset_delete_rr(node, rr_type, rdatawl, 
			rdatalen);
	}
	lock_rw_unlock(&node->lock);
	if(delete_respip)
		respip_sockaddr_delete(r->respip_set, node);
	lock_rw_unlock(&r->respip_set->lock);
}

void
rpz_remove_rr(struct rpz* r, size_t aznamelen, uint8_t* dname, size_t dnamelen,
	uint16_t rr_type, uint16_t rr_class, uint8_t* rdatawl, size_t rdatalen)
{
	size_t policydnamelen;
	enum rpz_trigger t;
	enum rpz_action a;
	uint8_t* policydname;

	if(!(policydname = calloc(1, LDNS_MAX_DOMAINLEN + 1)))
		return;

	a = rpz_rr_to_action(rr_type, rdatawl, rdatalen);
	if(a == RPZ_INVALID_ACTION) {
		free(policydname);
		return;
	}
	if(!(policydnamelen = strip_dname_origin(dname, dnamelen, aznamelen,
		policydname, LDNS_MAX_DOMAINLEN + 1))) {
		free(policydname);
		return;
	}
	t = rpz_dname_to_trigger(policydname, policydnamelen);
	if(t == RPZ_QNAME_TRIGGER) {
		rpz_remove_qname_trigger(r, policydname, policydnamelen, a,
			rr_type, rr_class, rdatawl, rdatalen);
	} else if(t == RPZ_RESPONSE_IP_TRIGGER) {
		rpz_remove_response_ip_trigger(r, policydname, policydnamelen,
			a, rr_type, rdatawl, rdatalen);
	}
	free(policydname);
}

/** print log information for an applied RPZ policy. Based on local-zone's
 * lz_inform_print().
 */
static void
log_rpz_apply(uint8_t* dname, enum rpz_action a, struct query_info* qinfo,
	struct comm_reply* repinfo, char* log_name)
{
	char ip[128], txt[512];
	char dnamestr[LDNS_MAX_DOMAINLEN+1];
	uint16_t port = ntohs(((struct sockaddr_in*)&repinfo->addr)->sin_port);
	dname_str(dname, dnamestr);
	addr_to_str(&repinfo->addr, repinfo->addrlen, ip, sizeof(ip));
	if(log_name)
		snprintf(txt, sizeof(txt), "rpz: applied [%s] %s %s %s@%u",
			log_name, dnamestr, rpz_action_to_string(a), ip,
			(unsigned)port);
	else
		snprintf(txt, sizeof(txt), "rpz: applied %s %s %s@%u",
			dnamestr, rpz_action_to_string(a), ip, (unsigned)port);
	log_nametypeclass(0, txt, qinfo->qname, qinfo->qtype, qinfo->qclass);
}

static struct clientip_synthesized_rr*
rpz_ipbased_trigger_lookup(struct clientip_synthesized_rrset* set,
			   struct sockaddr_storage* addr, socklen_t addrlen)
{
	struct clientip_synthesized_rr* raddr = NULL;
	enum rpz_action action = RPZ_INVALID_ACTION;

	lock_rw_rdlock(&set->lock);

	raddr = (struct clientip_synthesized_rr*)addr_tree_lookup(&set->entries,
			addr, addrlen);
	if(raddr != NULL) {
		action = raddr->action;
		lock_rw_unlock(&raddr->lock);
	}

	verbose(VERB_ALGO, "rpz: ipbased trigger lookup: found=%d action=%s",
		raddr != NULL, rpz_action_to_string(action));

	lock_rw_unlock(&set->lock);

	return raddr;
}

static inline
struct clientip_synthesized_rr*
rpz_resolve_client_action_and_zone(struct auth_zones* az, struct query_info* qinfo,
	struct comm_reply* repinfo, uint8_t* taglist, size_t taglen,
	struct ub_server_stats* stats,
	/* output parameters */
	struct local_zone** z_out, struct auth_zone** a_out, struct rpz** r_out)
{
	struct clientip_synthesized_rr* node = NULL;
	struct auth_zone* a = NULL;
	struct rpz* r = NULL;
	struct local_zone* z = NULL;

	lock_rw_rdlock(&az->rpz_lock);

	for(a = az->rpz_first; a; a = a->rpz_az_next) {
		lock_rw_rdlock(&a->lock);
		r = a->rpz;
		if(r->taglist && !taglist_intersect(r->taglist,
					r->taglistlen, taglist, taglen)) {
			lock_rw_unlock(&a->lock);
			continue;
		}
		z = rpz_find_zone(r->local_zones, qinfo->qname, qinfo->qname_len,
			qinfo->qclass, 0, 0, 0);
		node = rpz_ipbased_trigger_lookup(r->client_set, &repinfo->addr, repinfo->addrlen);
		if(z && r->action_override == RPZ_DISABLED_ACTION) {
			if(r->log)
				log_rpz_apply(z->name,
					r->action_override,
					qinfo, repinfo, r->log_name);
			/* TODO only register stats when stats_extended? */
			stats->rpz_action[r->action_override]++;
			lock_rw_unlock(&z->lock);
			z = NULL;
		}
		if(z) {
			break;
		} else {
			if(node != NULL) {
				lock_rw_unlock(&node->lock);
				node = NULL;
			}
		}
		/* not found in this auth_zone */
		lock_rw_unlock(&a->lock);
	}

	lock_rw_unlock(&az->rpz_lock);

	*r_out = r;
	*a_out = a;
	*z_out = z;

	return node;
}

static inline int
rpz_is_udp_query(struct comm_reply* repinfo) {
	return repinfo != NULL
			? (repinfo->c != NULL
				? repinfo->c->type == comm_udp
				: 0)
			: 0;
}

/** encode answer consisting of 1 rrset */
static int
rpz_local_encode(struct query_info* qinfo,struct edns_data* edns, sldns_buffer* buf,
	struct regional* temp, struct ub_packed_rrset_key* rrset, int ansec, int rcode)
{
	struct reply_info rep;
	uint16_t udpsize;

	memset(&rep, 0, sizeof(rep));
	rep.flags = (uint16_t)((BIT_QR | BIT_AA | BIT_RA) | rcode);
	rep.qdcount = 1;
	rep.rrset_count = ansec;
	if(ansec > 0) {
		rep.an_numrrsets = 1;
		rep.rrsets = &rrset;
		rep.ttl = ((struct packed_rrset_data*)rrset->entry.data)->rr_ttl[0];
	}

	udpsize = edns->udp_size;
	edns->edns_version = EDNS_ADVERTISED_VERSION;
	edns->udp_size = EDNS_ADVERTISED_SIZE;
	edns->ext_rcode = 0;
	edns->bits &= EDNS_DO;
	//!inplace_cb_reply_local_call(env, qinfo, NULL, &rep, rcode, edns,repinfo, temp) ||
	if(!reply_info_answer_encode(qinfo, &rep,
		*(uint16_t*)sldns_buffer_begin(buf), sldns_buffer_read_u16_at(buf, 2),
		buf, 0, 0, temp, udpsize, edns, (int)(edns->bits&EDNS_DO), 0)) {
		error_encode(buf, (LDNS_RCODE_SERVFAIL|BIT_AA), qinfo,
			*(uint16_t*)sldns_buffer_begin(buf),
			sldns_buffer_read_u16_at(buf, 2), edns);
	}

	return 1;
}

static struct local_rrset*
rpz_find_synthesized_rrset(int qtype, struct clientip_synthesized_rr* data) {
	struct local_rrset* cursor = data->data;
	while( cursor != NULL) {
		struct packed_rrset_key* packed_rrset = &cursor->rrset->rk;
		if(htons(qtype) == packed_rrset->type) {
			return cursor;
		}
		cursor = cursor->next;
	}
	return NULL;
}

static void
rpz_apply_clientip_localdata_action(struct clientip_synthesized_rr* raddr,
	struct query_info* qinfo, struct edns_data* edns, sldns_buffer* buf,
	struct regional* temp)
{
	struct local_rrset* rrset;
	enum rpz_action action = RPZ_INVALID_ACTION;
	struct ub_packed_rrset_key* rp = NULL;
	int rcode = LDNS_RCODE_NOERROR|BIT_AA;
	int rrset_count = 1;

	verbose(VERB_ALGO, "rpz: apply client ip trigger: found=%d action=%s",
		raddr != NULL, rpz_action_to_string(action));

	/* prepare synthesized answer for client */

	action = raddr->action;
	if(action == RPZ_LOCAL_DATA_ACTION && raddr->data == NULL ) {
		verbose(VERB_ALGO, "rpz: bug: local-data action but no local data");
		return;
	}

	/* check query type / rr type */

	rrset = rpz_find_synthesized_rrset(qinfo->qtype, raddr);
	if(rrset == NULL) {
		verbose(VERB_ALGO, "rpz: unable to find local-data for query");
		rrset_count = 0;
		goto nodata;
	}

	rp = respip_copy_rrset(rrset->rrset, temp);
	if(!rp) {
		verbose(VERB_ALGO, "rpz: local data action: out of memory");
		return;
	}

	rp->rk.flags |= PACKED_RRSET_FIXEDTTL;
	rp->rk.dname = qinfo->qname;
	rp->rk.dname_len = qinfo->qname_len;
nodata:
	rpz_local_encode(qinfo, edns, buf, temp, rp, rrset_count, rcode);
}

static inline struct dns_msg*
rpz_dns_msg_new(struct regional* region)
{
	struct dns_msg* msg =
			(struct dns_msg*)regional_alloc(region,
							sizeof(struct dns_msg));
	if(msg == NULL) { return NULL; }
	memset(msg, 0, sizeof(struct dns_msg));

	return msg;
}

static inline struct dns_msg*
rpz_synthesize_nodata(struct rpz* ATTR_UNUSED(r), struct module_qstate* ms)
{
	struct dns_msg* msg = rpz_dns_msg_new(ms->region);
	if(msg == NULL) { return msg; }
	msg->qinfo = ms->qinfo;
	msg->rep = construct_reply_info_base(ms->region,
					     LDNS_RCODE_NOERROR | BIT_RD | BIT_QR | BIT_AA | BIT_RA,
					     1, //qd
					     0, //ttl
					     0, //prettl
					     0, //expttl
					     0, //an
					     0, //ns
					     0, //ar
					     0, //total
					     sec_status_secure);
	return msg;
}

static inline struct dns_msg*
rpz_synthesize_nxdomain(struct rpz* ATTR_UNUSED(r), struct module_qstate* ms)
{
	struct dns_msg* msg = rpz_dns_msg_new(ms->region);
	if(msg == NULL) { return msg; }
	msg->qinfo = ms->qinfo;
	msg->rep = construct_reply_info_base(ms->region,
					     LDNS_RCODE_NXDOMAIN | BIT_RD | BIT_QR | BIT_AA | BIT_RA,
					     1, //qd
					     0, //ttl
					     0, //prettl
					     0, //expttl
					     0, //an
					     0, //ns
					     0, //ar
					     0, //total
					     sec_status_secure);
	return msg;
}

static inline struct dns_msg*
rpz_synthesize_localdata_from_rrset(struct rpz* ATTR_UNUSED(r), struct module_qstate* ms,
	struct local_rrset* rrset)
{
	struct dns_msg* msg = NULL;
	struct query_info* qi = &ms->qinfo;
	struct reply_info* new_reply_info;
	struct ub_packed_rrset_key* rp;


	msg = rpz_dns_msg_new(ms->region);
	if(msg == NULL) { return NULL; }

	// XXX: use ttl etc from rpz zone?
        new_reply_info = construct_reply_info_base(ms->region,
                                                   LDNS_RCODE_NOERROR | BIT_RD | BIT_QR | BIT_AA | BIT_RA,
                                                   1, //qd
                                                   0, //ttl
                                                   0, //prettl
                                                   0, //expttl
                                                   1, //an
                                                   0, //ns
                                                   0, //ar
                                                   1, //total
                                                   sec_status_secure);
	if(new_reply_info == NULL) {
		log_err("out of memory");
		return NULL;
	}
	rp = respip_copy_rrset(rrset->rrset, ms->region);
	if(rp == NULL) {
		log_err("out of memory");
		return NULL;
	}
	rp->rk.dname = qi->qname;
	rp->rk.dname_len = qi->qname_len;
	new_reply_info->rrsets[0] = rp;
	msg->rep = new_reply_info;
	return msg;
}

static inline struct dns_msg*
rpz_synthesize_nsip_localdata(struct rpz* r, struct module_qstate* ms,
	struct clientip_synthesized_rr* data)
{
	struct query_info* qi = &ms->qinfo;
	struct local_rrset* rrset;

	rrset = rpz_find_synthesized_rrset(qi->qtype, data);
	if(rrset == NULL) {
		verbose(VERB_ALGO, "rpz: nsip: no matching local data found");
		return NULL;
	}

	return rpz_synthesize_localdata_from_rrset(r, ms, rrset);
}

// copy'n'paste from localzone.c
static struct local_rrset*
local_data_find_type(struct local_data* data, uint16_t type, int alias_ok)
{
	struct local_rrset* p;
	type = htons(type);
	for(p = data->rrsets; p; p = p->next) {
		verbose(VERB_ALGO, "type=%d (%d)", type, p->rrset->rk.type);
		if(p->rrset->rk.type == type)
			return p;
		if(alias_ok && p->rrset->rk.type == htons(LDNS_RR_TYPE_CNAME))
			return p;
	}
	return NULL;
}

// based on localzone.c:local_data_answer()
static inline struct dns_msg*
rpz_synthesize_nsdname_localdata(struct rpz* r, struct module_qstate* ms,
	struct local_zone* z, struct matched_delegation_point const* match)
{
	struct local_data key;
	struct local_data* ld;
	struct local_rrset* rrset;

	if(match->dname == NULL) { return NULL; }

	key.node.key = &key;
	key.name = match->dname;
	key.namelen = match->dname_len;
	key.namelabs = dname_count_labels(match->dname);

	rpz_log_dname("nsdname local data", key.name, key.namelen);

	ld = (struct local_data*)rbtree_search(&z->data, &key.node);
	if(ld == NULL) {
		verbose(VERB_ALGO, "rpz: nsdname: impossible: qname not found");
		return NULL;
	}

	rrset = local_data_find_type(ld, ms->qinfo.qtype, 1);
	if(rrset == NULL) {
		verbose(VERB_ALGO, "rpz: nsdname: no matching local data found");
		return NULL;
	}

	return rpz_synthesize_localdata_from_rrset(r, ms, rrset);
}

static int
rpz_synthesize_qname_localdata(struct module_env* env, struct rpz* r,
	struct local_zone* z, enum localzone_type lzt, struct query_info* qinfo,
	struct edns_data* edns, sldns_buffer* buf, struct regional* temp,
	struct comm_reply* repinfo, struct ub_server_stats* stats)
{
	struct local_data* ld = NULL;
	int ret = 0;
	if(r->action_override == RPZ_CNAME_OVERRIDE_ACTION) {
		qinfo->local_alias = regional_alloc_zero(temp, sizeof(struct local_rrset));
		if(qinfo->local_alias == NULL) {
			return 0; /* out of memory */
		}
		qinfo->local_alias->rrset = regional_alloc_init(temp, r->cname_override,
								sizeof(*r->cname_override));
		if(qinfo->local_alias->rrset == NULL) {
			return 0; /* out of memory */
		}
		qinfo->local_alias->rrset->rk.dname = qinfo->qname;
		qinfo->local_alias->rrset->rk.dname_len = qinfo->qname_len;
		if(r->log) {
			log_rpz_apply(z->name, RPZ_CNAME_OVERRIDE_ACTION,
				      qinfo, repinfo, r->log_name);
		}
		stats->rpz_action[RPZ_CNAME_OVERRIDE_ACTION]++;
		return 0;
	}

	if(lzt == local_zone_redirect && local_data_answer(z, env, qinfo,
		edns, repinfo, buf, temp, dname_count_labels(qinfo->qname),
		&ld, lzt, -1, NULL, 0, NULL, 0)) {
		if(r->log) {
			log_rpz_apply(z->name,
				localzone_type_to_rpz_action(lzt), qinfo,
				repinfo, r->log_name);
		}
		stats->rpz_action[localzone_type_to_rpz_action(lzt)]++;
		return !qinfo->local_alias;
	}

	ret = local_zones_zone_answer(z, env, qinfo, edns, repinfo, buf, temp,
		0 /* no local data used */, lzt);
	if(r->log) {
		log_rpz_apply(z->name, localzone_type_to_rpz_action(lzt),
			      qinfo, repinfo, r->log_name);
	}
	stats->rpz_action[localzone_type_to_rpz_action(lzt)]++;
	return ret;
}

struct clientip_synthesized_rr*
rpz_delegation_point_ipbased_trigger_lookup(struct rpz* rpz, struct iter_qstate* is)
{
	struct delegpt_addr* cursor;
	struct clientip_synthesized_rr* action = NULL;
	if(is->dp == NULL) { return NULL; }
	for(cursor = is->dp->target_list;
	    cursor != NULL;
	    cursor = cursor->next_target) {
		if(cursor->bogus) { continue; }
		action = rpz_ipbased_trigger_lookup(rpz->ns_set, &cursor->addr,
						    cursor->addrlen);
		if(action != NULL) { return action; }
	}
	return NULL;
}

struct dns_msg*
rpz_apply_nsip_trigger(struct module_qstate* ms, struct rpz* r,
	struct clientip_synthesized_rr* raddr)
{
	enum rpz_action action = raddr->action;
	struct dns_msg* ret = NULL;

	if(r->action_override != RPZ_NO_OVERRIDE_ACTION) {
		verbose(VERB_ALGO, "rpz: using override action=%s (replaces=%s)",
			rpz_action_to_string(r->action_override), rpz_action_to_string(action));
		action = r->action_override;
	}

	verbose(VERB_ALGO, "rpz: iterator callback: nsip: apply action=%s",
		rpz_action_to_string(raddr->action));

	if(action == RPZ_LOCAL_DATA_ACTION && raddr->data == NULL) {
		verbose(VERB_ALGO, "rpz: bug: nsip local data action but no local data");
		ret = rpz_synthesize_nodata(r, ms);
		goto done;
	}

	switch(action) {
	case RPZ_NXDOMAIN_ACTION:
		ret = rpz_synthesize_nxdomain(r, ms);
		break;
	case RPZ_NODATA_ACTION:
		ret = rpz_synthesize_nodata(r, ms);
		break;
	case RPZ_TCP_ONLY_ACTION:
		// basically a passthru here but the tcp-only will be
		// honored before the query gets send
		ms->respip_action_info->action = respip_truncate;
		ret = NULL;
		break;
	case RPZ_DROP_ACTION:
		ret = rpz_synthesize_nodata(r, ms);
		ms->is_drop = 1;
		break;
	case RPZ_LOCAL_DATA_ACTION:
		ret = rpz_synthesize_nsip_localdata(r, ms, raddr);
		if(ret == NULL) { ret = rpz_synthesize_nodata(r, ms); }
		break;
	case RPZ_PASSTHRU_ACTION:
		ret = NULL;
		break;
	default:
		verbose(VERB_ALGO, "rpz: nsip: bug: unhandled or invalid action: '%s'",
			rpz_action_to_string(action));
		ret = NULL;
	}

done:
	lock_rw_unlock(&raddr->lock);
	return ret;
}

struct dns_msg*
rpz_apply_nsdname_trigger(struct module_qstate* ms, struct rpz* r,
	struct local_zone* z, struct matched_delegation_point const* match)
{
	struct dns_msg* ret = NULL;
	enum rpz_action action = localzone_type_to_rpz_action(z->type);

	if(r->action_override != RPZ_NO_OVERRIDE_ACTION) {
		verbose(VERB_ALGO, "rpz: using override action=%s (replaces=%s)",
			rpz_action_to_string(r->action_override), rpz_action_to_string(action));
		action = r->action_override;
	}

	verbose(VERB_ALGO, "rpz: nsdame trigger with action=%s", rpz_action_to_string(action));

	switch(action) {
	case RPZ_NXDOMAIN_ACTION:
		ret = rpz_synthesize_nxdomain(r, ms);
		break;
	case RPZ_NODATA_ACTION:
		ret = rpz_synthesize_nodata(r, ms);
		break;
	case RPZ_TCP_ONLY_ACTION:
		// basically a passthru here but the tcp-only will be
		// honored before the query gets send
		ms->respip_action_info->action = respip_truncate;
		ret = NULL;
		break;
	case RPZ_DROP_ACTION:
		ret = rpz_synthesize_nodata(r, ms);
		ms->is_drop = 1;
		break;
	case RPZ_LOCAL_DATA_ACTION:
		ret = rpz_synthesize_nsdname_localdata(r, ms, z, match);
		if(ret == NULL) { ret = rpz_synthesize_nodata(r, ms); }
		break;
	case RPZ_PASSTHRU_ACTION:
		ret = NULL;
		break;
	default:
		verbose(VERB_ALGO, "rpz: nsip: bug: unhandled or invalid action: '%s'",
			rpz_action_to_string(action));
		ret = NULL;
	}

	return ret;
}

static struct local_zone*
rpz_delegation_point_zone_lookup(struct delegpt* dp, struct local_zones* zones,
	uint16_t qclass,
	/* output parameter */
	struct matched_delegation_point* match)
{
	struct delegpt_ns* nameserver;
	struct local_zone* z = NULL;

	rpz_log_dname("delegation point", dp->name, dp->namelen);
	// XXX: do we want this?
	z = rpz_find_zone(zones, dp->name, dp->namelen, qclass, 0, 0, 0);
	if(z != NULL) {
		match->dname = dp->name;
		match->dname_len = dp->namelen;
	} else if(z == NULL) {
		for(nameserver = dp->nslist;
		    nameserver != NULL;
		    nameserver = nameserver->next) {
			rpz_log_dname("delegation point", nameserver->name, nameserver->namelen);
			z = rpz_find_zone(zones, nameserver->name, nameserver->namelen,
					  qclass, 0, 0, 0);
			if(z != NULL) {
				match->dname = nameserver->name;
				match->dname_len = nameserver->namelen;
				break;
			}
		}
	}

	verbose(VERB_ALGO, "rpz: delegation point zone found=%d", z != NULL);

	return z;
}

struct dns_msg*
rpz_callback_from_iterator_module(struct module_qstate* ms, struct iter_qstate* is)
{
	struct auth_zones* az;
	struct auth_zone* a;
	struct clientip_synthesized_rr* raddr = NULL;
	struct rpz* r;
	struct local_zone* z = NULL;
	struct matched_delegation_point match = {0};

	if(ms->env == NULL || ms->env->auth_zones == NULL) { return 0; }

	az = ms->env->auth_zones;

	verbose(VERB_ALGO, "rpz: iterator module callback: have_rpz=%d", az->rpz_first != NULL);

	lock_rw_rdlock(&az->rpz_lock);

	for(a = az->rpz_first; a != NULL; a = a->rpz_az_next) {
		lock_rw_rdlock(&a->lock);
		r = a->rpz;

		// XXX: check rfc which action has preference

		raddr = rpz_delegation_point_ipbased_trigger_lookup(r, is);
		if(raddr != NULL) {
			lock_rw_unlock(&a->lock);
			break;
		}

		z = rpz_delegation_point_zone_lookup(is->dp, r->nsdname_zones,
						     ms->qinfo.qclass, &match);
		if(z != NULL) {
			lock_rw_unlock(&a->lock);
			break;
		}
		lock_rw_unlock(&a->lock);
	}

	lock_rw_unlock(&az->rpz_lock);

	if(raddr == NULL && z == NULL) { return NULL; }
	else if(raddr != NULL) { return rpz_apply_nsip_trigger(ms, r, raddr); }
	else if(z != NULL) { return rpz_apply_nsdname_trigger(ms, r, z, &match); }
	else { return NULL; }
}

static int
rpz_apply_maybe_clientip_trigger(struct auth_zones* az, struct module_env* env,
	struct query_info* qinfo, struct edns_data* edns, struct comm_reply* repinfo,
	uint8_t* taglist, size_t taglen, struct ub_server_stats* stats,
	sldns_buffer* buf, struct regional* temp,
	/* output parameters */
	struct local_zone** z_out, struct auth_zone** a_out, struct rpz** r_out)
{
	int ret = 0;
	enum rpz_action client_action;
	struct clientip_synthesized_rr* node = rpz_resolve_client_action_and_zone(
		az, qinfo, repinfo, taglist, taglen, stats, z_out, a_out, r_out);

	client_action = node == NULL ? RPZ_INVALID_ACTION : node->action;

	verbose(VERB_ALGO, "rpz: qname trigger: client action=%s",
		rpz_action_to_string(client_action));

	if(*z_out == NULL || (client_action != RPZ_INVALID_ACTION &&
			      client_action != RPZ_PASSTHRU_ACTION)) {
		verbose(VERB_ALGO, "rpz: client action without zone");
		if(client_action == RPZ_PASSTHRU_ACTION
			|| client_action == RPZ_INVALID_ACTION
			|| (client_action == RPZ_TCP_ONLY_ACTION
				&& !rpz_is_udp_query(repinfo))) {
			ret = 0;
			goto done;
		}
		stats->rpz_action[client_action]++;
		if(client_action == RPZ_LOCAL_DATA_ACTION) {
			rpz_apply_clientip_localdata_action(node, qinfo, edns,
							    buf, temp);
		} else {
			// XXX: log_rpz_apply not possbile because no zone
			local_zones_zone_answer(NULL /*no zone*/, env, qinfo, edns,
				repinfo, buf, temp, 0 /* no local data used */,
				rpz_action_to_localzone_type(client_action));
		}
		ret = 1;
		goto done;
	}
	ret = -1;
done:
	if(node != NULL) {
		lock_rw_unlock(&node->lock);
	}
	return ret;
}

int
rpz_callback_from_worker_request(struct auth_zones* az, struct module_env* env,
	struct query_info* qinfo, struct edns_data* edns, sldns_buffer* buf,
	struct regional* temp, struct comm_reply* repinfo, uint8_t* taglist,
	size_t taglen, struct ub_server_stats* stats)
{
	struct rpz* r = NULL;
	struct auth_zone* a = NULL;
	struct local_zone* z = NULL;
	int ret;
	enum localzone_type lzt;

	int clientip_trigger = rpz_apply_maybe_clientip_trigger(az, env, qinfo,
		edns, repinfo, taglist, taglen, stats, buf, temp, &z, &a, &r);
	if(clientip_trigger >= 0) { return clientip_trigger; }

	if(z == NULL) {
		return 0;
	}

	log_assert(r);

	if(r->action_override == RPZ_NO_OVERRIDE_ACTION) {
		lzt = z->type;
	} else {
		lzt = rpz_action_to_localzone_type(r->action_override);
	}

	verbose(VERB_ALGO, "rpz: qname trigger with action=%s",
		rpz_action_to_string(localzone_type_to_rpz_action(lzt)));

	ret = rpz_synthesize_qname_localdata(env, r, z, lzt, qinfo, edns, buf, temp,
					     repinfo, stats);

	lock_rw_unlock(&z->lock);
	lock_rw_unlock(&a->lock);

	return ret;
}
