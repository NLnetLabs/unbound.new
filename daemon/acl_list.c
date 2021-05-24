/*
 * daemon/acl_list.h - client access control storage for the server.
 *
 * Copyright (c) 2007, NLnet Labs. All rights reserved.
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
 * This file helps the server keep out queries from outside sources, that
 * should not be answered.
 */
#include "config.h"
#include "daemon/acl_list.h"
#include "util/regional.h"
#include "util/log.h"
#include "util/config_file.h"
#include "util/net_help.h"
#include "services/localzone.h"
#include "sldns/str2wire.h"

struct acl_list* 
acl_list_create(void)
{
	struct acl_list* acl;

	// The acl_list structure persists across reloads, but the region
	// doesn't, so allocate the acl_list outside the regional allocator

	if ((acl = calloc(1, sizeof(*acl))) != NULL) {
		struct regional *rp;

		if ((rp = regional_create()) != NULL) {
			acl->region = rp;
			return (acl);
		}

		free(acl);
	}

	return (NULL);
}

void 
acl_list_delete(struct acl_list* acl)
{
	if (acl != NULL)  {
		regional_destroy(acl->region);
		free(acl);
	}
}

/** find or create node (NULL on parse or error) */
static struct acl_addr*
acl_find_or_create(struct acl_list* acl,
                   const char* netblock,
                   int *duplicate)
{
	struct sockaddr_storage addr;
	socklen_t addrlen;
	int net;

	if (!netblockstrtoaddr(netblock,
	                      UNBOUND_DNS_PORT,
	                      &addr, &addrlen, &net)) {
		log_err("cannot parse netblock: %s", netblock);
		return NULL;
	}

	struct acl_addr* node;

	// First, look to see if there's a duplicate node

	node = (struct acl_addr*) addr_tree_find(&acl->tree,
	                                         &addr, addrlen, net);

	if (node != NULL) {
		*duplicate = 1;
	} else if ((node = regional_alloc_zero(acl->region,
	                                       sizeof(*node))) != NULL) {
		*duplicate = 0;

		// Set the default mode to allow (for views, tags, etc).

		node->control = acl_allow;
		addr_tree_insert(&acl->tree, &node->node, &addr, addrlen, net);
	} else {
		log_err("out of memory");
	}

	return node;
}

/** apply acl_list string */
static struct acl_addr *
acl_list_str_mode(struct acl_list* acl,
                  const char* netblock,
                  enum acl_access control,
                  int complain_duplicates)
{
	struct acl_addr* node;
	int dup;

	if ((node = acl_find_or_create(acl, netblock, &dup)) != NULL) {
		if (!dup) {
			node->control = control;
		} else if (complain_duplicates) {
			verbose(VERB_QUERY, "duplicate acl for %s ignored.", netblock);
		}
	}

	return node;
}

/** apply acl_list string */
static struct acl_addr *
acl_list_str_cfg(struct acl_list* acl,
                 const char* netblock,
                 const char* mode,
                 int complain_duplicates)
{
	enum acl_access control;

	if (strcmp(mode, "allow") == 0)
		control = acl_allow;
	else if (strcmp(mode, "deny") == 0)
		control = acl_deny;
	else if (strcmp(mode, "refuse") == 0)
		control = acl_refuse;
	else if (strcmp(mode, "deny_non_local") == 0)
		control = acl_deny_non_local;
	else if (strcmp(mode, "refuse_non_local") == 0)
		control = acl_refuse_non_local;
	else if (strcmp(mode, "allow_snoop") == 0)
		control = acl_allow_snoop;
	else if (strcmp(mode, "allow_setrd") == 0)
		control = acl_allow_setrd;
	else {
		log_err("access control type %s unknown", mode);
		return NULL;
	}

	return acl_list_str_mode(acl, netblock, control, complain_duplicates);
}

/** apply acl_tag string */
static int
acl_list_tags_cfg(struct acl_list* acl,
                  const char* netblock,
                  uint8_t* bitmap,
                  size_t bitmaplen)
{
	struct acl_addr* node;
	int dup;

	if ((node = acl_find_or_create(acl, netblock, &dup)) == NULL) {
		return 0;
	}

	uint8_t *taglist;

	if (dup && node->taglist != NULL) {
		verbose(VERB_QUERY, "duplicate tags for %s ignored.", netblock);
	} else if ((taglist = regional_alloc_init(acl->region,
	                                          bitmap,
	                                          bitmaplen)) != NULL) {
		node->taglist = taglist;
		node->taglen  = bitmaplen;
	} else {
		log_err("out of memory");
		return 0;
	}

	return 1;
}

/** apply acl_view string */
static int
acl_list_view_cfg(struct acl_list* acl,
                  const char* netblock,
                  const char* viewname,
                  struct views* vs)
{
	struct view* v;

	if ((v = views_find_view(vs, viewname, 0)) == NULL) {
		log_err("no view with name: %s", viewname);
		return 0;
	}

	struct acl_addr* node;
	int dup;

	if ((node = acl_find_or_create(acl, netblock, &dup)) == NULL) {
		return 0;
	}
	if (dup && node->view != NULL) {
		verbose(VERB_QUERY, "duplicate view %s for %s ignored.",
		                    viewname,
		                    netblock);
	} else {
		node->view = v;
	}

	lock_rw_unlock(&node->view->lock);
	return 1;
}

/** apply acl_tag_action string */
static int
acl_list_tag_action_cfg(struct acl_list* acl,
                        struct config_file* cfg,
                        const char* netblock,
                        const char* tag,
                        const char* action)
{
	enum localzone_type t;
	int tagid;

	// Validate the tag parameters

	if ((tagid=find_tag_id(cfg, tag)) == -1) {
		log_err("cannot parse tag (define-tag it): %s %s", netblock, tag);
		return 0;
	}
	if (tagid >= cfg->num_tags) {
		log_err("tagid too large for array %s %s", netblock, tag);
		return 0;
	}
	if (!local_zone_str2type(action, &t)) {
		log_err("cannot parse access control action type: %s %s %s",
		        netblock,
		        tag,
		        action);
		return 0;
	}

	struct acl_addr* node;
	int dup;

	if ((node = acl_find_or_create(acl, netblock, &dup)) == NULL) {
		return 0;
	}

	uint8_t *actions;

	if ((actions = node->tag_actions) == NULL) {
		// Allocate a new array for the actions

		actions = regional_alloc_zero(acl->region,
		                              sizeof(*actions) * cfg->num_tags);

		if (actions == NULL) {
			log_err("out of memory");
			return 0;
		}

		node->tag_actions = actions;
		node->tag_actions_size = (size_t) cfg->num_tags;
	}

	actions[tagid] = (uint8_t) t;

	return 1;
}

/** check wire data parse */
static int
check_data(const char* data, const struct config_strlist* head)
{
	char buf[65536];
	uint8_t rr[LDNS_RR_BUF_SIZE];
	size_t len = sizeof(rr);
	int res;
	/* '.' is sufficient for validation, and it makes the call to
	 * sldns_wirerr_get_type() simpler below. */
	snprintf(buf, sizeof(buf), ". %s", data);
	len = sizeof(rr);
	res = sldns_str2wire_rr_buf(buf,
	                            rr, &len, NULL,
	                            3600,
	                            NULL, 0,
	                            NULL, 0);

	/* Reject it if we would end up having CNAME and other data (including
	 * another CNAME) for the same tag. */
	if (res == 0 && head) {
		const char* err_data = NULL;

		if (sldns_wirerr_get_type(rr, len, 1) == LDNS_RR_TYPE_CNAME) {
			/* adding CNAME while other data already exists. */
			err_data = data;
		} else {
			snprintf(buf, sizeof(buf), ". %s", head->str);
			len = sizeof(rr);
			res = sldns_str2wire_rr_buf(buf,
	                                    rr, &len, NULL,
	                                    3600,
	                                    NULL, 0,
	                                    NULL, 0);
			if (res != 0) {
				/* This should be impossible here as head->str
				 * has been validated, but we check it just in
				 * case. */
				return 0;
			}
			if (sldns_wirerr_get_type(rr, len, 1) ==
				LDNS_RR_TYPE_CNAME) /* already have CNAME */
				err_data = head->str;
		}
		if (err_data) {
			log_err("redirect tag data '%s' must not coexist with "
				"other data.", err_data);
			return 0;
		}
	}
	if (res == 0)
		return 1;
	log_err("rr data [char %d] parse error %s",
	        (int)LDNS_WIREPARSE_OFFSET(res)-2,
	        sldns_get_errorstr_parse(res));
	return 0;
}

/** apply acl_tag_data string */
static int
acl_list_tag_data_cfg(struct acl_list* acl,
                      struct config_file* cfg,
                      const char* netblock,
                      const char* tag,
                      const char* data)
{
	int tagid;

	// Validate the data first

	if ((tagid=find_tag_id(cfg, tag)) == -1) {
		log_err("cannot parse tag (define-tag it): %s %s", netblock, tag);
		return 0;
	}
	if (tagid >= cfg->num_tags) {
		log_err("tagid too large for array %s %s", netblock, tag);
		return 0;
	}

	struct acl_addr* node;
	int dup;

	if ((node = acl_find_or_create(acl, netblock, &dup)) == NULL) {
		return 0;
	}

	struct config_strlist **tag_datas;

	if ((tag_datas = node->tag_datas) == NULL) {
		// No tags_data array allocated yet - do so now. If this fails, we
		// we abort the start-up process.

		if ((tag_datas = regional_alloc_zero(acl->region,
		                                     sizeof(*tag_datas) *
		                                         cfg->num_tags)) == NULL) {
			log_err("out of memory");
			return 0;
		}

		node->tag_datas      = tag_datas;
		node->tag_datas_size = (size_t) cfg->num_tags;
	}

	if (!check_data(data, tag_datas[tagid])) {
		log_err("cannot parse access-control-tag data: %s %s '%s'",
		        netblock,
		        tag,
		        data);
		return 0;
	}

	char* dupdata;

	if ((dupdata = regional_strdup(acl->region, data)) == NULL) {
		log_err("out of memory");
		return 0;
	}

	if (!cfg_region_strlist_insert(acl->region,
	                               &tag_datas[tagid],
	                               dupdata)) {
		log_err("out of memory");
		return 0;
	}

	return 1;
}

/** read acl_list config */
static int 
read_acl_list(struct acl_list* acl, struct config_file* cfg)
{
	struct config_str2list* p;

	for (p = cfg->acls; p != NULL; p = p->next) {
		log_assert(p->str && p->str2);

		struct acl_addr *node;

		if ((node = acl_list_str_cfg(acl, p->str, p->str2, 1)) == NULL) {
			return 0;
		}
	}

	return 1;
}

/** read acl tags config */
static int 
read_acl_tags(struct acl_list* acl, struct config_file* cfg)
{
	struct config_strbytelist* np, *p = cfg->acl_tags;
	cfg->acl_tags = NULL;
	while (p) {
		log_assert(p->str && p->str2);
		if (!acl_list_tags_cfg(acl, p->str, p->str2, p->str2len)) {
			config_del_strbytelist(p);
			return 0;
		}
		/* free the items as we go to free up memory */
		np = p->next;
		free(p->str);
		free(p->str2);
		free(p);
		p = np;
	}
	return 1;
}

/** read acl view config */
static int 
read_acl_view(struct acl_list* acl,
              struct config_file* cfg,
              struct views* vs)
{
	// Process the defined ACL views - these will be lower priority than
	// views specified as match-clients. Free any successful entries.

	struct config_str2list* np, *p;

	for (p = cfg->acl_view; p != NULL; p = np) {
		log_assert(p->str && p->str2);

		if (!acl_list_view_cfg(acl, p->str, p->str2, vs)) {
			return 0;
		}
		/* free the items as we go to free up memory */
		free(p->str);
		free(p->str2);
		np = p->next;
		free(p);
		cfg->acl_view = np;
	}

	// For each defined view, add the match clients. The views are in the
	// reverse order they are defined, as are the match-clients. We don't
	// overwrite a previously assigned view, so the last definition takes
	// precedence.
	//
	// As above, free up the match-clients entries, but don't free the view
	// as we'll need the configuration information.

	struct config_view *vcfg;

	for (vcfg = cfg->views; vcfg != NULL; vcfg = vcfg->next) {
		struct config_strlist *sp, *nsp;

		for (sp = vcfg->match_clients; sp != NULL; sp = nsp) {
			log_assert(sp->str);

			if (!acl_list_view_cfg(acl, sp->str, vcfg->name, vs)) {
				// This should never happen because the configured view
				// had to have been successfully added to the tree

				return 0;
			}

			free(sp->str);
			nsp = sp->next;
			free(sp);
			vcfg->match_clients = nsp;
		}
	}

	return 1;
}

/** read acl tag actions config */
static int 
read_acl_tag_actions(struct acl_list* acl, struct config_file* cfg)
{
	struct config_str3list* p, *np;
	p = cfg->acl_tag_actions;
	cfg->acl_tag_actions = NULL;
	while (p) {
		log_assert(p->str && p->str2 && p->str3);
		if (!acl_list_tag_action_cfg(acl,
		                             cfg,
		                             p->str,
		                             p->str2,
		                             p->str3)) {
			config_deltrplstrlist(p);
			return 0;
		}
		/* free the items as we go to free up memory */
		np = p->next;
		free(p->str);
		free(p->str2);
		free(p->str3);
		free(p);
		p = np;
	}
	return 1;
}

/** read acl tag datas config */
static int 
read_acl_tag_datas(struct acl_list* acl, struct config_file* cfg)
{
	struct config_str3list* p, *np;
	p = cfg->acl_tag_datas;
	cfg->acl_tag_datas = NULL;
	while (p) {
		log_assert(p->str && p->str2 && p->str3);
		if (!acl_list_tag_data_cfg(acl, cfg, p->str, p->str2, p->str3)) {
			config_deltrplstrlist(p);
			return 0;
		}
		/* free the items as we go to free up memory */
		np = p->next;
		free(p->str);
		free(p->str2);
		free(p->str3);
		free(p);
		p = np;
	}
	return 1;
}

static void
set_acl_view(rbnode_type* n, void* arg)
{
	struct acl_addr* node = (struct acl_addr *) n;

	// Set the default view for any unassociated ACL addresses

	if (node->view == NULL) {
		node->view = arg;
	}
}

int 
acl_list_apply_cfg(struct acl_list* acl,
                   struct config_file* cfg,
                   struct views* vs)
{
	regional_free_all(acl->region);
	addr_tree_init(&acl->tree);
	if (!read_acl_list(acl, cfg))
		return 0;
	if (!read_acl_view(acl, cfg, vs))
		return 0;
	if (!read_acl_tags(acl, cfg))
		return 0;
	if (!read_acl_tag_actions(acl, cfg))
		return 0;
	if (!read_acl_tag_datas(acl, cfg))
		return 0;
	/* insert defaults, with '0' to ignore them if they are duplicates */
	if (acl_list_str_mode(acl, "0.0.0.0/0", acl_refuse, 0) == NULL) {
		return 0;
	}
	if (acl_list_str_mode(acl, "127.0.0.0/8", acl_allow, 0) == NULL) {
		return 0;
	}
	if (cfg->do_ip6) {
		if (acl_list_str_mode(acl, "::0/0", acl_refuse, 0) == NULL) {
			return 0;
		}
		if (acl_list_str_mode(acl, "::1", acl_allow, 0) == NULL) {
			return 0;
		}
		if (acl_list_str_mode(acl, "::ffff:127.0.0.1", acl_allow, 0) == NULL) {
			return 0;
		}
	}

	traverse_postorder(&acl->tree, set_acl_view, &vs->server_view);
	addr_tree_init_parents(&acl->tree);
	return 1;
}

enum acl_access 
acl_get_control(struct acl_addr* acl)
{
	return (acl != NULL) ? acl->control : acl_deny;
}

struct acl_addr*
acl_addr_lookup(struct acl_list* acl, struct sockaddr_storage* addr,
        socklen_t addrlen)
{
	return (struct acl_addr*)addr_tree_lookup(&acl->tree,
	                                          addr,
	                                          addrlen);
}

size_t 
acl_list_get_mem(struct acl_list* acl)
{
	size_t sz = 0;

	if (acl != NULL) {
		sz = sizeof(*acl) + regional_get_mem(acl->region);
	}

	return (sz);
}
