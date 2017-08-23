/* BGP RPKI
 * Copyright (C) 2013 Michael Mester (m.mester@fu-berlin.de)
 *
 * This file is part of Quagga
 *
 * Quagga is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2, or (at your option) any
 * later version.
 *
 * Quagga is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Quagga; see the file COPYING.  If not, write to the Free
 * Software Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
 * 02111-1307, USA.
 */

#include <zebra.h>
#include <pthread.h>
#include <time.h>
#include <stdbool.h>
#include <stdlib.h>
#include "prefix.h"
#include "log.h"
#include "command.h"
#include "linklist.h"
#include "memory.h"
#include "thread.h"
#include "filter.h"
#include "bgpd/bgpd.h"
#include "bgpd/bgp_table.h"
#include "bgp_advertise.h"
#include "bgpd/bgp_debug.h"
#include "bgpd/bgp_attr.h"
#include "bgpd/bgp_aspath.h"
#include "bgpd/bgp_rpki.h"
#include "bgpd/bgp_rpki_commands.h"
#include "rtrlib/rtrlib.h"
#include "rtrlib/rtr_mgr.h"
#include "rtrlib/lib/ip.h"
#include "rtrlib/transport/tcp/tcp_transport.h"
#if defined(FOUND_SSH)
#include "rtrlib/transport/ssh/ssh_transport.h"
#endif
#include "hook.h"
#include "libfrr.h"
#include "version.h"

#define RPKI_OUTPUT_STRING "Control rpki specific settings\n"

#include "bgp_rpki_clippy.c"

DEFINE_MTYPE_STATIC(BGPD, BGP_RPKI_CACHE, "BGP RPKI Cache server")
DEFINE_MTYPE_STATIC(BGPD, BGP_RPKI_CACHE_GROUP, "BGP RPKI Cache server group")

/**********************************/
/** Declaration of variables     **/
/**********************************/
struct rtr_mgr_config *rtr_config;
struct list *cache_list;
int rtr_is_running;
int route_map_active;

/**********************************/
/** Declaration of structs       **/
/**********************************/
enum return_values { SUCCESS = 0, ERROR = -1 };

struct cache {
       enum { TCP, SSH } type;
       struct tr_socket *tr_socket;
       union {
               struct tr_tcp_config *tcp_config;
               struct tr_ssh_config *ssh_config;
       } tr_config;
       struct rtr_socket *rtr_socket;
       uint8_t preference;
};

struct rpki_for_each_record_arg {
	struct vty *vty;
	unsigned int *prefix_amount;
};

static void *rpki_malloc_wrapper(size_t size)
{
	return XMALLOC(MTYPE_BGP_RPKI_CACHE, size);
}

static void *rpki_realloc_wrapper(void *ptr, size_t size)
{
	return XREALLOC(MTYPE_BGP_RPKI_CACHE, ptr, size);
}

static void rpki_free_wrapper(void *ptr)
{
	XFREE(MTYPE_BGP_RPKI_CACHE, ptr);
}


static route_map_result_t route_match_rpki(void *rule, struct prefix *prefix,
					   route_map_object_t type,
					   void *object)
{
	int *rpki_status = rule;
	struct bgp_info *bgp_info;

	if (type == RMAP_BGP) {
		bgp_info = object;

		if (rpki_validate_prefix(bgp_info->peer, bgp_info->attr, prefix)
		    == *rpki_status) {
			return RMAP_MATCH;
		}
	}
	return RMAP_NOMATCH;
}

static void *route_match_rpki_compile(const char *arg)
{
	int *rpki_status;

	rpki_status = XMALLOC(MTYPE_ROUTE_MAP_COMPILED, sizeof(u_char));

	if (strcmp(arg, "valid") == 0)
		*rpki_status = RPKI_VALID;
	else if (strcmp(arg, "invalid") == 0)
		*rpki_status = RPKI_INVALID;
	else
		*rpki_status = RPKI_NOTFOUND;

	return rpki_status;
}

static void route_match_rpki_free(void *rule)
{
	rpki_set_route_map_active(0);
	XFREE(MTYPE_ROUTE_MAP_COMPILED, rule);
}

static struct cmd_node rpki_node = {RPKI_NODE, "%s(config-rpki)# ", 1};
struct route_map_rule_cmd route_match_rpki_cmd = {"rpki", route_match_rpki,
						  route_match_rpki_compile,
						  route_match_rpki_free};
/*******************************************/
/*******************************************/
void rpki_start(void);
extern void bgp_process(struct bgp *bgp, struct bgp_node *rn, afi_t afi,
			safi_t safi);
static void delete_cache(void *value);
//static void delete_cache_group(void *_cache_group);
void install_cli_commands(void);
static int rpki_config_write(struct vty *vty);
static void overwrite_exit_commands(void);
static void bgp_rpki_free_cache(struct cache *cache);
struct rtr_mgr_group *get_rtr_mgr_groups(void);
unsigned int get_number_of_cache_groups(void);
//void delete_cache_group_list(void);
static int add_ssh_cache(const char *host,
			 const unsigned int port,
			 const char *username,
			 const char *client_privkey_path,
			 const char *client_pubkey_path,
			 const char *server_pubkey_path,
			 const uint8_t preference);
//static struct list *create_cache_list(void);
static struct rtr_socket *create_rtr_socket(struct tr_socket *tr_socket);
static struct cache *find_cache(const char *host, const char *port_string);
static int add_tcp_cache(const char *host,
			 const char *port,
			 const uint8_t preference);
/*****************************************/
/*****************************************/
//static void list_all_nodes(struct vty *vty, const struct trie_node *node,
//			   unsigned int *count);
static void print_record(const struct pfx_record *record, void *data);
//static void update_cb(struct pfx_table *p, const struct pfx_record rec,
//		      const bool added);
//static void ipv6_addr_to_network_byte_order(const uint32_t *src,
//					    uint32_t *dest);
//static void revalidate_prefix(struct bgp *bgp, afi_t afi,
//			      struct prefix *prefix);
/*static int rpki_update_cb_sync_bgpd(struct thread *thread);*/
/*static void rpki_update_cb_sync_rtr(struct pfx_table *p __attribute__((unused)),*/
				    /*const struct pfx_record rec,*/
				    /*const bool added __attribute__((unused)));*/

/*****************************************/
/** Implementation of public functions  **/
/*****************************************/

static struct rtr_socket *create_rtr_socket(struct tr_socket *tr_socket)
{
	struct rtr_socket *rtr_socket =
		XMALLOC(MTYPE_BGP_RPKI_CACHE, sizeof(struct rtr_socket));
	if (rtr_socket == NULL) {
		return NULL;
	}
	rtr_socket->tr_socket = tr_socket;
	return rtr_socket;
}

static struct cache *find_cache(const char *host, const char *port_string)
{
	struct listnode *cache_node;
	struct cache *cache;
	unsigned int port_int = atoi(port_string);
	for (ALL_LIST_ELEMENTS_RO(cache_list, cache_node, cache)) {
		if (cache->type == TCP ) {
			struct tr_tcp_config *config = cache->tr_config.tcp_config;
			if (strcmp(config->host, host) &&
			    strcmp(config->port, port_string)) {
				return cache;
			}
		} else {
			struct tr_ssh_config *config = cache->tr_config.ssh_config;
			if (strcmp(config->host, host) &&
			    config->port == port_int) {
				return cache;
			}
		}
	}
	return NULL;
}

static struct cache *find_cache_pref(const uint8_t preference)
{
	struct listnode *cache_node;
	struct cache *cache;
	for (ALL_LIST_ELEMENTS_RO(cache_list, cache_node, cache)) {
		if (cache->preference == preference) {
			return cache;
		}
	}
	return NULL;
}



/*static struct list *create_cache_list()
{
	struct list *cache_list = list_new();
	cache_list->del = delete_cache;
	cache_list->count = 0;
	return cache_list;
}*/

/*static cache_group *create_cache_group(int preference_value)
{
	cache_group *group;
	if ((group = XMALLOC(MTYPE_BGP_RPKI_CACHE_GROUP, sizeof(cache_group)))
	    == NULL) {
		return NULL;
	}
	group->cache_config_list = create_cache_list();
	group->preference_value = preference_value;
	group->delete_flag = 0;
	return group;
}*/

/*static void reprocess_routes(struct bgp *bgp)
{
	afi_t afi;
	for (afi = AFI_IP; afi < AFI_MAX; ++afi) {
		rpki_revalidate_all_routes(bgp, afi);
	}
}*/

/*static void ipv6_addr_to_network_byte_order(const uint32_t *src, uint32_t *dest)
{
	int i;
	for (i = 0; i < 4; i++)
		dest[i] = htonl(src[i]);
}*/

static void print_record(const struct pfx_record *record, void *data)
{
	char ip[INET6_ADDRSTRLEN];
	struct rpki_for_each_record_arg *arg = data;
	struct vty *vty = arg->vty;

	arg->prefix_amount++;

	lrtr_ip_addr_to_str(&(record->prefix), ip, sizeof(ip));
	vty_out(vty, "%-40s   %3u - %3u   %10u\n", ip, record->min_len,
		record->max_len, record->asn);
}

/*static void list_all_nodes(struct vty *vty, const struct trie_node *node,
			   unsigned int *count)
{
	*count += 1;

	if (node->lchild != NULL) {
		list_all_nodes(vty, node->lchild, count);
	}

	print_record(vty, node);

	if (node->rchild != NULL) {
		list_all_nodes(vty, node->rchild, count);
	}
}*/

struct rtr_mgr_group *get_rtr_mgr_groups()
{
	struct listnode *cache_node;
	struct rtr_mgr_group *rtr_mgr_groups;
	struct cache *cache;

	int number_of_groups = listcount(cache_list);
	if (number_of_groups == 0) {
		return NULL;
	}

	if ((rtr_mgr_groups =
		     XMALLOC(MTYPE_BGP_RPKI_CACHE_GROUP,
			     number_of_groups * sizeof(struct rtr_mgr_group)))
	    == NULL) {
		return NULL;
	}

	size_t i = 0;

	for (ALL_LIST_ELEMENTS_RO(cache_list, cache_node, cache)) {
		rtr_mgr_groups[i].sockets = &(cache->rtr_socket);
		rtr_mgr_groups[i].sockets_len = 1;
		rtr_mgr_groups[i].preference = cache->preference;
	}

	return rtr_mgr_groups;
}

/*unsigned int get_number_of_cache_groups()
{
	delete_marked_cache_groups();
	return listcount(cache_group_list);
}*/

/*void delete_cache_group_list()
{
	list_delete(cache_group_list);
}*/

inline void rpki_set_route_map_active(int activate)
{
	route_map_active = activate;
}

inline int rpki_is_route_map_active()
{
	return route_map_active;
}

inline int rpki_is_synchronized(void)
{
	return rtr_is_running && rtr_mgr_conf_in_sync(rtr_config);
}

inline int rpki_is_running(void)
{
	return rtr_is_running;
}

/*static void revalidate_prefix(struct bgp *bgp, afi_t afi, struct prefix *prefix)
{
	struct bgp_node *bgp_node;
	struct bgp_info *bgp_info;
	safi_t safi;

	for (safi = SAFI_UNICAST; safi < SAFI_MAX; safi++) {
		bgp_node = bgp_node_lookup(bgp->rib[afi][safi], prefix);
		if (bgp_node != NULL && bgp_node->info != NULL) {
			bool status_changed = false;
			for (bgp_info = bgp_node->info; bgp_info;
			     bgp_info = bgp_info->next) {
				u_char old_status =
					bgp_info->rpki_validation_status;
				bgp_info->rpki_validation_status =
					rpki_validate_prefix(bgp_info->peer,
							     bgp_info->attr,
							     &bgp_node->p);
				if (old_status
				    != bgp_info->rpki_validation_status) {
					status_changed = true;
				}
			}
			if (status_changed) {
				int ret;
				struct bgp_adj_in *ain;
				for (ain = bgp_node->adj_in; ain;
				     ain = ain->next) {
					struct bgp_info *ri = bgp_node->info;
					u_char *tag = (ri && ri->extra)
							      ? ri->extra->tag
							      : NULL;
					ret = bgp_update(
						ain->peer, &bgp_node->p,
						ain->attr, afi, safi,
						ZEBRA_ROUTE_BGP,
						BGP_ROUTE_NORMAL, NULL, tag, 1);

					if (ret < 0) {
						bgp_unlock_node(bgp_node);
						return;
					}
				}
			}
		}
	}
}*/

/*static void update_cb(struct pfx_table *p __attribute__((unused)),
		      const struct pfx_record rec,
		      const bool added __attribute__((unused)))
{
	struct bgp *bgp;
	struct listnode *node;
	struct prefix prefix;

	if (!rpki_is_synchronized()) {
		return;
	}

	for (ALL_LIST_ELEMENTS_RO(bm->bgp, node, bgp)) {
		if (bgp_flag_check(bgp, BGP_FLAG_VALIDATE_DISABLE)) {
			continue;
		}
		for (prefix.prefixlen = rec.min_len;
		     prefix.prefixlen < rec.max_len; ++prefix.prefixlen) {
			switch (rec.prefix.ver) {
			case LRTR_IPV4:
				prefix.family = AFI_IP;
				prefix.u.prefix4.s_addr =
					htonl(rec.prefix.u.addr4.addr);
				revalidate_prefix(bgp, AFI_IP, &prefix);
				break;
			case LRTR_IPV6:
				prefix.family = AFI_IP6;
				ipv6_addr_to_network_byte_order(
					rec.prefix.u.addr6.addr,
					prefix.u.prefix6.s6_addr32);
				revalidate_prefix(bgp, AFI_IP6, &prefix);
				break;
			default:
				break;
			}
		}
	}
}*/

/*void rpki_init_sync_socket()
{
	int fds[2];
	if (socketpair(PF_LOCAL, SOCK_DGRAM, 0, fds) != 0) {
		RPKI_DEBUG("Could not open rpki sync socket");
		return;
	}
	rpki_sync_socket_rtr = fds[0];
	rpki_sync_socket_bgpd = fds[1];
	fcntl(rpki_sync_socket_rtr, F_SETFL, O_NONBLOCK);
	thread_add_read(bm->master, rpki_update_cb_sync_bgpd, 0,
			rpki_sync_socket_bgpd, NULL);
}*/

static int bgp_rpki_init(struct thread_master *master)
{
	//rpki_init_sync_socket();
	rpki_debug = 0;
	rtr_is_running = 0;

	cache_list = list_new();
	cache_list->del = &bgp_rpki_free_cache;

	polling_period = POLLING_PERIOD_DEFAULT;
	expire_interval = EXPIRE_INTERVAL_DEFAULT;
	retry_interval = RETRY_INTERVAL_DEFAULT;
	timeout = TIMEOUT_DEFAULT;
	initial_synchronisation_timeout =
		INITIAL_SYNCHRONISATION_TIMEOUT_DEFAULT;
	install_cli_commands();
	rpki_start();
	return 0;
}

static int bgp_rpki_fini()
{
	if (rtr_is_running) {
		rtr_mgr_stop(rtr_config);
		rtr_mgr_free(rtr_config);
	}

	list_delete(cache_list);


	return 0;
}

static int bgp_rpki_module_init(void)
{
	lrtr_set_alloc_functions(rpki_malloc_wrapper,
				 rpki_realloc_wrapper,
				 rpki_free_wrapper);

	hook_register(frr_late_init, bgp_rpki_init);
	hook_register(frr_early_fini, &bgp_rpki_fini);

	return 0;
}


void rpki_start(void)
{
	unsigned int waiting_time = 0;
	if (list_isempty(cache_list)) {
		RPKI_DEBUG(
			"No caches were found in config. Prefix validation is off.");
		return;
	}
	RPKI_DEBUG("Init rtr_mgr.");
	int groups_len = listcount(cache_list);
	struct rtr_mgr_group *groups = get_rtr_mgr_groups();

	rtr_mgr_init(&rtr_config, groups, groups_len, polling_period, expire_interval,
		     retry_interval, NULL, NULL, NULL, NULL);

	RPKI_DEBUG("Starting rtr_mgr.");
	rtr_mgr_start(rtr_config);
	rtr_is_running = 1;
	RPKI_DEBUG("Waiting for rtr connection to synchronize.");
	while (waiting_time++ <= initial_synchronisation_timeout) {
		if (rtr_mgr_conf_in_sync(rtr_config)) {
			break;
		}
		sleep(1);
	}
	if (rtr_mgr_conf_in_sync(rtr_config)) {
		RPKI_DEBUG("Got synchronisation with at least one RPKI cache!");
	} else {
		RPKI_DEBUG(
			"Timeout expired! Proceeding without RPKI validation data.");
	}

	XFREE(MTYPE_BGP_RPKI_CACHE_GROUP, groups);
}

void rpki_reset_session(void)
{
	RPKI_DEBUG("Resetting RPKI Session");
	if (rtr_is_running) {
		rtr_mgr_stop(rtr_config);
		rtr_mgr_free(rtr_config);
		rtr_is_running = 0;
	}
	rpki_start();
}

void rpki_finish(void)
{
	RPKI_DEBUG("Stopping");

	rtr_mgr_stop(rtr_config);
	rtr_mgr_free(rtr_config);
	rtr_is_running = 0;
}

struct rtr_mgr_group* rpki_get_connected_group()
{
	if (list_isempty(cache_list)) {
		return NULL;
	}

	return rtr_mgr_get_first_group(rtr_config);
}

void rpki_print_prefix_table(struct vty *vty)
{
	struct rpki_for_each_record_arg arg;

	unsigned int number_of_ipv4_prefixes = 0;
	unsigned int number_of_ipv6_prefixes = 0;
	arg.vty = vty;
	struct pfx_table *pfx_table = rpki_get_connected_group()->sockets[0]->pfx_table;

	vty_out(vty, "RPKI/RTR prefix table\n");
	vty_out(vty, "%-40s %s  %s \n", "Prefix", "Prefix Length", "Origin-AS");

	arg.prefix_amount = &number_of_ipv4_prefixes;
	pfx_table_for_each_ipv4_record(pfx_table, print_record, &arg);

	arg.prefix_amount = &number_of_ipv6_prefixes;
	pfx_table_for_each_ipv6_record(pfx_table, print_record, &arg);

	vty_out(vty, "Number of IPv4 Prefixes: %u \n", number_of_ipv4_prefixes);
	vty_out(vty, "Number of IPv6 Prefixes: %u \n", number_of_ipv6_prefixes);
}

/*void rpki_set_validation_status(struct bgp *bgp, struct bgp_info *bgp_info,
				struct prefix *prefix)
{
	int validate_disable = bgp_flag_check(bgp, BGP_FLAG_VALIDATE_DISABLE);

	for (; bgp_info; bgp_info = bgp_info->next) {
		if (validate_disable) {
			bgp_info->rpki_validation_status = 0;
		} else {
			bgp_info->rpki_validation_status = rpki_validate_prefix(
				bgp_info->peer, bgp_info->attr, prefix);
		}
	}
}*/

int rpki_validate_prefix(struct peer *peer, struct attr *attr,
			 struct prefix *prefix)
{
	struct assegment *as_segment;
	as_t as_number = 0;
	struct lrtr_ip_addr ip_addr_prefix;
	enum pfxv_state result;
	char buf[BUFSIZ];
	const char *prefix_string;

	if (!rpki_is_synchronized()
	    || bgp_flag_check(peer->bgp, BGP_FLAG_VALIDATE_DISABLE)) {
		return 0;
	}

	// No aspath means route comes from iBGP
	if (!attr->aspath || !attr->aspath->segments) {
		// Set own as number
		as_number = peer->bgp->as;
	} else {
		as_segment = attr->aspath->segments;
		// Find last AsSegment
		while (as_segment->next) {
			as_segment = as_segment->next;
		}
		if (as_segment->type == AS_SEQUENCE) {
			// Get rightmost asn
			as_number = as_segment->as[as_segment->length - 1];
		} else if (as_segment->type == AS_CONFED_SEQUENCE
			   || as_segment->type == AS_CONFED_SET) {
			// Set own as number
			as_number = peer->bgp->as;
		} else {
			// RFC says: "Take distinguished value NONE as asn"
			// which means state is unknown
			return RPKI_NOTFOUND;
		}
	}

	// Get the prefix in requested format
	switch (prefix->family) {
	case AF_INET:
		ip_addr_prefix.ver = LRTR_IPV4;
		ip_addr_prefix.u.addr4.addr = ntohl(prefix->u.prefix4.s_addr);
		break;

#ifdef HAVE_IPV6
	case AF_INET6:
		ip_addr_prefix.ver = LRTR_IPV6;
		ipv6_addr_to_host_byte_order(prefix->u.prefix6.s6_addr32,
					     ip_addr_prefix.u.addr6.addr);
		break;
#endif /* HAVE_IPV6 */

	default:
		return 0;
	}

	// Do the actual validation
	rtr_mgr_validate(rtr_config, as_number, &ip_addr_prefix,
			 prefix->prefixlen, &result);

	// Print Debug output
	prefix_string =
		inet_ntop(prefix->family, &prefix->u.prefix, buf, BUFSIZ);
	switch (result) {
	case BGP_PFXV_STATE_VALID:
		RPKI_DEBUG(
			"Validating Prefix %s/%hhu from asn %u    Result: VALID",
			prefix_string, prefix->prefixlen, as_number);
		return RPKI_VALID;
	case BGP_PFXV_STATE_NOT_FOUND:
		RPKI_DEBUG(
			"Validating Prefix %s/%hhu from asn %u    Result: NOT FOUND",
			prefix_string, prefix->prefixlen, as_number);
		return RPKI_NOTFOUND;
	case BGP_PFXV_STATE_INVALID:
		RPKI_DEBUG(
			"Validating Prefix %s/%hhu from asn %u    Result: INVALID",
			prefix_string, prefix->prefixlen, as_number);
		return RPKI_INVALID;
	default:
		RPKI_DEBUG(
			"Validating Prefix %s/%hhu from asn %u    Result: CANNOT VALIDATE",
			prefix_string, prefix->prefixlen, as_number);
		break;
	}
	return 0;
}

/*void rpki_revalidate_all_routes(struct bgp *bgp, afi_t afi)
{
	struct bgp_node *bgp_node;
	struct bgp_info *bgp_info;
	safi_t safi;
	for (safi = SAFI_UNICAST; safi < SAFI_MAX; safi++) {
		for (bgp_node = bgp_table_top(bgp->rib[afi][safi]); bgp_node;
		     bgp_node = bgp_route_next(bgp_node)) {
			if (bgp_node->info != NULL) {
				bool status_changed = false;
				for (bgp_info = bgp_node->info; bgp_info;
				     bgp_info = bgp_info->next) {
					u_char old_status =
						bgp_info->rpki_validation_status;
					bgp_info->rpki_validation_status =
						rpki_validate_prefix(
							bgp_info->peer,
							bgp_info->attr,
							&bgp_node->p);
					if (old_status
					    != bgp_info->rpki_validation_status) {
						status_changed = true;
					}
				}
				if (status_changed) {
					bgp_process(bgp, bgp_node, afi, safi);
				}
			}
		}
	}
}*/

/*****************************************/
/** Implementation of private functions **/
/*****************************************/

/*static int rpki_update_cb_sync_bgpd(struct thread *thread)
{
	struct pfx_record *rec;
	thread_add_read(bm->master, rpki_update_cb_sync_bgpd, 0,
			rpki_sync_socket_bgpd, NULL);
	int rtval =
		read(rpki_sync_socket_bgpd, &rec, sizeof(struct pfx_record *));
	if (rtval < 1) {
		RPKI_DEBUG("Could not read from rpki_sync_socket_bgpd");
		// memory leak?
		return rtval;
	}
	update_cb(NULL, *rec, NULL);
	free(rec);
	return 0;
}*/


/*static void rpki_update_cb_sync_rtr(struct pfx_table *p __attribute__((unused)),
				    const struct pfx_record rec,
				    const bool added __attribute__((unused)))
{

	struct pfx_record *rec_copy = malloc(sizeof(struct pfx_record));
	memcpy(rec_copy, &rec, sizeof(struct pfx_record));
	int rtval = write(rpki_sync_socket_rtr, &rec_copy,
			  sizeof(struct pfx_record *));
	if (rtval < 1) {
		RPKI_DEBUG("Could not write to rpki_sync_socket_rtr");
	}
}*/


static int add_cache(struct cache *cache)
{
	uint8_t preference = cache->preference;
	struct rtr_mgr_group group;

	group.preference = preference;
	group.sockets_len = 1;
	group.sockets = &(cache->rtr_socket);

	listnode_add(cache_list, cache);

	if (rtr_is_running &&
	    rtr_mgr_add_group(rtr_config, &group) != RTR_SUCCESS) {
		return ERROR;
	}

	return SUCCESS;

}

static int add_tcp_cache(const char *host,
			 const char *port,
			 const uint8_t preference)
{
	struct tr_tcp_config *tcp_config =
		XMALLOC(MTYPE_BGP_RPKI_CACHE, sizeof(struct tr_tcp_config));
	struct tr_socket *tr_socket;
	struct rtr_socket *rtr_socket;
	struct cache *cache = XMALLOC(MTYPE_BGP_RPKI_CACHE, sizeof(struct cache));
	if ((tr_socket =
		     XMALLOC(MTYPE_BGP_RPKI_CACHE, sizeof(struct tr_socket)))
	    == NULL) {
		return ERROR;
	}

	tcp_config->host = XSTRDUP(MTYPE_BGP_RPKI_CACHE, host);
	tcp_config->port = XSTRDUP(MTYPE_BGP_RPKI_CACHE, port);
	tcp_config->bindaddr = NULL;

	tr_tcp_init(tcp_config, tr_socket);

	if ((rtr_socket = create_rtr_socket(tr_socket)) == NULL) {
		return ERROR;
	}

	cache->type = TCP;
	cache->tr_socket = tr_socket;
	cache->tr_config.tcp_config = tcp_config;
	cache->rtr_socket = rtr_socket;
	cache->preference = preference;

	return add_cache(cache);
}

static int add_ssh_cache(const char *host,
			 const unsigned int port,
			 const char *username,
			 const char *client_privkey_path,
			 const char *client_pubkey_path,
			 const char *server_pubkey_path,
			 const uint8_t preference)
{

	struct tr_ssh_config *ssh_config =
		XMALLOC(MTYPE_BGP_RPKI_CACHE, sizeof(struct tr_ssh_config));
	struct cache *cache = XMALLOC(MTYPE_BGP_RPKI_CACHE, sizeof(struct cache));
	struct tr_socket *tr_socket;
	struct rtr_socket *rtr_socket;
	if ((tr_socket = XMALLOC(MTYPE_BGP_RPKI_CACHE, sizeof(struct tr_socket)))
	    == NULL) {
		return ERROR;
	}

	ssh_config->port = port;
	ssh_config->host = XSTRDUP(MTYPE_BGP_RPKI_CACHE, host);
	ssh_config->bindaddr = NULL;

	ssh_config->username = XSTRDUP(MTYPE_BGP_RPKI_CACHE, username);
	ssh_config->client_privkey_path = XSTRDUP(
			MTYPE_BGP_RPKI_CACHE, client_privkey_path);
	ssh_config->server_hostkey_path = XSTRDUP(MTYPE_BGP_RPKI_CACHE, server_pubkey_path);

	tr_ssh_init(ssh_config, tr_socket);
	if ((rtr_socket = create_rtr_socket(tr_socket)) == NULL) {
		return ERROR;
	}

	cache->type = SSH;
	cache->tr_socket = tr_socket;
	cache->tr_config.ssh_config = ssh_config;
	cache->rtr_socket = rtr_socket;
	cache->preference = preference;

	return add_cache(cache);
}

static void bgp_rpki_free_cache(struct cache *cache)
{
	if (cache->type == TCP) {
		XFREE(MTYPE_BGP_RPKI_CACHE,
		      cache->tr_config.tcp_config->host);
		XFREE(MTYPE_BGP_RPKI_CACHE,
		      cache->tr_config.tcp_config->port);
		XFREE(MTYPE_BGP_RPKI_CACHE, cache->tr_config.tcp_config);
	} else {
		XFREE(MTYPE_BGP_RPKI_CACHE,
		      cache->tr_config.ssh_config->host);
		XFREE(MTYPE_BGP_RPKI_CACHE,
		      cache->tr_config.ssh_config->username);
		XFREE(MTYPE_BGP_RPKI_CACHE,
		      cache->tr_config.ssh_config->client_privkey_path);
		XFREE(MTYPE_BGP_RPKI_CACHE,
		      cache->tr_config.ssh_config->server_hostkey_path);
		XFREE(MTYPE_BGP_RPKI_CACHE, cache->tr_config.ssh_config);
	}
	XFREE(MTYPE_BGP_RPKI_CACHE, cache->tr_socket);
	XFREE(MTYPE_BGP_RPKI_CACHE, cache->rtr_socket);
	XFREE(MTYPE_BGP_RPKI_CACHE, cache);
}

/*static void delete_cache_group(void *_cache_group)
{
	cache_group *group = _cache_group;
	list_delete(group->cache_config_list);
	XFREE(MTYPE_BGP_RPKI_CACHE_GROUP, group);
}*/

static int rpki_config_write(struct vty *vty)
{
	struct listnode *cache_node;
	struct cache *cache;
	if (listcount(cache_list)) {
		if (rpki_debug) {
			vty_out(vty, "debug rpki\n");
		}
		vty_out(vty, "! \n");
		vty_out(vty, "rpki\n");
		vty_out(vty, "  rpki polling_period %d \n", polling_period);
		vty_out(vty, "  rpki timeout %d \n", timeout);
		vty_out(vty, "  rpki initial-synchronisation-timeout %d \n",
			initial_synchronisation_timeout);
		vty_out(vty, "! \n");
		for (ALL_LIST_ELEMENTS_RO(cache_list, cache_node,
					  cache)) {
			switch (cache->type) {
				struct tr_tcp_config *tcp_config;
				struct tr_ssh_config *ssh_config;
			case TCP:
				tcp_config = cache->tr_config.tcp_config;
				vty_out(vty, "    rpki cache %s %s \n",
					tcp_config->host,
					tcp_config->port);
				break;

			case SSH:
				ssh_config = cache->tr_config.ssh_config;
				vty_out(vty,
					"    rpki cache %s %u %s %s %s \n",
					ssh_config->host,
					ssh_config->port,
					ssh_config->username,
					ssh_config->client_privkey_path,
					ssh_config->server_hostkey_path
							!= NULL
						? ssh_config
							  ->server_hostkey_path
						: " ");
				break;

			default:
				break;
			}
		}
		return 1;
	} else {
		return 0;
	}
}

DEFUN (rpki,
    rpki_cmd,
    "rpki",
    BGP_STR
    "Enable rpki and enter rpki configuration mode\n")
{
	vty->node = RPKI_NODE;
	return CMD_SUCCESS;
}

DEFPY (rpki_polling_period,
    rpki_polling_period_cmd,
    "rpki polling_period (1-86400)$pp",
    RPKI_OUTPUT_STRING
    "Set polling period\n"
    "Polling period value\n")
{
	polling_period = pp;
	return CMD_SUCCESS;
}

DEFUN (no_rpki_polling_period,
    no_rpki_polling_period_cmd,
    "no rpki polling_period",
    NO_STR
    RPKI_OUTPUT_STRING
    "Set polling period back to default\n")
{
	polling_period = POLLING_PERIOD_DEFAULT;
	return CMD_SUCCESS;
}

DEFPY (rpki_expire_interval,
    rpki_expire_interval_cmd,
    "rpki expire_interval (600-172800)$tmp",
    RPKI_OUTPUT_STRING
    "Set expire interval\n"
    "Expire interval value\n")
{
	if (tmp >= polling_period) {
		expire_interval = tmp;
		return CMD_SUCCESS;
	} else {
		vty_out(vty,
			"%% Expiry interval must be polling period or larger\n");
		return CMD_WARNING_CONFIG_FAILED;
	}
}

DEFUN (no_rpki_expire_interval,
    no_rpki_expire_interval_cmd,
    "no rpki expire_interval",
    NO_STR
    RPKI_OUTPUT_STRING
    "Set expire interval back to default\n")
{
	expire_interval = polling_period * 2;
	return CMD_SUCCESS;
}

DEFPY (rpki_retry_interval,
    rpki_retry_interval_cmd,
    "rpki retry_interval (1-7200)$tmp",
    RPKI_OUTPUT_STRING
    "Set retry interval\n"
    "retry interval value\n")
{
	retry_interval = tmp;
	return CMD_SUCCESS;
}

DEFUN (no_rpki_retry_interval,
    no_rpki_retry_interval_cmd,
    "no rpki retry_interval",
    NO_STR
    RPKI_OUTPUT_STRING
    "Set retry interval back to default\n")
{
	retry_interval = RETRY_INTERVAL_DEFAULT;
	return CMD_SUCCESS;
}

DEFPY (rpki_timeout,
    rpki_timeout_cmd,
    "rpki timeout (1-4294967295)$to_arg",
    RPKI_OUTPUT_STRING
    "Set timeout\n"
    "Timeout value\n")
{
	timeout = to_arg;
	return CMD_SUCCESS;
}

DEFUN (no_rpki_timeout,
    no_rpki_timeout_cmd,
    "no rpki timeout",
    NO_STR
    RPKI_OUTPUT_STRING
    "Set timeout back to default\n")
{
	timeout = TIMEOUT_DEFAULT;
	return CMD_SUCCESS;
}

DEFPY (rpki_synchronisation_timeout,
    rpki_synchronisation_timeout_cmd,
    "rpki initial-synchronisation-timeout (1-4294967295)$ito_arg",
    RPKI_OUTPUT_STRING
    "Set a timeout for the initial synchronisation of prefix validation data\n"
    "Timeout value\n")
{
	initial_synchronisation_timeout = ito_arg;
	return CMD_SUCCESS;
}

DEFUN (no_rpki_synchronisation_timeout,
    no_rpki_synchronisation_timeout_cmd,
    "no rpki initial-synchronisation-timeout",
    NO_STR
    RPKI_OUTPUT_STRING
    "Set the inital synchronisation timeout back to default (30 sec.)\n")
{
	initial_synchronisation_timeout =
		INITIAL_SYNCHRONISATION_TIMEOUT_DEFAULT;
	return CMD_SUCCESS;
}

/*DEFPY (rpki_group,
    rpki_group_cmd,
    "rpki group (0-4294967295)",
    RPKI_OUTPUT_STRING
    "Select an existing or start a new group of cache servers\n"
    "Preference Value for this group (lower value means higher preference)\n")
{
	cache_group *new_cache_group;

	currently_selected_cache_group = find_cache_group(group);

	// Group does not yet exist so create new one
	if (currently_selected_cache_group == NULL) {
		if ((new_cache_group = create_cache_group(group)) == NULL) {
			vty_out(vty,
				"Could not create new rpki cache group because "
				"of memory allocation error\n");
			return CMD_WARNING;
		}
		listnode_add(cache_group_list, new_cache_group);
		currently_selected_cache_group = new_cache_group;
	}
	return CMD_SUCCESS;
}

DEFPY (no_rpki_group,
    no_rpki_group_cmd,
    "no rpki group (0-4294967295)",
    NO_STR
    RPKI_OUTPUT_STRING
    "Remove a group of cache servers\n"
    "Preference Value for this group (lower value means higher preference)\n")
{
	cache_group *cache_group;
	cache_group = find_cache_group(group);
	if (cache_group == NULL) {
		vty_out(vty,
			"There is no cache group with preference value %ld\n",
			group);
		return CMD_WARNING;
	}
	cache_group->delete_flag = 1;
	currently_selected_cache_group = NULL;
	return CMD_SUCCESS;
}*/


DEFPY (rpki_cache,
    rpki_cache_cmd,
    "rpki cache <A.B.C.D|WORD> "
    "<TCPPORT|(1-65535)$sshport SSH_UNAME SSH_PRIVKEY SSH_PUBKEY [SERVER_PUBKEY]> "
    "preference (1-255)",
    RPKI_OUTPUT_STRING
    "Install a cache server to current group\n"
    "IP address of cache server\n Hostname of cache server\n"
    "TCP port number \n"
    "SSH port number \n"
    "SSH user name \n"
    "Path to own SSH private key \n"
    "Path to own SSH public key \n"
    "Path to Public key of cache server \n")
{
	int return_value = SUCCESS;
	// use ssh connection

	if (ssh_uname) {
		return_value = add_ssh_cache(
			cache, sshport, ssh_uname, ssh_privkey, ssh_pubkey,
			server_pubkey, preference);
	} else { // use tcp connection
		return_value = add_tcp_cache(cache, tcpport, preference);
		vty_out(vty,
			"TEMPORARY RPKI DBUGMSG: Added TCP cache to group\n");
	}

	if (return_value == ERROR) {
		vty_out(vty,
			"Could not create new rpki cache because "
			"of memory allocation error\n");
		return CMD_WARNING;
	}

	if (!rpki_is_running()) {
		rpki_start();
	}

	//rpki_start();

	return CMD_SUCCESS;
}

DEFPY (no_rpki_cache,
    no_rpki_cache_cmd,
    "no rpki cache (1-255)$preference",
    NO_STR
    RPKI_OUTPUT_STRING
    "Remove a cache server\n"
    "Preference of the server\n")
{
	struct cache *cache = find_cache_pref(preference);

	if (cache == NULL) {
		vty_out(vty, "Could not find cache %ld\n", preference);
		return CMD_WARNING;
	}

	if (rtr_is_running) {
		if (rtr_mgr_remove_group(rtr_config, preference) == RTR_ERROR) {
			vty_out(vty, "Could not remove cache %ld\n", preference);
			return CMD_WARNING;
		}
	}

	listnode_delete(cache_list, cache);
	bgp_rpki_free_cache(cache);

	return CMD_SUCCESS;
}


/*DEFUN (bgp_bestpath_prefix_validate_disable,
      bgp_bestpaient.c:189: CHECK:BRACES: Unbalancedth_prefix_validate_disable_cmd,
       "bgp bestpath prefix-validate disable",
       "BGP specific commands\n"
       "Change the default bestpath selection\n"
       "Prefix validation attribute\n"
       "Disable prefix validation\n")
{
	VTY_DECLVAR_CONTEXT(bgp, bgp);
	bgp_flag_set(bgp, BGP_FLAG_VALIDATE_DISABLE);
	//reprocess_routes(bgp);
	return CMD_SUCCESS;
}*/

/*DEFUN (no_bgp_bestpath_prefix_validate_disable,
    no_bgp_bestpath_prefix_validate_disable_cmd,
       "no bgp bestpath prefix-validate disable",
       NO_STR
       "BGP specific commands\n"
       "Change the default bestpath selection\n"
       "Prefix validation attribute\n"
       "Disable prefix validation\n")
{
	VTY_DECLVAR_CONTEXT(bgp, bgp);
	bgp_flag_unset(bgp, BGP_FLAG_VALIDATE_DISABLE);
	//reprocess_routes(bgp);
	return CMD_SUCCESS;
}*/

/*DEFUN (bgp_bestpath_prefix_validate_allow_invalid,
      bgp_bestpath_prefix_validate_allow_invalid_cmd,
       "bgp bestpath prefix-validate allow-invalid",
       "BGP specific commands\n"
       "Change the default bestpath selection\n"
       "Prefix validation attribute\n"
       "Allow routes to be selected as bestpath even if their prefix validation status is invalid\n")
{
	VTY_DECLVAR_CONTEXT(bgp, bgp);
	bgp_flag_set(bgp, BGP_FLAG_ALLOW_INVALID);
	//reprocess_routes(bgp);
	return CMD_SUCCESS;
}*/

/*DEFUN (no_bgp_bestpath_prefix_validate_allow_invalid,
    no_bgp_bestpath_prefix_validate_allow_invalid_cmd,
       "no bgp bestpath prefix-validate allow-invalid",
       NO_STR
       "BGP specific commands\n"
       "Change the default bestpath selection\n"
       "Prefix validation attribute\n"
       "Allow routes to be selected as bestpath even if their prefix validation status is invalid\n")
{
	VTY_DECLVAR_CONTEXT(bgp, bgp);
	bgp_flag_unset(bgp, BGP_FLAG_ALLOW_INVALID);
	//reprocess_routes(bgp);
	return CMD_SUCCESS;
}*/

DEFUN (show_rpki_prefix_table,
    show_rpki_prefix_table_cmd,
    "show rpki prefix-table",
    SHOW_STR
    RPKI_OUTPUT_STRING
    "Show validated prefixes which were received from RPKI Cache")
{
	struct listnode *cache_node;
	struct cache *cache;

	for (ALL_LIST_ELEMENTS_RO(cache_list, cache_node, cache)) {
		vty_out(vty, "host: %s port: %s\n", cache->tr_config.tcp_config->host, cache->tr_config.tcp_config->port);
	}
	if (rpki_is_synchronized()) {
		rpki_print_prefix_table(vty);
	} else {
		vty_out(vty, "No connection to RPKI cache server.\n");
	}
	return CMD_SUCCESS;
}
// TODO: make this work again!
DEFUN (show_rpki_cache_connection,
    show_rpki_cache_connection_cmd,
    "show rpki cache-connection",
    SHOW_STR
    RPKI_OUTPUT_STRING
    "Show to which RPKI Cache Servers we have a connection")
{
	if (rpki_is_synchronized()) {
		struct listnode *cache_node;
		struct cache *cache;
		struct rtr_mgr_group* group = rpki_get_connected_group();
		if (group == NULL) {
			vty_out(vty, "Cannot find a connected group. \n");
			return CMD_SUCCESS;
		}
		vty_out(vty, "Connected to group %d \n", group->preference);
		for (ALL_LIST_ELEMENTS_RO(cache_list, cache_node,
					  cache)) {
			if (cache->preference == group->preference) {
				struct tr_tcp_config *tcp_config;
				struct tr_ssh_config *ssh_config;

				switch (cache->type) {
				case TCP:
					tcp_config =
						cache->tr_config
							.tcp_config;
					vty_out(vty,
						"rpki tcp cache %s %s "
						"pref %hhu \n",
						tcp_config->host,
						tcp_config->port,
						cache->preference);
					break;

				case SSH:
					ssh_config =
						cache->tr_config
							.ssh_config;
					vty_out(vty,
						"rpki ssh cache %s %u "
						"pref %hhu \n",
						ssh_config->host,
						ssh_config->port,
						cache->preference);
					break;

				default:
					break;
				}
			}
		}
	} else {
		vty_out(vty, "No connection to RPKI cache server.\n");
	}

	return CMD_SUCCESS;
}

DEFUN (rpki_exit,
    rpki_exit_cmd,
    "exit",
    "Exit rpki configuration and restart rpki session")
{
	rpki_reset_session();
	vty->node = CONFIG_NODE;
	return CMD_SUCCESS;
}

/* quit is alias of exit. */
ALIAS(rpki_exit, rpki_quit_cmd, "quit",
      "Exit rpki configuration and restart rpki session")

DEFUN (rpki_end,
    rpki_end_cmd,
    "end",
    "End rpki configuration, restart rpki session and change to enable mode.")
{
	rpki_reset_session();
	vty_config_unlock(vty);
	vty->node = ENABLE_NODE;
	return CMD_SUCCESS;
}

DEFUN (debug_rpki,
    debug_rpki_cmd,
       "debug rpki",
       DEBUG_STR
       "Enable debugging for rpki")
{
	rpki_debug = 1;
	return CMD_SUCCESS;
}

DEFUN (no_debug_rpki,
       no_debug_rpki_cmd,
       "no debug rpki",
       NO_STR
       DEBUG_STR
       "Disable debugging for rpki")
{
	rpki_debug = 0;
	return CMD_SUCCESS;
}
static void overwrite_exit_commands()
{
	unsigned int i;
	vector cmd_vector = rpki_node.cmd_vector;
	for (i = 0; i < cmd_vector->active; ++i) {
		struct cmd_element *cmd =
			(struct cmd_element *)vector_lookup(cmd_vector, i);
		if (strcmp(cmd->string, "exit") == 0
		    || strcmp(cmd->string, "quit") == 0
		    || strcmp(cmd->string, "exit") == 0) {
			vector_unset(cmd_vector, i);
		}
	}
	/*
	 The comments in the following 3 lines must not be removed.
	 They prevent the script ../vtysh/extract.pl from copying the lines
	 into ../vtysh/vtysh_cmd.c which would cause the commands to be
	 ambiguous
	 and we don't want that.
	 */
	install_element(RPKI_NODE /*DO NOT REMOVE THIS COMMENT*/,
			&rpki_exit_cmd);
	install_element(RPKI_NODE /*DO NOT REMOVE THIS COMMENT*/,
			&rpki_quit_cmd);
	install_element(RPKI_NODE /*DO NOT REMOVE THIS COMMENT*/,
			&rpki_end_cmd);
}
void install_cli_commands()
{
	//cache_group_list = list_new();
	//cache_group_list->del = delete_cache_group;
	//cache_group_list->count = 0;

	//TODO: make config write work
	install_node(&rpki_node, &rpki_config_write);
	install_default(RPKI_NODE);
	overwrite_exit_commands();
	install_element(CONFIG_NODE, &rpki_cmd);
	install_element(VIEW_NODE, &rpki_cmd);

	/* Install rpki polling period commands */
	install_element(CONFIG_NODE, &rpki_polling_period_cmd);
	install_element(CONFIG_NODE, &no_rpki_polling_period_cmd);

	/* Install rpki expire interval commands */
	install_element(CONFIG_NODE, &rpki_expire_interval_cmd);
	install_element(CONFIG_NODE, &no_rpki_expire_interval_cmd);

	/* Install rpki timeout commands */
	install_element(CONFIG_NODE, &rpki_timeout_cmd);
	install_element(CONFIG_NODE, &no_rpki_timeout_cmd);

	/* Install rpki synchronisation timeout commands */
	install_element(CONFIG_NODE, &rpki_synchronisation_timeout_cmd);
	install_element(CONFIG_NODE, &no_rpki_synchronisation_timeout_cmd);

	/* Install rpki group commands */
	//install_element(CONFIG_NODE, &rpki_group_cmd);
	//install_element(CONFIG_NODE, &no_rpki_group_cmd);

	/* Install rpki cache commands */
	install_element(CONFIG_NODE, &rpki_cache_cmd);
	install_element(CONFIG_NODE, &no_rpki_cache_cmd);

	/* Install prefix_validate disable commands */
	//install_element(BGP_NODE, &bgp_bestpath_prefix_validate_disable_cmd);
	//install_element(BGP_NODE, &no_bgp_bestpath_prefix_validate_disable_cmd);

	/* Install prefix_validate allow_invalid commands */
	//install_element(BGP_NODE,
	//		&bgp_bestpath_prefix_validate_allow_invalid_cmd);
	//install_element(BGP_NODE,
	//		&no_bgp_bestpath_prefix_validate_allow_invalid_cmd);

	/* Install show commands */
	install_element(ENABLE_NODE, &show_rpki_prefix_table_cmd);
	//TODO: make this work again!
	install_element(ENABLE_NODE, &show_rpki_cache_connection_cmd);

	/* Install debug commands */
	install_element(CONFIG_NODE, &debug_rpki_cmd);
	install_element(ENABLE_NODE, &debug_rpki_cmd);
	install_element(CONFIG_NODE, &no_debug_rpki_cmd);
	install_element(ENABLE_NODE, &no_debug_rpki_cmd);

	/*[> Install route match <]*/
	/*route_map_install_match(&route_match_rpki_cmd);*/
}
FRR_MODULE_SETUP(.name = "bgpd_rpki", .version = "0.3.6",
		 .description = "Enable RPKI support for FRR.",
	 .init = bgp_rpki_module_init)
