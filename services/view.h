/*
 * services/view.h - named views containing local zones authority service.
 *
 * Copyright (c) 2016, NLnet Labs. All rights reserved.
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
 * This file contains functions to enable named views that can hold local zone
 * authority service.
 */

#ifndef SERVICES_VIEW_H
#define SERVICES_VIEW_H
#include "util/rbtree.h"
#include "util/locks.h"
struct regional;
struct config_file;
struct config_view;
struct respip_set;
struct daemon;
struct alloc_cache;

/**
 * View statistics - one allocated per thread to avoid locking
 */
struct view_stats {
	/** number of queries from clients received. */
	long long num_queries;
	/** number of queries from clients received. */
	long long num_queries_ip_ratelimited;
	/** number of queries that had a cache-miss. */
	long long num_queries_missed_cache;
	/** number of prefetch queries - cachehits with prefetch */
	long long num_queries_prefetch;
	/** expired answers served from cache */
	long long ans_expired;
};

/**
 * View enviroment - encapsulates view-specific values
 */
struct view {
	/** view name. */
	char* name;
	/** view-specific configuration information */
	struct config_file *view_cfg;
	/** Server view - if the same as the current view, this is the server */
	struct view* server_view;
	/** Local zones and response IP sets are view-aware and treat
	 ** them differently.
	 */
	/** view specific local authority zones */
	struct local_zones* local_zones;
	/** server zones, if fallback is configured */
	struct local_zones* server_zones;
	/** response-ip configuration data for this view */
	struct respip_set* respip_set;
	/** server response-ip configuration data, if fallback is configured */
	struct respip_set* server_set;
	/** Caches, stubs, forwards, etc. inherit from the server if not
	 ** defined in the view. For clean-up, we have to mark whether or
	 ** not to free them
     */
	/** shared message cache */
	struct slabhash* msg_cache;
	/** shared rrset cache */
	struct rrset_cache* rrset_cache;
	/** stub/root zone hints */
	struct iter_hints* hints;
	/** forward zones hints */
	struct iter_forwards* fwds;
	/** Ownership flags */
	unsigned int view_flags;
	/** Number of stats structures */
	unsigned int view_threads;
	/** Per-thread view stats - indexed by thread number */
	struct view_stats *view_stats;
	/** lock on the data in the structure
	 * For the node and name you need to also hold the views_tree lock to
	 * change them. */
	lock_rw_type lock;
};

#define	VIEW_FLAG_SHARE_CACHE  0x01    // Caches are shared with the server
#define	VIEW_FLAG_SHARE_HINTS  0x02    // Hints are shared with the server
#define	VIEW_FLAG_SHARE_FWDS   0x04    // Forwards are shared with the server

/**
 * Views storage, shared.
 */
struct views {
	/** default view */
	struct view server_view;
	/** lock on the view tree */
	lock_rw_type lock;
	/** rbtree of struct view */
	rbtree_type vtree;
};

/**
 * View. Named structure holding local authority zones.
 */
struct view_node {
	/** rbtree node, key is name */
	rbnode_type node;
	/** view information */
	struct view vinfo;
};


/**
 * Create views storage
 * @return new struct or NULL on error.
 */
struct views* views_create(void);

/**
 * Delete views storage
 * @param v: views to delete.
 */
void views_delete(struct views* v);

/**
 * Apply config settings to a daemon context
 * @param daemon: the unbound daemon
 * @return false on error.
 */
int daemon_views(struct daemon *daemon);

/**
 * Apply config settings;
 * Takes care of locking.
 * @param v: view is set up.
 * @param cfg: config data.
 * @return false on error.
 */
int views_apply_cfg(struct views* v, struct config_file* cfg);

/**
 * Compare two view entries in rbtree. Sort canonical.
 * @param v1: view 1
 * @param v2: view 2
 * @return: negative, positive or 0 comparison value.
 */
int view_cmp(const void* v1, const void* v2);

/* Find a view by name.
 * @param vs: views
 * @param name: name of the view we are looking for
 * @param write: 1 for obtaining write lock on found view, 0 for read lock
 * @return: locked view or NULL. 
 */
struct view* views_find_view(struct views* vs, const char* name, int write);
int
views_configure(struct views *vs,
		struct config_file *cfg,
		struct alloc_cache *alloc);
#endif /* SERVICES_VIEW_H */
