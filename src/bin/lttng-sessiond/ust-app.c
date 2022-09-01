/*
 * Copyright (C) 2011 EfficiOS Inc.
 * Copyright (C) 2016 Jérémie Galarneau <jeremie.galarneau@efficios.com>
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 */

#define _LGPL_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <urcu/compiler.h>
#include <signal.h>

#include <common/bytecode/bytecode.h>
#include <common/compat/errno.h>
#include <common/common.h>
#include <common/hashtable/utils.h>
#include <lttng/event-rule/event-rule.h>
#include <lttng/event-rule/event-rule-internal.h>
#include <lttng/event-rule/user-tracepoint.h>
#include <lttng/condition/condition.h>
#include <lttng/condition/event-rule-matches-internal.h>
#include <lttng/condition/event-rule-matches.h>
#include <lttng/trigger/trigger-internal.h>
#include <common/sessiond-comm/sessiond-comm.h>

#include "buffer-registry.h"
#include "condition-internal.h"
#include "fd-limit.h"
#include "health-sessiond.h"
#include "ust-app.h"
#include "ust-consumer.h"
#include "lttng-ust-ctl.h"
#include "lttng-ust-error.h"
#include "utils.h"
#include "session.h"
#include "lttng-sessiond.h"
#include "notification-thread-commands.h"
#include "rotate.h"
#include "event.h"
#include "event-notifier-error-accounting.h"
#include "ust-field-utils.h"

struct lttng_ht *ust_app_ht;
struct lttng_ht *ust_app_ht_by_sock;
struct lttng_ht *ust_app_ht_by_notify_sock;

static
int ust_app_flush_app_session(struct ust_app *app, struct ust_app_session *ua_sess);

/* Next available channel key. Access under next_channel_key_lock. */
static uint64_t _next_channel_key;
static pthread_mutex_t next_channel_key_lock = PTHREAD_MUTEX_INITIALIZER;

/* Next available session ID. Access under next_session_id_lock. */
static uint64_t _next_session_id;
static pthread_mutex_t next_session_id_lock = PTHREAD_MUTEX_INITIALIZER;

/*
 * Return the incremented value of next_channel_key.
 */
static uint64_t get_next_channel_key(void)
{
	uint64_t ret;

	pthread_mutex_lock(&next_channel_key_lock);
	ret = ++_next_channel_key;
	pthread_mutex_unlock(&next_channel_key_lock);
	return ret;
}

/*
 * Return the atomically incremented value of next_session_id.
 */
static uint64_t get_next_session_id(void)
{
	uint64_t ret;

	pthread_mutex_lock(&next_session_id_lock);
	ret = ++_next_session_id;
	pthread_mutex_unlock(&next_session_id_lock);
	return ret;
}

static void copy_channel_attr_to_ustctl(
		struct lttng_ust_ctl_consumer_channel_attr *attr,
		struct lttng_ust_abi_channel_attr *uattr)
{
	/* Copy event attributes since the layout is different. */
	attr->subbuf_size = uattr->subbuf_size;
	attr->num_subbuf = uattr->num_subbuf;
	attr->overwrite = uattr->overwrite;
	attr->switch_timer_interval = uattr->switch_timer_interval;
	attr->read_timer_interval = uattr->read_timer_interval;
	attr->output = uattr->output;
	attr->blocking_timeout = uattr->u.s.blocking_timeout;
}

/*
 * Match function for the hash table lookup.
 *
 * It matches an ust app event based on three attributes which are the event
 * name, the filter bytecode and the loglevel.
 */
static int ht_match_ust_app_event(struct cds_lfht_node *node, const void *_key)
{
	struct ust_app_event *event;
	const struct ust_app_ht_key *key;
	int ev_loglevel_value;

	assert(node);
	assert(_key);

	event = caa_container_of(node, struct ust_app_event, node.node);
	key = _key;
	ev_loglevel_value = event->attr.loglevel;

	/* Match the 4 elements of the key: name, filter, loglevel, exclusions */

	/* Event name */
	if (strncmp(event->attr.name, key->name, sizeof(event->attr.name)) != 0) {
		goto no_match;
	}

	/* Event loglevel. */
	if (ev_loglevel_value != key->loglevel_type) {
		if (event->attr.loglevel_type == LTTNG_UST_ABI_LOGLEVEL_ALL
				&& key->loglevel_type == 0 &&
				ev_loglevel_value == -1) {
			/*
			 * Match is accepted. This is because on event creation, the
			 * loglevel is set to -1 if the event loglevel type is ALL so 0 and
			 * -1 are accepted for this loglevel type since 0 is the one set by
			 * the API when receiving an enable event.
			 */
		} else {
			goto no_match;
		}
	}

	/* One of the filters is NULL, fail. */
	if ((key->filter && !event->filter) || (!key->filter && event->filter)) {
		goto no_match;
	}

	if (key->filter && event->filter) {
		/* Both filters exists, check length followed by the bytecode. */
		if (event->filter->len != key->filter->len ||
				memcmp(event->filter->data, key->filter->data,
					event->filter->len) != 0) {
			goto no_match;
		}
	}

	/* One of the exclusions is NULL, fail. */
	if ((key->exclusion && !event->exclusion) || (!key->exclusion && event->exclusion)) {
		goto no_match;
	}

	if (key->exclusion && event->exclusion) {
		/* Both exclusions exists, check count followed by the names. */
		if (event->exclusion->count != key->exclusion->count ||
				memcmp(event->exclusion->names, key->exclusion->names,
					event->exclusion->count * LTTNG_UST_ABI_SYM_NAME_LEN) != 0) {
			goto no_match;
		}
	}


	/* Match. */
	return 1;

no_match:
	return 0;
}

/*
 * Unique add of an ust app event in the given ht. This uses the custom
 * ht_match_ust_app_event match function and the event name as hash.
 */
static void add_unique_ust_app_event(struct ust_app_channel *ua_chan,
		struct ust_app_event *event)
{
	struct cds_lfht_node *node_ptr;
	struct ust_app_ht_key key;
	struct lttng_ht *ht;

	assert(ua_chan);
	assert(ua_chan->events);
	assert(event);

	ht = ua_chan->events;
	key.name = event->attr.name;
	key.filter = event->filter;
	key.loglevel_type = event->attr.loglevel;
	key.exclusion = event->exclusion;

	node_ptr = cds_lfht_add_unique(ht->ht,
			ht->hash_fct(event->node.key, lttng_ht_seed),
			ht_match_ust_app_event, &key, &event->node.node);
	assert(node_ptr == &event->node.node);
}

/*
 * Close the notify socket from the given RCU head object. This MUST be called
 * through a call_rcu().
 */
static void close_notify_sock_rcu(struct rcu_head *head)
{
	int ret;
	struct ust_app_notify_sock_obj *obj =
		caa_container_of(head, struct ust_app_notify_sock_obj, head);

	/* Must have a valid fd here. */
	assert(obj->fd >= 0);

	ret = close(obj->fd);
	if (ret) {
		ERR("close notify sock %d RCU", obj->fd);
	}
	lttng_fd_put(LTTNG_FD_APPS, 1);

	free(obj);
}

/*
 * Return the session registry according to the buffer type of the given
 * session.
 *
 * A registry per UID object MUST exists before calling this function or else
 * it assert() if not found. RCU read side lock must be acquired.
 */
static struct ust_registry_session *get_session_registry(
		struct ust_app_session *ua_sess)
{
	struct ust_registry_session *registry = NULL;

	assert(ua_sess);

	switch (ua_sess->buffer_type) {
	case LTTNG_BUFFER_PER_PID:
	{
		struct buffer_reg_pid *reg_pid = buffer_reg_pid_find(ua_sess->id);
		if (!reg_pid) {
			goto error;
		}
		registry = reg_pid->registry->reg.ust;
		break;
	}
	case LTTNG_BUFFER_PER_UID:
	{
		struct buffer_reg_uid *reg_uid = buffer_reg_uid_find(
				ua_sess->tracing_id, ua_sess->bits_per_long,
				lttng_credentials_get_uid(&ua_sess->real_credentials));
		if (!reg_uid) {
			goto error;
		}
		registry = reg_uid->registry->reg.ust;
		break;
	}
	default:
		assert(0);
	};

error:
	return registry;
}

/*
 * Delete ust context safely. RCU read lock must be held before calling
 * this function.
 */
static
void delete_ust_app_ctx(int sock, struct ust_app_ctx *ua_ctx,
		struct ust_app *app)
{
	int ret;

	assert(ua_ctx);

	if (ua_ctx->obj) {
		pthread_mutex_lock(&app->sock_lock);
		ret = lttng_ust_ctl_release_object(sock, ua_ctx->obj);
		pthread_mutex_unlock(&app->sock_lock);
		if (ret < 0) {
			if (ret == -EPIPE || ret == -LTTNG_UST_ERR_EXITING) {
				DBG3("UST app release ctx failed. Application is dead: pid = %d, sock = %d",
						app->pid, app->sock);
			} else if (ret == -EAGAIN) {
				WARN("UST app release ctx failed. Communication time out: pid = %d, sock = %d",
						app->pid, app->sock);
			} else {
				ERR("UST app release ctx obj handle %d failed with ret %d: pid = %d, sock = %d",
						ua_ctx->obj->handle, ret,
						app->pid, app->sock);
			}
		}
		free(ua_ctx->obj);
	}
	free(ua_ctx);
}

/*
 * Delete ust app event safely. RCU read lock must be held before calling
 * this function.
 */
static
void delete_ust_app_event(int sock, struct ust_app_event *ua_event,
		struct ust_app *app)
{
	int ret;

	assert(ua_event);

	free(ua_event->filter);
	if (ua_event->exclusion != NULL)
		free(ua_event->exclusion);
	if (ua_event->obj != NULL) {
		pthread_mutex_lock(&app->sock_lock);
		ret = lttng_ust_ctl_release_object(sock, ua_event->obj);
		pthread_mutex_unlock(&app->sock_lock);
		if (ret < 0) {
			if (ret == -EPIPE || ret == -LTTNG_UST_ERR_EXITING) {
				DBG3("UST app release event failed. Application is dead: pid = %d, sock = %d",
						app->pid, app->sock);
			} else if (ret == -EAGAIN) {
				WARN("UST app release event failed. Communication time out: pid = %d, sock = %d",
						app->pid, app->sock);
			} else {
				ERR("UST app release event obj failed with ret %d: pid = %d, sock = %d",
						ret, app->pid, app->sock);
			}
		}
		free(ua_event->obj);
	}
	free(ua_event);
}

/*
 * Delayed reclaim of a ust_app_event_notifier_rule object. This MUST be called
 * through a call_rcu().
 */
static
void free_ust_app_event_notifier_rule_rcu(struct rcu_head *head)
{
	struct ust_app_event_notifier_rule *obj = caa_container_of(
			head, struct ust_app_event_notifier_rule, rcu_head);

	free(obj);
}

/*
 * Delete ust app event notifier rule safely.
 */
static void delete_ust_app_event_notifier_rule(int sock,
		struct ust_app_event_notifier_rule *ua_event_notifier_rule,
		struct ust_app *app)
{
	int ret;

	assert(ua_event_notifier_rule);

	if (ua_event_notifier_rule->exclusion != NULL) {
		free(ua_event_notifier_rule->exclusion);
	}

	if (ua_event_notifier_rule->obj != NULL) {
		pthread_mutex_lock(&app->sock_lock);
		ret = lttng_ust_ctl_release_object(sock, ua_event_notifier_rule->obj);
		pthread_mutex_unlock(&app->sock_lock);
		if (ret < 0) {
			if (ret == -EPIPE || ret == -LTTNG_UST_ERR_EXITING) {
				DBG3("UST app release event notifier failed. Application is dead: pid = %d, sock = %d",
						app->pid, app->sock);
			} else if (ret == -EAGAIN) {
				WARN("UST app release event notifier failed. Communication time out: pid = %d, sock = %d",
						app->pid, app->sock);
			} else {
				ERR("UST app release event notifier failed with ret %d: pid = %d, sock = %d",
						ret, app->pid, app->sock);
			}
		}

		free(ua_event_notifier_rule->obj);
	}

	lttng_trigger_put(ua_event_notifier_rule->trigger);
	call_rcu(&ua_event_notifier_rule->rcu_head,
			free_ust_app_event_notifier_rule_rcu);
}

/*
 * Release ust data object of the given stream.
 *
 * Return 0 on success or else a negative value.
 */
static int release_ust_app_stream(int sock, struct ust_app_stream *stream,
		struct ust_app *app)
{
	int ret = 0;

	assert(stream);

	if (stream->obj) {
		pthread_mutex_lock(&app->sock_lock);
		ret = lttng_ust_ctl_release_object(sock, stream->obj);
		pthread_mutex_unlock(&app->sock_lock);
		if (ret < 0) {
			if (ret == -EPIPE || ret == -LTTNG_UST_ERR_EXITING) {
				DBG3("UST app release stream failed. Application is dead: pid = %d, sock = %d",
						app->pid, app->sock);
			} else if (ret == -EAGAIN) {
				WARN("UST app release stream failed. Communication time out: pid = %d, sock = %d",
						app->pid, app->sock);
			} else {
				ERR("UST app release stream obj failed with ret %d: pid = %d, sock = %d",
						ret, app->pid, app->sock);
			}
		}
		lttng_fd_put(LTTNG_FD_APPS, 2);
		free(stream->obj);
	}

	return ret;
}

/*
 * Delete ust app stream safely. RCU read lock must be held before calling
 * this function.
 */
static
void delete_ust_app_stream(int sock, struct ust_app_stream *stream,
		struct ust_app *app)
{
	assert(stream);

	(void) release_ust_app_stream(sock, stream, app);
	free(stream);
}

/*
 * We need to execute ht_destroy outside of RCU read-side critical
 * section and outside of call_rcu thread, so we postpone its execution
 * using ht_cleanup_push. It is simpler than to change the semantic of
 * the many callers of delete_ust_app_session().
 */
static
void delete_ust_app_channel_rcu(struct rcu_head *head)
{
	struct ust_app_channel *ua_chan =
		caa_container_of(head, struct ust_app_channel, rcu_head);

	ht_cleanup_push(ua_chan->ctx);
	ht_cleanup_push(ua_chan->events);
	free(ua_chan);
}

/*
 * Extract the lost packet or discarded events counter when the channel is
 * being deleted and store the value in the parent channel so we can
 * access it from lttng list and at stop/destroy.
 *
 * The session list lock must be held by the caller.
 */
static
void save_per_pid_lost_discarded_counters(struct ust_app_channel *ua_chan)
{
	uint64_t discarded = 0, lost = 0;
	struct ltt_session *session;
	struct ltt_ust_channel *uchan;

	if (ua_chan->attr.type != LTTNG_UST_ABI_CHAN_PER_CPU) {
		return;
	}

	rcu_read_lock();
	session = session_find_by_id(ua_chan->session->tracing_id);
	if (!session || !session->ust_session) {
		/*
		 * Not finding the session is not an error because there are
		 * multiple ways the channels can be torn down.
		 *
		 * 1) The session daemon can initiate the destruction of the
		 *    ust app session after receiving a destroy command or
		 *    during its shutdown/teardown.
		 * 2) The application, since we are in per-pid tracing, is
		 *    unregistering and tearing down its ust app session.
		 *
		 * Both paths are protected by the session list lock which
		 * ensures that the accounting of lost packets and discarded
		 * events is done exactly once. The session is then unpublished
		 * from the session list, resulting in this condition.
		 */
		goto end;
	}

	if (ua_chan->attr.overwrite) {
		consumer_get_lost_packets(ua_chan->session->tracing_id,
				ua_chan->key, session->ust_session->consumer,
				&lost);
	} else {
		consumer_get_discarded_events(ua_chan->session->tracing_id,
				ua_chan->key, session->ust_session->consumer,
				&discarded);
	}
	uchan = trace_ust_find_channel_by_name(
			session->ust_session->domain_global.channels,
			ua_chan->name);
	if (!uchan) {
		ERR("Missing UST channel to store discarded counters");
		goto end;
	}

	uchan->per_pid_closed_app_discarded += discarded;
	uchan->per_pid_closed_app_lost += lost;

end:
	rcu_read_unlock();
	if (session) {
		session_put(session);
	}
}

/*
 * Delete ust app channel safely. RCU read lock must be held before calling
 * this function.
 *
 * The session list lock must be held by the caller.
 */
static
void delete_ust_app_channel(int sock, struct ust_app_channel *ua_chan,
		struct ust_app *app)
{
	int ret;
	struct lttng_ht_iter iter;
	struct ust_app_event *ua_event;
	struct ust_app_ctx *ua_ctx;
	struct ust_app_stream *stream, *stmp;
	struct ust_registry_session *registry;

	assert(ua_chan);

	DBG3("UST app deleting channel %s", ua_chan->name);

	/* Wipe stream */
	cds_list_for_each_entry_safe(stream, stmp, &ua_chan->streams.head, list) {
		cds_list_del(&stream->list);
		delete_ust_app_stream(sock, stream, app);
	}

	/* Wipe context */
	cds_lfht_for_each_entry(ua_chan->ctx->ht, &iter.iter, ua_ctx, node.node) {
		cds_list_del(&ua_ctx->list);
		ret = lttng_ht_del(ua_chan->ctx, &iter);
		assert(!ret);
		delete_ust_app_ctx(sock, ua_ctx, app);
	}

	/* Wipe events */
	cds_lfht_for_each_entry(ua_chan->events->ht, &iter.iter, ua_event,
			node.node) {
		ret = lttng_ht_del(ua_chan->events, &iter);
		assert(!ret);
		delete_ust_app_event(sock, ua_event, app);
	}

	if (ua_chan->session->buffer_type == LTTNG_BUFFER_PER_PID) {
		/* Wipe and free registry from session registry. */
		registry = get_session_registry(ua_chan->session);
		if (registry) {
			ust_registry_channel_del_free(registry, ua_chan->key,
				sock >= 0);
		}
		/*
		 * A negative socket can be used by the caller when
		 * cleaning-up a ua_chan in an error path. Skip the
		 * accounting in this case.
		 */
		if (sock >= 0) {
			save_per_pid_lost_discarded_counters(ua_chan);
		}
	}

	if (ua_chan->obj != NULL) {
		/* Remove channel from application UST object descriptor. */
		iter.iter.node = &ua_chan->ust_objd_node.node;
		ret = lttng_ht_del(app->ust_objd, &iter);
		assert(!ret);
		pthread_mutex_lock(&app->sock_lock);
		ret = lttng_ust_ctl_release_object(sock, ua_chan->obj);
		pthread_mutex_unlock(&app->sock_lock);
		if (ret < 0) {
			if (ret == -EPIPE || ret == -LTTNG_UST_ERR_EXITING) {
				DBG3("UST app channel %s release failed. Application is dead: pid = %d, sock = %d",
						ua_chan->name, app->pid,
						app->sock);
			} else if (ret == -EAGAIN) {
				WARN("UST app channel %s release failed. Communication time out: pid = %d, sock = %d",
						ua_chan->name, app->pid,
						app->sock);
			} else {
				ERR("UST app channel %s release failed with ret %d: pid = %d, sock = %d",
						ua_chan->name, ret, app->pid,
						app->sock);
			}
		}
		lttng_fd_put(LTTNG_FD_APPS, 1);
		free(ua_chan->obj);
	}
	call_rcu(&ua_chan->rcu_head, delete_ust_app_channel_rcu);
}

int ust_app_register_done(struct ust_app *app)
{
	int ret;

	pthread_mutex_lock(&app->sock_lock);
	ret = lttng_ust_ctl_register_done(app->sock);
	pthread_mutex_unlock(&app->sock_lock);
	return ret;
}

int ust_app_release_object(struct ust_app *app, struct lttng_ust_abi_object_data *data)
{
	int ret, sock;

	if (app) {
		pthread_mutex_lock(&app->sock_lock);
		sock = app->sock;
	} else {
		sock = -1;
	}
	ret = lttng_ust_ctl_release_object(sock, data);
	if (app) {
		pthread_mutex_unlock(&app->sock_lock);
	}
	return ret;
}

/*
 * Push metadata to consumer socket.
 *
 * RCU read-side lock must be held to guarantee existance of socket.
 * Must be called with the ust app session lock held.
 * Must be called with the registry lock held.
 *
 * On success, return the len of metadata pushed or else a negative value.
 * Returning a -EPIPE return value means we could not send the metadata,
 * but it can be caused by recoverable errors (e.g. the application has
 * terminated concurrently).
 */
ssize_t ust_app_push_metadata(struct ust_registry_session *registry,
		struct consumer_socket *socket, int send_zero_data)
{
	int ret;
	char *metadata_str = NULL;
	size_t len, offset, new_metadata_len_sent;
	ssize_t ret_val;
	uint64_t metadata_key, metadata_version;

	assert(registry);
	assert(socket);

	metadata_key = registry->metadata_key;

	/*
	 * Means that no metadata was assigned to the session. This can
	 * happens if no start has been done previously.
	 */
	if (!metadata_key) {
		return 0;
	}

	offset = registry->metadata_len_sent;
	len = registry->metadata_len - registry->metadata_len_sent;
	new_metadata_len_sent = registry->metadata_len;
	metadata_version = registry->metadata_version;
	if (len == 0) {
		DBG3("No metadata to push for metadata key %" PRIu64,
				registry->metadata_key);
		ret_val = len;
		if (send_zero_data) {
			DBG("No metadata to push");
			goto push_data;
		}
		goto end;
	}

	/* Allocate only what we have to send. */
	metadata_str = zmalloc(len);
	if (!metadata_str) {
		PERROR("zmalloc ust app metadata string");
		ret_val = -ENOMEM;
		goto error;
	}
	/* Copy what we haven't sent out. */
	memcpy(metadata_str, registry->metadata + offset, len);

push_data:
	pthread_mutex_unlock(&registry->lock);
	/*
	 * We need to unlock the registry while we push metadata to
	 * break a circular dependency between the consumerd metadata
	 * lock and the sessiond registry lock. Indeed, pushing metadata
	 * to the consumerd awaits that it gets pushed all the way to
	 * relayd, but doing so requires grabbing the metadata lock. If
	 * a concurrent metadata request is being performed by
	 * consumerd, this can try to grab the registry lock on the
	 * sessiond while holding the metadata lock on the consumer
	 * daemon. Those push and pull schemes are performed on two
	 * different bidirectionnal communication sockets.
	 */
	ret = consumer_push_metadata(socket, metadata_key,
			metadata_str, len, offset, metadata_version);
	pthread_mutex_lock(&registry->lock);
	if (ret < 0) {
		/*
		 * There is an acceptable race here between the registry
		 * metadata key assignment and the creation on the
		 * consumer. The session daemon can concurrently push
		 * metadata for this registry while being created on the
		 * consumer since the metadata key of the registry is
		 * assigned *before* it is setup to avoid the consumer
		 * to ask for metadata that could possibly be not found
		 * in the session daemon.
		 *
		 * The metadata will get pushed either by the session
		 * being stopped or the consumer requesting metadata if
		 * that race is triggered.
		 */
		if (ret == -LTTCOMM_CONSUMERD_CHANNEL_FAIL) {
			ret = 0;
		} else {
			ERR("Error pushing metadata to consumer");
		}
		ret_val = ret;
		goto error_push;
	} else {
		/*
		 * Metadata may have been concurrently pushed, since
		 * we're not holding the registry lock while pushing to
		 * consumer.  This is handled by the fact that we send
		 * the metadata content, size, and the offset at which
		 * that metadata belongs. This may arrive out of order
		 * on the consumer side, and the consumer is able to
		 * deal with overlapping fragments. The consumer
		 * supports overlapping fragments, which must be
		 * contiguous starting from offset 0. We keep the
		 * largest metadata_len_sent value of the concurrent
		 * send.
		 */
		registry->metadata_len_sent =
			max_t(size_t, registry->metadata_len_sent,
				new_metadata_len_sent);
	}
	free(metadata_str);
	return len;

end:
error:
	if (ret_val) {
		/*
		 * On error, flag the registry that the metadata is
		 * closed. We were unable to push anything and this
		 * means that either the consumer is not responding or
		 * the metadata cache has been destroyed on the
		 * consumer.
		 */
		registry->metadata_closed = 1;
	}
error_push:
	free(metadata_str);
	return ret_val;
}

/*
 * For a given application and session, push metadata to consumer.
 * Either sock or consumer is required : if sock is NULL, the default
 * socket to send the metadata is retrieved from consumer, if sock
 * is not NULL we use it to send the metadata.
 * RCU read-side lock must be held while calling this function,
 * therefore ensuring existance of registry. It also ensures existance
 * of socket throughout this function.
 *
 * Return 0 on success else a negative error.
 * Returning a -EPIPE return value means we could not send the metadata,
 * but it can be caused by recoverable errors (e.g. the application has
 * terminated concurrently).
 */
static int push_metadata(struct ust_registry_session *registry,
		struct consumer_output *consumer)
{
	int ret_val;
	ssize_t ret;
	struct consumer_socket *socket;

	assert(registry);
	assert(consumer);

	pthread_mutex_lock(&registry->lock);
	if (registry->metadata_closed) {
		ret_val = -EPIPE;
		goto error;
	}

	/* Get consumer socket to use to push the metadata.*/
	socket = consumer_find_socket_by_bitness(registry->bits_per_long,
			consumer);
	if (!socket) {
		ret_val = -1;
		goto error;
	}

	ret = ust_app_push_metadata(registry, socket, 0);
	if (ret < 0) {
		ret_val = ret;
		goto error;
	}
	pthread_mutex_unlock(&registry->lock);
	return 0;

error:
	pthread_mutex_unlock(&registry->lock);
	return ret_val;
}

/*
 * Send to the consumer a close metadata command for the given session. Once
 * done, the metadata channel is deleted and the session metadata pointer is
 * nullified. The session lock MUST be held unless the application is
 * in the destroy path.
 *
 * Do not hold the registry lock while communicating with the consumerd, because
 * doing so causes inter-process deadlocks between consumerd and sessiond with
 * the metadata request notification.
 *
 * Return 0 on success else a negative value.
 */
static int close_metadata(struct ust_registry_session *registry,
		struct consumer_output *consumer)
{
	int ret;
	struct consumer_socket *socket;
	uint64_t metadata_key;
	bool registry_was_already_closed;

	assert(registry);
	assert(consumer);

	rcu_read_lock();

	pthread_mutex_lock(&registry->lock);
	metadata_key = registry->metadata_key;
	registry_was_already_closed = registry->metadata_closed;
	if (metadata_key != 0) {
		/*
		 * Metadata closed. Even on error this means that the consumer
		 * is not responding or not found so either way a second close
		 * should NOT be emit for this registry.
		 */
		registry->metadata_closed = 1;
	}
	pthread_mutex_unlock(&registry->lock);

	if (metadata_key == 0 || registry_was_already_closed) {
		ret = 0;
		goto end;
	}

	/* Get consumer socket to use to push the metadata.*/
	socket = consumer_find_socket_by_bitness(registry->bits_per_long,
			consumer);
	if (!socket) {
		ret = -1;
		goto end;
	}

	ret = consumer_close_metadata(socket, metadata_key);
	if (ret < 0) {
		goto end;
	}

end:
	rcu_read_unlock();
	return ret;
}

/*
 * We need to execute ht_destroy outside of RCU read-side critical
 * section and outside of call_rcu thread, so we postpone its execution
 * using ht_cleanup_push. It is simpler than to change the semantic of
 * the many callers of delete_ust_app_session().
 */
static
void delete_ust_app_session_rcu(struct rcu_head *head)
{
	struct ust_app_session *ua_sess =
		caa_container_of(head, struct ust_app_session, rcu_head);

	ht_cleanup_push(ua_sess->channels);
	free(ua_sess);
}

/*
 * Delete ust app session safely. RCU read lock must be held before calling
 * this function.
 *
 * The session list lock must be held by the caller.
 */
static
void delete_ust_app_session(int sock, struct ust_app_session *ua_sess,
		struct ust_app *app)
{
	int ret;
	struct lttng_ht_iter iter;
	struct ust_app_channel *ua_chan;
	struct ust_registry_session *registry;

	assert(ua_sess);

	pthread_mutex_lock(&ua_sess->lock);

	assert(!ua_sess->deleted);
	ua_sess->deleted = true;

	registry = get_session_registry(ua_sess);
	/* Registry can be null on error path during initialization. */
	if (registry) {
		/* Push metadata for application before freeing the application. */
		(void) push_metadata(registry, ua_sess->consumer);

		/*
		 * Don't ask to close metadata for global per UID buffers. Close
		 * metadata only on destroy trace session in this case. Also, the
		 * previous push metadata could have flag the metadata registry to
		 * close so don't send a close command if closed.
		 */
		if (ua_sess->buffer_type != LTTNG_BUFFER_PER_UID) {
			/* And ask to close it for this session registry. */
			(void) close_metadata(registry, ua_sess->consumer);
		}
	}

	cds_lfht_for_each_entry(ua_sess->channels->ht, &iter.iter, ua_chan,
			node.node) {
		ret = lttng_ht_del(ua_sess->channels, &iter);
		assert(!ret);
		delete_ust_app_channel(sock, ua_chan, app);
	}

	/* In case of per PID, the registry is kept in the session. */
	if (ua_sess->buffer_type == LTTNG_BUFFER_PER_PID) {
		struct buffer_reg_pid *reg_pid = buffer_reg_pid_find(ua_sess->id);
		if (reg_pid) {
			/*
			 * Registry can be null on error path during
			 * initialization.
			 */
			buffer_reg_pid_remove(reg_pid);
			buffer_reg_pid_destroy(reg_pid);
		}
	}

	if (ua_sess->handle != -1) {
		pthread_mutex_lock(&app->sock_lock);
		ret = lttng_ust_ctl_release_handle(sock, ua_sess->handle);
		pthread_mutex_unlock(&app->sock_lock);
		if (ret < 0) {
			if (ret == -EPIPE || ret == -LTTNG_UST_ERR_EXITING) {
				DBG3("UST app release session handle failed. Application is dead: pid = %d, sock = %d",
						app->pid, app->sock);
			} else if (ret == -EAGAIN) {
				WARN("UST app release session handle failed. Communication time out: pid = %d, sock = %d",
						app->pid, app->sock);
			} else {
				ERR("UST app release session handle failed with ret %d: pid = %d, sock = %d",
						ret, app->pid, app->sock);
			}
		}

		/* Remove session from application UST object descriptor. */
		iter.iter.node = &ua_sess->ust_objd_node.node;
		ret = lttng_ht_del(app->ust_sessions_objd, &iter);
		assert(!ret);
	}

	pthread_mutex_unlock(&ua_sess->lock);

	consumer_output_put(ua_sess->consumer);

	call_rcu(&ua_sess->rcu_head, delete_ust_app_session_rcu);
}

/*
 * Delete a traceable application structure from the global list. Never call
 * this function outside of a call_rcu call.
 *
 * RCU read side lock should _NOT_ be held when calling this function.
 */
static
void delete_ust_app(struct ust_app *app)
{
	int ret, sock;
	struct ust_app_session *ua_sess, *tmp_ua_sess;
	struct lttng_ht_iter iter;
	struct ust_app_event_notifier_rule *event_notifier_rule;
	bool event_notifier_write_fd_is_open;

	/*
	 * The session list lock must be held during this function to guarantee
	 * the existence of ua_sess.
	 */
	session_lock_list();
	/* Delete ust app sessions info */
	sock = app->sock;
	app->sock = -1;

	/* Wipe sessions */
	cds_list_for_each_entry_safe(ua_sess, tmp_ua_sess, &app->teardown_head,
			teardown_node) {
		/* Free every object in the session and the session. */
		rcu_read_lock();
		delete_ust_app_session(sock, ua_sess, app);
		rcu_read_unlock();
	}

	/* Remove the event notifier rules associated with this app. */
	rcu_read_lock();
	cds_lfht_for_each_entry (app->token_to_event_notifier_rule_ht->ht,
			&iter.iter, event_notifier_rule, node.node) {
		ret = lttng_ht_del(app->token_to_event_notifier_rule_ht, &iter);
		assert(!ret);

		delete_ust_app_event_notifier_rule(
				app->sock, event_notifier_rule, app);
	}

	rcu_read_unlock();

	ht_cleanup_push(app->sessions);
	ht_cleanup_push(app->ust_sessions_objd);
	ht_cleanup_push(app->ust_objd);
	ht_cleanup_push(app->token_to_event_notifier_rule_ht);

	/*
	 * This could be NULL if the event notifier setup failed (e.g the app
	 * was killed or the tracer does not support this feature).
	 */
	if (app->event_notifier_group.object) {
		enum lttng_error_code ret_code;
		enum event_notifier_error_accounting_status status;

		const int event_notifier_read_fd = lttng_pipe_get_readfd(
				app->event_notifier_group.event_pipe);

		ret_code = notification_thread_command_remove_tracer_event_source(
				the_notification_thread_handle,
				event_notifier_read_fd);
		if (ret_code != LTTNG_OK) {
			ERR("Failed to remove application tracer event source from notification thread");
		}

		status = event_notifier_error_accounting_unregister_app(app);
		if (status != EVENT_NOTIFIER_ERROR_ACCOUNTING_STATUS_OK) {
			ERR("Error unregistering app from event notifier error accounting");
		}

		lttng_ust_ctl_release_object(sock, app->event_notifier_group.object);
		free(app->event_notifier_group.object);
	}

	event_notifier_write_fd_is_open = lttng_pipe_is_write_open(
			app->event_notifier_group.event_pipe);
	lttng_pipe_destroy(app->event_notifier_group.event_pipe);
	/*
	 * Release the file descriptors reserved for the event notifier pipe.
	 * The app could be destroyed before the write end of the pipe could be
	 * passed to the application (and closed). In that case, both file
	 * descriptors must be released.
	 */
	lttng_fd_put(LTTNG_FD_APPS, event_notifier_write_fd_is_open ? 2 : 1);

	/*
	 * Wait until we have deleted the application from the sock hash table
	 * before closing this socket, otherwise an application could re-use the
	 * socket ID and race with the teardown, using the same hash table entry.
	 *
	 * It's OK to leave the close in call_rcu. We want it to stay unique for
	 * all RCU readers that could run concurrently with unregister app,
	 * therefore we _need_ to only close that socket after a grace period. So
	 * it should stay in this RCU callback.
	 *
	 * This close() is a very important step of the synchronization model so
	 * every modification to this function must be carefully reviewed.
	 */
	ret = close(sock);
	if (ret) {
		PERROR("close");
	}
	lttng_fd_put(LTTNG_FD_APPS, 1);

	DBG2("UST app pid %d deleted", app->pid);
	free(app);
	session_unlock_list();
}

/*
 * URCU intermediate call to delete an UST app.
 */
static
void delete_ust_app_rcu(struct rcu_head *head)
{
	struct lttng_ht_node_ulong *node =
		caa_container_of(head, struct lttng_ht_node_ulong, head);
	struct ust_app *app =
		caa_container_of(node, struct ust_app, pid_n);

	DBG3("Call RCU deleting app PID %d", app->pid);
	delete_ust_app(app);
}

/*
 * Delete the session from the application ht and delete the data structure by
 * freeing every object inside and releasing them.
 *
 * The session list lock must be held by the caller.
 */
static void destroy_app_session(struct ust_app *app,
		struct ust_app_session *ua_sess)
{
	int ret;
	struct lttng_ht_iter iter;

	assert(app);
	assert(ua_sess);

	iter.iter.node = &ua_sess->node.node;
	ret = lttng_ht_del(app->sessions, &iter);
	if (ret) {
		/* Already scheduled for teardown. */
		goto end;
	}

	/* Once deleted, free the data structure. */
	delete_ust_app_session(app->sock, ua_sess, app);

end:
	return;
}

/*
 * Alloc new UST app session.
 */
static
struct ust_app_session *alloc_ust_app_session(void)
{
	struct ust_app_session *ua_sess;

	/* Init most of the default value by allocating and zeroing */
	ua_sess = zmalloc(sizeof(struct ust_app_session));
	if (ua_sess == NULL) {
		PERROR("malloc");
		goto error_free;
	}

	ua_sess->handle = -1;
	ua_sess->channels = lttng_ht_new(0, LTTNG_HT_TYPE_STRING);
	ua_sess->metadata_attr.type = LTTNG_UST_ABI_CHAN_METADATA;
	pthread_mutex_init(&ua_sess->lock, NULL);

	return ua_sess;

error_free:
	return NULL;
}

/*
 * Alloc new UST app channel.
 */
static
struct ust_app_channel *alloc_ust_app_channel(const char *name,
		struct ust_app_session *ua_sess,
		struct lttng_ust_abi_channel_attr *attr)
{
	struct ust_app_channel *ua_chan;

	/* Init most of the default value by allocating and zeroing */
	ua_chan = zmalloc(sizeof(struct ust_app_channel));
	if (ua_chan == NULL) {
		PERROR("malloc");
		goto error;
	}

	/* Setup channel name */
	strncpy(ua_chan->name, name, sizeof(ua_chan->name));
	ua_chan->name[sizeof(ua_chan->name) - 1] = '\0';

	ua_chan->enabled = 1;
	ua_chan->handle = -1;
	ua_chan->session = ua_sess;
	ua_chan->key = get_next_channel_key();
	ua_chan->ctx = lttng_ht_new(0, LTTNG_HT_TYPE_ULONG);
	ua_chan->events = lttng_ht_new(0, LTTNG_HT_TYPE_STRING);
	lttng_ht_node_init_str(&ua_chan->node, ua_chan->name);

	CDS_INIT_LIST_HEAD(&ua_chan->streams.head);
	CDS_INIT_LIST_HEAD(&ua_chan->ctx_list);

	/* Copy attributes */
	if (attr) {
		/* Translate from lttng_ust_channel to lttng_ust_ctl_consumer_channel_attr. */
		ua_chan->attr.subbuf_size = attr->subbuf_size;
		ua_chan->attr.num_subbuf = attr->num_subbuf;
		ua_chan->attr.overwrite = attr->overwrite;
		ua_chan->attr.switch_timer_interval = attr->switch_timer_interval;
		ua_chan->attr.read_timer_interval = attr->read_timer_interval;
		ua_chan->attr.output = attr->output;
		ua_chan->attr.blocking_timeout = attr->u.s.blocking_timeout;
	}
	/* By default, the channel is a per cpu channel. */
	ua_chan->attr.type = LTTNG_UST_ABI_CHAN_PER_CPU;

	DBG3("UST app channel %s allocated", ua_chan->name);

	return ua_chan;

error:
	return NULL;
}

/*
 * Allocate and initialize a UST app stream.
 *
 * Return newly allocated stream pointer or NULL on error.
 */
struct ust_app_stream *ust_app_alloc_stream(void)
{
	struct ust_app_stream *stream = NULL;

	stream = zmalloc(sizeof(*stream));
	if (stream == NULL) {
		PERROR("zmalloc ust app stream");
		goto error;
	}

	/* Zero could be a valid value for a handle so flag it to -1. */
	stream->handle = -1;

error:
	return stream;
}

/*
 * Alloc new UST app event.
 */
static
struct ust_app_event *alloc_ust_app_event(char *name,
		struct lttng_ust_abi_event *attr)
{
	struct ust_app_event *ua_event;

	/* Init most of the default value by allocating and zeroing */
	ua_event = zmalloc(sizeof(struct ust_app_event));
	if (ua_event == NULL) {
		PERROR("Failed to allocate ust_app_event structure");
		goto error;
	}

	ua_event->enabled = 1;
	strncpy(ua_event->name, name, sizeof(ua_event->name));
	ua_event->name[sizeof(ua_event->name) - 1] = '\0';
	lttng_ht_node_init_str(&ua_event->node, ua_event->name);

	/* Copy attributes */
	if (attr) {
		memcpy(&ua_event->attr, attr, sizeof(ua_event->attr));
	}

	DBG3("UST app event %s allocated", ua_event->name);

	return ua_event;

error:
	return NULL;
}

/*
 * Allocate a new UST app event notifier rule.
 */
static struct ust_app_event_notifier_rule *alloc_ust_app_event_notifier_rule(
		struct lttng_trigger *trigger)
{
	enum lttng_event_rule_generate_exclusions_status
			generate_exclusion_status;
	enum lttng_condition_status cond_status;
	struct ust_app_event_notifier_rule *ua_event_notifier_rule;
	struct lttng_condition *condition = NULL;
	const struct lttng_event_rule *event_rule = NULL;

	ua_event_notifier_rule = zmalloc(sizeof(struct ust_app_event_notifier_rule));
	if (ua_event_notifier_rule == NULL) {
		PERROR("Failed to allocate ust_app_event_notifier_rule structure");
		goto error;
	}

	ua_event_notifier_rule->enabled = 1;
	ua_event_notifier_rule->token = lttng_trigger_get_tracer_token(trigger);
	lttng_ht_node_init_u64(&ua_event_notifier_rule->node,
			ua_event_notifier_rule->token);

	condition = lttng_trigger_get_condition(trigger);
	assert(condition);
	assert(lttng_condition_get_type(condition) ==
			LTTNG_CONDITION_TYPE_EVENT_RULE_MATCHES);

	cond_status = lttng_condition_event_rule_matches_get_rule(
			condition, &event_rule);
	assert(cond_status == LTTNG_CONDITION_STATUS_OK);
	assert(event_rule);

	ua_event_notifier_rule->error_counter_index =
			lttng_condition_event_rule_matches_get_error_counter_index(condition);
	/* Acquire the event notifier's reference to the trigger. */
	lttng_trigger_get(trigger);

	ua_event_notifier_rule->trigger = trigger;
	ua_event_notifier_rule->filter = lttng_event_rule_get_filter_bytecode(event_rule);
	generate_exclusion_status = lttng_event_rule_generate_exclusions(
			event_rule, &ua_event_notifier_rule->exclusion);
	switch (generate_exclusion_status) {
	case LTTNG_EVENT_RULE_GENERATE_EXCLUSIONS_STATUS_OK:
	case LTTNG_EVENT_RULE_GENERATE_EXCLUSIONS_STATUS_NONE:
		break;
	default:
		/* Error occurred. */
		ERR("Failed to generate exclusions from trigger while allocating an event notifier rule");
		goto error_put_trigger;
	}

	DBG3("UST app event notifier rule allocated: token = %" PRIu64,
			ua_event_notifier_rule->token);

	return ua_event_notifier_rule;

error_put_trigger:
	lttng_trigger_put(trigger);
error:
	free(ua_event_notifier_rule);
	return NULL;
}

/*
 * Alloc new UST app context.
 */
static
struct ust_app_ctx *alloc_ust_app_ctx(struct lttng_ust_context_attr *uctx)
{
	struct ust_app_ctx *ua_ctx;

	ua_ctx = zmalloc(sizeof(struct ust_app_ctx));
	if (ua_ctx == NULL) {
		goto error;
	}

	CDS_INIT_LIST_HEAD(&ua_ctx->list);

	if (uctx) {
		memcpy(&ua_ctx->ctx, uctx, sizeof(ua_ctx->ctx));
		if (uctx->ctx == LTTNG_UST_ABI_CONTEXT_APP_CONTEXT) {
			char *provider_name = NULL, *ctx_name = NULL;

			provider_name = strdup(uctx->u.app_ctx.provider_name);
			ctx_name = strdup(uctx->u.app_ctx.ctx_name);
			if (!provider_name || !ctx_name) {
				free(provider_name);
				free(ctx_name);
				goto error;
			}

			ua_ctx->ctx.u.app_ctx.provider_name = provider_name;
			ua_ctx->ctx.u.app_ctx.ctx_name = ctx_name;
		}
	}

	DBG3("UST app context %d allocated", ua_ctx->ctx.ctx);
	return ua_ctx;
error:
	free(ua_ctx);
	return NULL;
}

/*
 * Create a liblttng-ust filter bytecode from given bytecode.
 *
 * Return allocated filter or NULL on error.
 */
static struct lttng_ust_abi_filter_bytecode *create_ust_filter_bytecode_from_bytecode(
		const struct lttng_bytecode *orig_f)
{
	struct lttng_ust_abi_filter_bytecode *filter = NULL;

	/* Copy filter bytecode. */
	filter = zmalloc(sizeof(*filter) + orig_f->len);
	if (!filter) {
		PERROR("Failed to allocate lttng_ust_filter_bytecode: bytecode len = %" PRIu32 " bytes", orig_f->len);
		goto error;
	}

	assert(sizeof(struct lttng_bytecode) ==
			sizeof(struct lttng_ust_abi_filter_bytecode));
	memcpy(filter, orig_f, sizeof(*filter) + orig_f->len);
error:
	return filter;
}

/*
 * Create a liblttng-ust capture bytecode from given bytecode.
 *
 * Return allocated filter or NULL on error.
 */
static struct lttng_ust_abi_capture_bytecode *
create_ust_capture_bytecode_from_bytecode(const struct lttng_bytecode *orig_f)
{
	struct lttng_ust_abi_capture_bytecode *capture = NULL;

	/* Copy capture bytecode. */
	capture = zmalloc(sizeof(*capture) + orig_f->len);
	if (!capture) {
		PERROR("Failed to allocate lttng_ust_abi_capture_bytecode: bytecode len = %" PRIu32 " bytes", orig_f->len);
		goto error;
	}

	assert(sizeof(struct lttng_bytecode) ==
			sizeof(struct lttng_ust_abi_capture_bytecode));
	memcpy(capture, orig_f, sizeof(*capture) + orig_f->len);
error:
	return capture;
}

/*
 * Find an ust_app using the sock and return it. RCU read side lock must be
 * held before calling this helper function.
 */
struct ust_app *ust_app_find_by_sock(int sock)
{
	struct lttng_ht_node_ulong *node;
	struct lttng_ht_iter iter;

	lttng_ht_lookup(ust_app_ht_by_sock, (void *)((unsigned long) sock), &iter);
	node = lttng_ht_iter_get_node_ulong(&iter);
	if (node == NULL) {
		DBG2("UST app find by sock %d not found", sock);
		goto error;
	}

	return caa_container_of(node, struct ust_app, sock_n);

error:
	return NULL;
}

/*
 * Find an ust_app using the notify sock and return it. RCU read side lock must
 * be held before calling this helper function.
 */
static struct ust_app *find_app_by_notify_sock(int sock)
{
	struct lttng_ht_node_ulong *node;
	struct lttng_ht_iter iter;

	lttng_ht_lookup(ust_app_ht_by_notify_sock, (void *)((unsigned long) sock),
			&iter);
	node = lttng_ht_iter_get_node_ulong(&iter);
	if (node == NULL) {
		DBG2("UST app find by notify sock %d not found", sock);
		goto error;
	}

	return caa_container_of(node, struct ust_app, notify_sock_n);

error:
	return NULL;
}

/*
 * Lookup for an ust app event based on event name, filter bytecode and the
 * event loglevel.
 *
 * Return an ust_app_event object or NULL on error.
 */
static struct ust_app_event *find_ust_app_event(struct lttng_ht *ht,
		const char *name, const struct lttng_bytecode *filter,
		int loglevel_value,
		const struct lttng_event_exclusion *exclusion)
{
	struct lttng_ht_iter iter;
	struct lttng_ht_node_str *node;
	struct ust_app_event *event = NULL;
	struct ust_app_ht_key key;

	assert(name);
	assert(ht);

	/* Setup key for event lookup. */
	key.name = name;
	key.filter = filter;
	key.loglevel_type = loglevel_value;
	/* lttng_event_exclusion and lttng_ust_event_exclusion structures are similar */
	key.exclusion = exclusion;

	/* Lookup using the event name as hash and a custom match fct. */
	cds_lfht_lookup(ht->ht, ht->hash_fct((void *) name, lttng_ht_seed),
			ht_match_ust_app_event, &key, &iter.iter);
	node = lttng_ht_iter_get_node_str(&iter);
	if (node == NULL) {
		goto end;
	}

	event = caa_container_of(node, struct ust_app_event, node);

end:
	return event;
}

/*
 * Look-up an event notifier rule based on its token id.
 *
 * Must be called with the RCU read lock held.
 * Return an ust_app_event_notifier_rule object or NULL on error.
 */
static struct ust_app_event_notifier_rule *find_ust_app_event_notifier_rule(
		struct lttng_ht *ht, uint64_t token)
{
	struct lttng_ht_iter iter;
	struct lttng_ht_node_u64 *node;
	struct ust_app_event_notifier_rule *event_notifier_rule = NULL;

	assert(ht);

	lttng_ht_lookup(ht, &token, &iter);
	node = lttng_ht_iter_get_node_u64(&iter);
	if (node == NULL) {
		DBG2("UST app event notifier rule token not found: token = %" PRIu64,
				token);
		goto end;
	}

	event_notifier_rule = caa_container_of(
			node, struct ust_app_event_notifier_rule, node);
end:
	return event_notifier_rule;
}

/*
 * Create the channel context on the tracer.
 *
 * Called with UST app session lock held.
 */
static
int create_ust_channel_context(struct ust_app_channel *ua_chan,
		struct ust_app_ctx *ua_ctx, struct ust_app *app)
{
	int ret;

	health_code_update();

	pthread_mutex_lock(&app->sock_lock);
	ret = lttng_ust_ctl_add_context(app->sock, &ua_ctx->ctx,
			ua_chan->obj, &ua_ctx->obj);
	pthread_mutex_unlock(&app->sock_lock);
	if (ret < 0) {
		if (ret == -EPIPE || ret == -LTTNG_UST_ERR_EXITING) {
			ret = 0;
			DBG3("UST app create channel context failed. Application is dead: pid = %d, sock = %d",
					app->pid, app->sock);
		} else if (ret == -EAGAIN) {
			ret = 0;
			WARN("UST app create channel context failed. Communication time out: pid = %d, sock = %d",
					app->pid, app->sock);
		} else {
			ERR("UST app create channel context failed with ret %d: pid = %d, sock = %d",
					ret, app->pid, app->sock);
		}
		goto error;
	}

	ua_ctx->handle = ua_ctx->obj->handle;

	DBG2("UST app context handle %d created successfully for channel %s",
			ua_ctx->handle, ua_chan->name);

error:
	health_code_update();
	return ret;
}

/*
 * Set the filter on the tracer.
 */
static int set_ust_object_filter(struct ust_app *app,
		const struct lttng_bytecode *bytecode,
		struct lttng_ust_abi_object_data *ust_object)
{
	int ret;
	struct lttng_ust_abi_filter_bytecode *ust_bytecode = NULL;

	health_code_update();

	ust_bytecode = create_ust_filter_bytecode_from_bytecode(bytecode);
	if (!ust_bytecode) {
		ret = -LTTNG_ERR_NOMEM;
		goto error;
	}
	pthread_mutex_lock(&app->sock_lock);
	ret = lttng_ust_ctl_set_filter(app->sock, ust_bytecode,
			ust_object);
	pthread_mutex_unlock(&app->sock_lock);
	if (ret < 0) {
		if (ret == -EPIPE || ret == -LTTNG_UST_ERR_EXITING) {
			ret = 0;
			DBG3("UST app  set filter failed. Application is dead: pid = %d, sock = %d",
					app->pid, app->sock);
		} else if (ret == -EAGAIN) {
			ret = 0;
			WARN("UST app  set filter failed. Communication time out: pid = %d, sock = %d",
					app->pid, app->sock);
		} else {
			ERR("UST app  set filter failed with ret %d: pid = %d, sock = %d, object = %p",
					ret, app->pid, app->sock, ust_object);
		}
		goto error;
	}

	DBG2("UST filter successfully set: object = %p", ust_object);

error:
	health_code_update();
	free(ust_bytecode);
	return ret;
}

/*
 * Set a capture bytecode for the passed object.
 * The sequence number enforces the ordering at runtime and on reception of
 * the captured payloads.
 */
static int set_ust_capture(struct ust_app *app,
		const struct lttng_bytecode *bytecode,
		unsigned int capture_seqnum,
		struct lttng_ust_abi_object_data *ust_object)
{
	int ret;
	struct lttng_ust_abi_capture_bytecode *ust_bytecode = NULL;

	health_code_update();

	ust_bytecode = create_ust_capture_bytecode_from_bytecode(bytecode);
	if (!ust_bytecode) {
		ret = -LTTNG_ERR_NOMEM;
		goto error;
	}

	/*
	 * Set the sequence number to ensure the capture of fields is ordered.
	 */
	ust_bytecode->seqnum = capture_seqnum;

	pthread_mutex_lock(&app->sock_lock);
	ret = lttng_ust_ctl_set_capture(app->sock, ust_bytecode,
			ust_object);
	pthread_mutex_unlock(&app->sock_lock);
	if (ret < 0) {
		if (ret == -EPIPE || ret == -LTTNG_UST_ERR_EXITING) {
			ret = 0;
			DBG3("UST app set capture failed. Application is dead: pid = %d, sock = %d",
					app->pid, app->sock);
		} else if (ret == -EAGAIN) {
			ret = 0;
			DBG3("UST app set capture failed. Communication timeout: pid = %d, sock = %d",
					app->pid, app->sock);
		} else {
			ERR("UST app event set capture failed with ret %d: pid = %d, sock = %d",
					ret, app->pid,
					app->sock);
		}

		goto error;
	}

	DBG2("UST capture successfully set: object = %p", ust_object);

error:
	health_code_update();
	free(ust_bytecode);
	return ret;
}

static
struct lttng_ust_abi_event_exclusion *create_ust_exclusion_from_exclusion(
		const struct lttng_event_exclusion *exclusion)
{
	struct lttng_ust_abi_event_exclusion *ust_exclusion = NULL;
	size_t exclusion_alloc_size = sizeof(struct lttng_ust_abi_event_exclusion) +
		LTTNG_UST_ABI_SYM_NAME_LEN * exclusion->count;

	ust_exclusion = zmalloc(exclusion_alloc_size);
	if (!ust_exclusion) {
		PERROR("malloc");
		goto end;
	}

	assert(sizeof(struct lttng_event_exclusion) ==
			sizeof(struct lttng_ust_abi_event_exclusion));
	memcpy(ust_exclusion, exclusion, exclusion_alloc_size);
end:
	return ust_exclusion;
}

/*
 * Set event exclusions on the tracer.
 */
static int set_ust_object_exclusions(struct ust_app *app,
		const struct lttng_event_exclusion *exclusions,
		struct lttng_ust_abi_object_data *ust_object)
{
	int ret;
	struct lttng_ust_abi_event_exclusion *ust_exclusions = NULL;

	assert(exclusions && exclusions->count > 0);

	health_code_update();

	ust_exclusions = create_ust_exclusion_from_exclusion(
			exclusions);
	if (!ust_exclusions) {
		ret = -LTTNG_ERR_NOMEM;
		goto error;
	}
	pthread_mutex_lock(&app->sock_lock);
	ret = lttng_ust_ctl_set_exclusion(app->sock, ust_exclusions, ust_object);
	pthread_mutex_unlock(&app->sock_lock);
	if (ret < 0) {
		if (ret == -EPIPE || ret == -LTTNG_UST_ERR_EXITING) {
			ret = 0;
			DBG3("UST app event exclusion failed. Application is dead: pid = %d, sock = %d",
					app->pid, app->sock);
		} else if (ret == -EAGAIN) {
			ret = 0;
			WARN("UST app event exclusion failed. Communication time out(pid: %d, sock = %d",
					app->pid, app->sock);
		} else {
			ERR("UST app event exclusions failed with ret %d: pid = %d, sock = %d, object = %p",
					ret, app->pid, app->sock, ust_object);
		}
		goto error;
	}

	DBG2("UST exclusions set successfully for object %p", ust_object);

error:
	health_code_update();
	free(ust_exclusions);
	return ret;
}

/*
 * Disable the specified event on to UST tracer for the UST session.
 */
static int disable_ust_object(struct ust_app *app,
		struct lttng_ust_abi_object_data *object)
{
	int ret;

	health_code_update();

	pthread_mutex_lock(&app->sock_lock);
	ret = lttng_ust_ctl_disable(app->sock, object);
	pthread_mutex_unlock(&app->sock_lock);
	if (ret < 0) {
		if (ret == -EPIPE || ret == -LTTNG_UST_ERR_EXITING) {
			ret = 0;
			DBG3("UST app disable object failed. Application is dead: pid = %d, sock = %d",
					app->pid, app->sock);
		} else if (ret == -EAGAIN) {
			ret = 0;
			WARN("UST app disable object failed. Communication time out: pid = %d, sock = %d",
					app->pid, app->sock);
		} else {
			ERR("UST app disable object failed with ret %d: pid = %d, sock = %d, object = %p",
					ret, app->pid, app->sock, object);
		}
		goto error;
	}

	DBG2("UST app object %p disabled successfully for app: pid = %d",
			object, app->pid);

error:
	health_code_update();
	return ret;
}

/*
 * Disable the specified channel on to UST tracer for the UST session.
 */
static int disable_ust_channel(struct ust_app *app,
		struct ust_app_session *ua_sess, struct ust_app_channel *ua_chan)
{
	int ret;

	health_code_update();

	pthread_mutex_lock(&app->sock_lock);
	ret = lttng_ust_ctl_disable(app->sock, ua_chan->obj);
	pthread_mutex_unlock(&app->sock_lock);
	if (ret < 0) {
		if (ret == -EPIPE || ret == -LTTNG_UST_ERR_EXITING) {
			ret = 0;
			DBG3("UST app disable channel failed. Application is dead: pid = %d, sock = %d",
					app->pid, app->sock);
		} else if (ret == -EAGAIN) {
			ret = 0;
			WARN("UST app disable channel failed. Communication time out: pid = %d, sock = %d",
					app->pid, app->sock);
		} else {
			ERR("UST app channel %s disable failed, session handle %d, with ret %d: pid = %d, sock = %d",
					ua_chan->name, ua_sess->handle, ret,
					app->pid, app->sock);
		}
		goto error;
	}

	DBG2("UST app channel %s disabled successfully for app: pid = %d",
			ua_chan->name, app->pid);

error:
	health_code_update();
	return ret;
}

/*
 * Enable the specified channel on to UST tracer for the UST session.
 */
static int enable_ust_channel(struct ust_app *app,
		struct ust_app_session *ua_sess, struct ust_app_channel *ua_chan)
{
	int ret;

	health_code_update();

	pthread_mutex_lock(&app->sock_lock);
	ret = lttng_ust_ctl_enable(app->sock, ua_chan->obj);
	pthread_mutex_unlock(&app->sock_lock);
	if (ret < 0) {
		if (ret == -EPIPE || ret == -LTTNG_UST_ERR_EXITING) {
			ret = 0;
			DBG3("UST app channel %s enable failed. Application is dead: pid = %d, sock = %d",
					ua_chan->name, app->pid, app->sock);
		} else if (ret == -EAGAIN) {
			ret = 0;
			WARN("UST app channel %s enable failed. Communication time out: pid = %d, sock = %d",
					ua_chan->name, app->pid, app->sock);
		} else {
			ERR("UST app channel %s enable failed, session handle %d, with ret %d: pid = %d, sock = %d",
					ua_chan->name, ua_sess->handle, ret,
					app->pid, app->sock);
		}
		goto error;
	}

	ua_chan->enabled = 1;

	DBG2("UST app channel %s enabled successfully for app: pid = %d",
			ua_chan->name, app->pid);

error:
	health_code_update();
	return ret;
}

/*
 * Enable the specified event on to UST tracer for the UST session.
 */
static int enable_ust_object(
		struct ust_app *app, struct lttng_ust_abi_object_data *ust_object)
{
	int ret;

	health_code_update();

	pthread_mutex_lock(&app->sock_lock);
	ret = lttng_ust_ctl_enable(app->sock, ust_object);
	pthread_mutex_unlock(&app->sock_lock);
	if (ret < 0) {
		if (ret == -EPIPE || ret == -LTTNG_UST_ERR_EXITING) {
			ret = 0;
			DBG3("UST app enable object failed. Application is dead: pid = %d, sock = %d",
					app->pid, app->sock);
		} else if (ret == -EAGAIN) {
			ret = 0;
			WARN("UST app enable object failed. Communication time out: pid = %d, sock = %d",
					app->pid, app->sock);
		} else {
			ERR("UST app enable object failed with ret %d: pid = %d, sock = %d, object = %p",
					ret, app->pid, app->sock, ust_object);
		}
		goto error;
	}

	DBG2("UST app object %p enabled successfully for app: pid = %d",
			ust_object, app->pid);

error:
	health_code_update();
	return ret;
}

/*
 * Send channel and stream buffer to application.
 *
 * Return 0 on success. On error, a negative value is returned.
 */
static int send_channel_pid_to_ust(struct ust_app *app,
		struct ust_app_session *ua_sess, struct ust_app_channel *ua_chan)
{
	int ret;
	struct ust_app_stream *stream, *stmp;

	assert(app);
	assert(ua_sess);
	assert(ua_chan);

	health_code_update();

	DBG("UST app sending channel %s to UST app sock %d", ua_chan->name,
			app->sock);

	/* Send channel to the application. */
	ret = ust_consumer_send_channel_to_ust(app, ua_sess, ua_chan);
	if (ret == -EPIPE || ret == -LTTNG_UST_ERR_EXITING) {
		ret = -ENOTCONN;	/* Caused by app exiting. */
		goto error;
	} else if (ret == -EAGAIN) {
		/* Caused by timeout. */
		WARN("Communication with application %d timed out on send_channel for channel \"%s\" of session \"%" PRIu64 "\".",
				app->pid, ua_chan->name, ua_sess->tracing_id);
		/* Treat this the same way as an application that is exiting. */
		ret = -ENOTCONN;
		goto error;
	} else if (ret < 0) {
		goto error;
	}

	health_code_update();

	/* Send all streams to application. */
	cds_list_for_each_entry_safe(stream, stmp, &ua_chan->streams.head, list) {
		ret = ust_consumer_send_stream_to_ust(app, ua_chan, stream);
		if (ret == -EPIPE || ret == -LTTNG_UST_ERR_EXITING) {
			ret = -ENOTCONN; /* Caused by app exiting. */
			goto error;
		} else if (ret == -EAGAIN) {
			/* Caused by timeout. */
			WARN("Communication with application %d timed out on send_stream for stream \"%s\" of channel \"%s\" of session \"%" PRIu64 "\".",
					app->pid, stream->name, ua_chan->name,
					ua_sess->tracing_id);
			/*
			 * Treat this the same way as an application that is
			 * exiting.
			 */
			ret = -ENOTCONN;
		} else if (ret < 0) {
			goto error;
		}
		/* We don't need the stream anymore once sent to the tracer. */
		cds_list_del(&stream->list);
		delete_ust_app_stream(-1, stream, app);
	}
	/* Flag the channel that it is sent to the application. */
	ua_chan->is_sent = 1;

error:
	health_code_update();
	return ret;
}

/*
 * Create the specified event onto the UST tracer for a UST session.
 *
 * Should be called with session mutex held.
 */
static
int create_ust_event(struct ust_app *app, struct ust_app_session *ua_sess,
		struct ust_app_channel *ua_chan, struct ust_app_event *ua_event)
{
	int ret = 0;

	health_code_update();

	/* Create UST event on tracer */
	pthread_mutex_lock(&app->sock_lock);
	ret = lttng_ust_ctl_create_event(app->sock, &ua_event->attr, ua_chan->obj,
			&ua_event->obj);
	pthread_mutex_unlock(&app->sock_lock);
	if (ret < 0) {
		if (ret == -EPIPE || ret == -LTTNG_UST_ERR_EXITING) {
			ret = 0;
			DBG3("UST app create event failed. Application is dead: pid = %d, sock = %d",
					app->pid, app->sock);
		} else if (ret == -EAGAIN) {
			ret = 0;
			WARN("UST app create event failed. Communication time out: pid = %d, sock = %d",
					app->pid, app->sock);
		} else {
			ERR("UST app create event '%s' failed with ret %d: pid = %d, sock = %d",
					ua_event->attr.name, ret, app->pid,
					app->sock);
		}
		goto error;
	}

	ua_event->handle = ua_event->obj->handle;

	DBG2("UST app event %s created successfully for pid:%d object = %p",
			ua_event->attr.name, app->pid, ua_event->obj);

	health_code_update();

	/* Set filter if one is present. */
	if (ua_event->filter) {
		ret = set_ust_object_filter(app, ua_event->filter, ua_event->obj);
		if (ret < 0) {
			goto error;
		}
	}

	/* Set exclusions for the event */
	if (ua_event->exclusion) {
		ret = set_ust_object_exclusions(app, ua_event->exclusion, ua_event->obj);
		if (ret < 0) {
			goto error;
		}
	}

	/* If event not enabled, disable it on the tracer */
	if (ua_event->enabled) {
		/*
		 * We now need to explicitly enable the event, since it
		 * is now disabled at creation.
		 */
		ret = enable_ust_object(app, ua_event->obj);
		if (ret < 0) {
			/*
			 * If we hit an EPERM, something is wrong with our enable call. If
			 * we get an EEXIST, there is a problem on the tracer side since we
			 * just created it.
			 */
			switch (ret) {
			case -LTTNG_UST_ERR_PERM:
				/* Code flow problem */
				assert(0);
			case -LTTNG_UST_ERR_EXIST:
				/* It's OK for our use case. */
				ret = 0;
				break;
			default:
				break;
			}
			goto error;
		}
	}

error:
	health_code_update();
	return ret;
}

static int init_ust_event_notifier_from_event_rule(
		const struct lttng_event_rule *rule,
		struct lttng_ust_abi_event_notifier *event_notifier)
{
	enum lttng_event_rule_status status;
	enum lttng_ust_abi_loglevel_type ust_loglevel_type = LTTNG_UST_ABI_LOGLEVEL_ALL;
	int loglevel = -1, ret = 0;
	const char *pattern;


	memset(event_notifier, 0, sizeof(*event_notifier));

	if (lttng_event_rule_targets_agent_domain(rule)) {
		/*
		 * Special event for agents
		 * The actual meat of the event is in the filter that will be
		 * attached later on.
		 * Set the default values for the agent event.
		 */
		pattern = event_get_default_agent_ust_name(
				lttng_event_rule_get_domain_type(rule));
		loglevel = 0;
		ust_loglevel_type = LTTNG_UST_ABI_LOGLEVEL_ALL;
	} else {
		const struct lttng_log_level_rule *log_level_rule;

		assert(lttng_event_rule_get_type(rule) ==
				LTTNG_EVENT_RULE_TYPE_USER_TRACEPOINT);

		status = lttng_event_rule_user_tracepoint_get_name_pattern(rule, &pattern);
		if (status != LTTNG_EVENT_RULE_STATUS_OK) {
			/* At this point, this is a fatal error. */
			abort();
		}

		status = lttng_event_rule_user_tracepoint_get_log_level_rule(
				rule, &log_level_rule);
		if (status == LTTNG_EVENT_RULE_STATUS_UNSET) {
			ust_loglevel_type = LTTNG_UST_ABI_LOGLEVEL_ALL;
		} else if (status == LTTNG_EVENT_RULE_STATUS_OK) {
			enum lttng_log_level_rule_status llr_status;

			switch (lttng_log_level_rule_get_type(log_level_rule)) {
			case LTTNG_LOG_LEVEL_RULE_TYPE_EXACTLY:
				ust_loglevel_type = LTTNG_UST_ABI_LOGLEVEL_SINGLE;
				llr_status = lttng_log_level_rule_exactly_get_level(
						log_level_rule, &loglevel);
				break;
			case LTTNG_LOG_LEVEL_RULE_TYPE_AT_LEAST_AS_SEVERE_AS:
				ust_loglevel_type = LTTNG_UST_ABI_LOGLEVEL_RANGE;
				llr_status = lttng_log_level_rule_at_least_as_severe_as_get_level(
						log_level_rule, &loglevel);
				break;
			default:
				abort();
			}

			assert(llr_status == LTTNG_LOG_LEVEL_RULE_STATUS_OK);
		} else {
			/* At this point this is a fatal error. */
			abort();
		}
	}

	event_notifier->event.instrumentation = LTTNG_UST_ABI_TRACEPOINT;
	ret = lttng_strncpy(event_notifier->event.name, pattern,
			sizeof(event_notifier->event.name));
	if (ret) {
		ERR("Failed to copy event rule pattern to notifier: pattern = '%s' ",
				pattern);
		goto end;
	}

	event_notifier->event.loglevel_type = ust_loglevel_type;
	event_notifier->event.loglevel = loglevel;
end:
	return ret;
}

/*
 * Create the specified event notifier against the user space tracer of a
 * given application.
 */
static int create_ust_event_notifier(struct ust_app *app,
		struct ust_app_event_notifier_rule *ua_event_notifier_rule)
{
	int ret = 0;
	enum lttng_condition_status condition_status;
	const struct lttng_condition *condition = NULL;
	struct lttng_ust_abi_event_notifier event_notifier;
	const struct lttng_event_rule *event_rule = NULL;
	unsigned int capture_bytecode_count = 0, i;
	enum lttng_condition_status cond_status;
	enum lttng_event_rule_type event_rule_type;

	health_code_update();
	assert(app->event_notifier_group.object);

	condition = lttng_trigger_get_const_condition(
			ua_event_notifier_rule->trigger);
	assert(condition);
	assert(lttng_condition_get_type(condition) ==
			LTTNG_CONDITION_TYPE_EVENT_RULE_MATCHES);

	condition_status = lttng_condition_event_rule_matches_get_rule(
			condition, &event_rule);
	assert(condition_status == LTTNG_CONDITION_STATUS_OK);

	assert(event_rule);

	event_rule_type = lttng_event_rule_get_type(event_rule);
	assert(event_rule_type == LTTNG_EVENT_RULE_TYPE_USER_TRACEPOINT ||
			event_rule_type == LTTNG_EVENT_RULE_TYPE_JUL_LOGGING ||
			event_rule_type ==
					LTTNG_EVENT_RULE_TYPE_LOG4J_LOGGING ||
			event_rule_type ==
					LTTNG_EVENT_RULE_TYPE_PYTHON_LOGGING);

	init_ust_event_notifier_from_event_rule(event_rule, &event_notifier);
	event_notifier.event.token = ua_event_notifier_rule->token;
	event_notifier.error_counter_index = ua_event_notifier_rule->error_counter_index;

	/* Create UST event notifier against the tracer. */
	pthread_mutex_lock(&app->sock_lock);
	ret = lttng_ust_ctl_create_event_notifier(app->sock, &event_notifier,
			app->event_notifier_group.object,
			&ua_event_notifier_rule->obj);
	pthread_mutex_unlock(&app->sock_lock);
	if (ret < 0) {
		if (ret == -EPIPE || ret == -LTTNG_UST_ERR_EXITING) {
			ret = 0;
			DBG3("UST app create event notifier failed. Application is dead: pid = %d, sock = %d",
					app->pid, app->sock);
		} else if (ret == -EAGAIN) {
			ret = 0;
			WARN("UST app create event notifier failed. Communication time out: pid = %d, sock = %d",
					app->pid, app->sock);
		} else {
			ERR("UST app create event notifier '%s' failed with ret %d: pid = %d, sock = %d",
					event_notifier.event.name, ret, app->pid,
					app->sock);
		}
		goto error;
	}

	ua_event_notifier_rule->handle = ua_event_notifier_rule->obj->handle;

	DBG2("UST app event notifier %s created successfully: app = '%s': pid = %d), object = %p",
			event_notifier.event.name, app->name, app->pid,
			ua_event_notifier_rule->obj);

	health_code_update();

	/* Set filter if one is present. */
	if (ua_event_notifier_rule->filter) {
		ret = set_ust_object_filter(app, ua_event_notifier_rule->filter,
				ua_event_notifier_rule->obj);
		if (ret < 0) {
			goto error;
		}
	}

	/* Set exclusions for the event. */
	if (ua_event_notifier_rule->exclusion) {
		ret = set_ust_object_exclusions(app,
				ua_event_notifier_rule->exclusion,
				ua_event_notifier_rule->obj);
		if (ret < 0) {
			goto error;
		}
	}

	/* Set the capture bytecodes. */
	cond_status = lttng_condition_event_rule_matches_get_capture_descriptor_count(
			condition, &capture_bytecode_count);
	assert(cond_status == LTTNG_CONDITION_STATUS_OK);

	for (i = 0; i < capture_bytecode_count; i++) {
		const struct lttng_bytecode *capture_bytecode =
				lttng_condition_event_rule_matches_get_capture_bytecode_at_index(
						condition, i);

		ret = set_ust_capture(app, capture_bytecode, i,
				ua_event_notifier_rule->obj);
		if (ret < 0) {
			goto error;
		}
	}

	/*
	 * We now need to explicitly enable the event, since it
	 * is disabled at creation.
	 */
	ret = enable_ust_object(app, ua_event_notifier_rule->obj);
	if (ret < 0) {
		/*
		 * If we hit an EPERM, something is wrong with our enable call.
		 * If we get an EEXIST, there is a problem on the tracer side
		 * since we just created it.
		 */
		switch (ret) {
		case -LTTNG_UST_ERR_PERM:
			/* Code flow problem. */
			abort();
		case -LTTNG_UST_ERR_EXIST:
			/* It's OK for our use case. */
			ret = 0;
			break;
		default:
			break;
		}

		goto error;
	}

	ua_event_notifier_rule->enabled = true;

error:
	health_code_update();
	return ret;
}

/*
 * Copy data between an UST app event and a LTT event.
 */
static void shadow_copy_event(struct ust_app_event *ua_event,
		struct ltt_ust_event *uevent)
{
	size_t exclusion_alloc_size;

	strncpy(ua_event->name, uevent->attr.name, sizeof(ua_event->name));
	ua_event->name[sizeof(ua_event->name) - 1] = '\0';

	ua_event->enabled = uevent->enabled;

	/* Copy event attributes */
	memcpy(&ua_event->attr, &uevent->attr, sizeof(ua_event->attr));

	/* Copy filter bytecode */
	if (uevent->filter) {
		ua_event->filter = lttng_bytecode_copy(uevent->filter);
		/* Filter might be NULL here in case of ENONEM. */
	}

	/* Copy exclusion data */
	if (uevent->exclusion) {
		exclusion_alloc_size = sizeof(struct lttng_event_exclusion) +
				LTTNG_UST_ABI_SYM_NAME_LEN * uevent->exclusion->count;
		ua_event->exclusion = zmalloc(exclusion_alloc_size);
		if (ua_event->exclusion == NULL) {
			PERROR("malloc");
		} else {
			memcpy(ua_event->exclusion, uevent->exclusion,
					exclusion_alloc_size);
		}
	}
}

/*
 * Copy data between an UST app channel and a LTT channel.
 */
static void shadow_copy_channel(struct ust_app_channel *ua_chan,
		struct ltt_ust_channel *uchan)
{
	DBG2("UST app shadow copy of channel %s started", ua_chan->name);

	strncpy(ua_chan->name, uchan->name, sizeof(ua_chan->name));
	ua_chan->name[sizeof(ua_chan->name) - 1] = '\0';

	ua_chan->tracefile_size = uchan->tracefile_size;
	ua_chan->tracefile_count = uchan->tracefile_count;

	/* Copy event attributes since the layout is different. */
	ua_chan->attr.subbuf_size = uchan->attr.subbuf_size;
	ua_chan->attr.num_subbuf = uchan->attr.num_subbuf;
	ua_chan->attr.overwrite = uchan->attr.overwrite;
	ua_chan->attr.switch_timer_interval = uchan->attr.switch_timer_interval;
	ua_chan->attr.read_timer_interval = uchan->attr.read_timer_interval;
	ua_chan->monitor_timer_interval = uchan->monitor_timer_interval;
	ua_chan->attr.output = uchan->attr.output;
	ua_chan->attr.blocking_timeout = uchan->attr.u.s.blocking_timeout;

	/*
	 * Note that the attribute channel type is not set since the channel on the
	 * tracing registry side does not have this information.
	 */

	ua_chan->enabled = uchan->enabled;
	ua_chan->tracing_channel_id = uchan->id;

	DBG3("UST app shadow copy of channel %s done", ua_chan->name);
}

/*
 * Copy data between a UST app session and a regular LTT session.
 */
static void shadow_copy_session(struct ust_app_session *ua_sess,
		struct ltt_ust_session *usess, struct ust_app *app)
{
	struct tm *timeinfo;
	char datetime[16];
	int ret;
	char tmp_shm_path[PATH_MAX];

	timeinfo = localtime(&app->registration_time);
	strftime(datetime, sizeof(datetime), "%Y%m%d-%H%M%S", timeinfo);

	DBG2("Shadow copy of session handle %d", ua_sess->handle);

	ua_sess->tracing_id = usess->id;
	ua_sess->id = get_next_session_id();
	LTTNG_OPTIONAL_SET(&ua_sess->real_credentials.uid, app->uid);
	LTTNG_OPTIONAL_SET(&ua_sess->real_credentials.gid, app->gid);
	LTTNG_OPTIONAL_SET(&ua_sess->effective_credentials.uid, usess->uid);
	LTTNG_OPTIONAL_SET(&ua_sess->effective_credentials.gid, usess->gid);
	ua_sess->buffer_type = usess->buffer_type;
	ua_sess->bits_per_long = app->bits_per_long;

	/* There is only one consumer object per session possible. */
	consumer_output_get(usess->consumer);
	ua_sess->consumer = usess->consumer;

	ua_sess->output_traces = usess->output_traces;
	ua_sess->live_timer_interval = usess->live_timer_interval;
	copy_channel_attr_to_ustctl(&ua_sess->metadata_attr,
			&usess->metadata_attr);

	switch (ua_sess->buffer_type) {
	case LTTNG_BUFFER_PER_PID:
		ret = snprintf(ua_sess->path, sizeof(ua_sess->path),
				DEFAULT_UST_TRACE_PID_PATH "/%s-%d-%s-%ld", app->name, app->pid,
				datetime, ua_sess->id);
		break;
	case LTTNG_BUFFER_PER_UID:
		ret = snprintf(ua_sess->path, sizeof(ua_sess->path),
				DEFAULT_UST_TRACE_UID_PATH,
				lttng_credentials_get_uid(&ua_sess->real_credentials),
				app->bits_per_long);
		break;
	default:
		assert(0);
		goto error;
	}
	if (ret < 0) {
		PERROR("asprintf UST shadow copy session");
		assert(0);
		goto error;
	}

	strncpy(ua_sess->root_shm_path, usess->root_shm_path,
		sizeof(ua_sess->root_shm_path));
	ua_sess->root_shm_path[sizeof(ua_sess->root_shm_path) - 1] = '\0';
	strncpy(ua_sess->shm_path, usess->shm_path,
		sizeof(ua_sess->shm_path));
	ua_sess->shm_path[sizeof(ua_sess->shm_path) - 1] = '\0';
	if (ua_sess->shm_path[0]) {
		switch (ua_sess->buffer_type) {
		case LTTNG_BUFFER_PER_PID:
			ret = snprintf(tmp_shm_path, sizeof(tmp_shm_path),
					"/" DEFAULT_UST_TRACE_PID_PATH "/%s-%d-%s-%ld",
					app->name, app->pid, datetime, ua_sess->id);
			break;
		case LTTNG_BUFFER_PER_UID:
			ret = snprintf(tmp_shm_path, sizeof(tmp_shm_path),
					"/" DEFAULT_UST_TRACE_UID_PATH,
					app->uid, app->bits_per_long);
			break;
		default:
			assert(0);
			goto error;
		}
		if (ret < 0) {
			PERROR("sprintf UST shadow copy session");
			assert(0);
			goto error;
		}
		strncat(ua_sess->shm_path, tmp_shm_path,
			sizeof(ua_sess->shm_path) - strlen(ua_sess->shm_path) - 1);
		ua_sess->shm_path[sizeof(ua_sess->shm_path) - 1] = '\0';
	}
	return;

error:
	consumer_output_put(ua_sess->consumer);
}

/*
 * Lookup sesison wrapper.
 */
static
void __lookup_session_by_app(const struct ltt_ust_session *usess,
			struct ust_app *app, struct lttng_ht_iter *iter)
{
	/* Get right UST app session from app */
	lttng_ht_lookup(app->sessions, &usess->id, iter);
}

/*
 * Return ust app session from the app session hashtable using the UST session
 * id.
 */
static struct ust_app_session *lookup_session_by_app(
		const struct ltt_ust_session *usess, struct ust_app *app)
{
	struct lttng_ht_iter iter;
	struct lttng_ht_node_u64 *node;

	__lookup_session_by_app(usess, app, &iter);
	node = lttng_ht_iter_get_node_u64(&iter);
	if (node == NULL) {
		goto error;
	}

	return caa_container_of(node, struct ust_app_session, node);

error:
	return NULL;
}

/*
 * Setup buffer registry per PID for the given session and application. If none
 * is found, a new one is created, added to the global registry and
 * initialized. If regp is valid, it's set with the newly created object.
 *
 * Return 0 on success or else a negative value.
 */
static int setup_buffer_reg_pid(struct ust_app_session *ua_sess,
		struct ust_app *app, struct buffer_reg_pid **regp)
{
	int ret = 0;
	struct buffer_reg_pid *reg_pid;

	assert(ua_sess);
	assert(app);

	rcu_read_lock();

	reg_pid = buffer_reg_pid_find(ua_sess->id);
	if (!reg_pid) {
		/*
		 * This is the create channel path meaning that if there is NO
		 * registry available, we have to create one for this session.
		 */
		ret = buffer_reg_pid_create(ua_sess->id, &reg_pid,
			ua_sess->root_shm_path, ua_sess->shm_path);
		if (ret < 0) {
			goto error;
		}
	} else {
		goto end;
	}

	/* Initialize registry. */
	ret = ust_registry_session_init(&reg_pid->registry->reg.ust, app,
			app->bits_per_long, app->uint8_t_alignment,
			app->uint16_t_alignment, app->uint32_t_alignment,
			app->uint64_t_alignment, app->long_alignment,
			app->byte_order, app->version.major, app->version.minor,
			reg_pid->root_shm_path, reg_pid->shm_path,
			lttng_credentials_get_uid(&ua_sess->effective_credentials),
			lttng_credentials_get_gid(&ua_sess->effective_credentials),
			ua_sess->tracing_id,
			app->uid);
	if (ret < 0) {
		/*
		 * reg_pid->registry->reg.ust is NULL upon error, so we need to
		 * destroy the buffer registry, because it is always expected
		 * that if the buffer registry can be found, its ust registry is
		 * non-NULL.
		 */
		buffer_reg_pid_destroy(reg_pid);
		goto error;
	}

	buffer_reg_pid_add(reg_pid);

	DBG3("UST app buffer registry per PID created successfully");

end:
	if (regp) {
		*regp = reg_pid;
	}
error:
	rcu_read_unlock();
	return ret;
}

/*
 * Setup buffer registry per UID for the given session and application. If none
 * is found, a new one is created, added to the global registry and
 * initialized. If regp is valid, it's set with the newly created object.
 *
 * Return 0 on success or else a negative value.
 */
static int setup_buffer_reg_uid(struct ltt_ust_session *usess,
		struct ust_app_session *ua_sess,
		struct ust_app *app, struct buffer_reg_uid **regp)
{
	int ret = 0;
	struct buffer_reg_uid *reg_uid;

	assert(usess);
	assert(app);

	rcu_read_lock();

	reg_uid = buffer_reg_uid_find(usess->id, app->bits_per_long, app->uid);
	if (!reg_uid) {
		/*
		 * This is the create channel path meaning that if there is NO
		 * registry available, we have to create one for this session.
		 */
		ret = buffer_reg_uid_create(usess->id, app->bits_per_long, app->uid,
				LTTNG_DOMAIN_UST, &reg_uid,
				ua_sess->root_shm_path, ua_sess->shm_path);
		if (ret < 0) {
			goto error;
		}
	} else {
		goto end;
	}

	/* Initialize registry. */
	ret = ust_registry_session_init(&reg_uid->registry->reg.ust, NULL,
			app->bits_per_long, app->uint8_t_alignment,
			app->uint16_t_alignment, app->uint32_t_alignment,
			app->uint64_t_alignment, app->long_alignment,
			app->byte_order, app->version.major,
			app->version.minor, reg_uid->root_shm_path,
			reg_uid->shm_path, usess->uid, usess->gid,
			ua_sess->tracing_id, app->uid);
	if (ret < 0) {
		/*
		 * reg_uid->registry->reg.ust is NULL upon error, so we need to
		 * destroy the buffer registry, because it is always expected
		 * that if the buffer registry can be found, its ust registry is
		 * non-NULL.
		 */
		buffer_reg_uid_destroy(reg_uid, NULL);
		goto error;
	}
	/* Add node to teardown list of the session. */
	cds_list_add(&reg_uid->lnode, &usess->buffer_reg_uid_list);

	buffer_reg_uid_add(reg_uid);

	DBG3("UST app buffer registry per UID created successfully");
end:
	if (regp) {
		*regp = reg_uid;
	}
error:
	rcu_read_unlock();
	return ret;
}

/*
 * Create a session on the tracer side for the given app.
 *
 * On success, ua_sess_ptr is populated with the session pointer or else left
 * untouched. If the session was created, is_created is set to 1. On error,
 * it's left untouched. Note that ua_sess_ptr is mandatory but is_created can
 * be NULL.
 *
 * Returns 0 on success or else a negative code which is either -ENOMEM or
 * -ENOTCONN which is the default code if the lttng_ust_ctl_create_session fails.
 */
static int find_or_create_ust_app_session(struct ltt_ust_session *usess,
		struct ust_app *app, struct ust_app_session **ua_sess_ptr,
		int *is_created)
{
	int ret, created = 0;
	struct ust_app_session *ua_sess;

	assert(usess);
	assert(app);
	assert(ua_sess_ptr);

	health_code_update();

	ua_sess = lookup_session_by_app(usess, app);
	if (ua_sess == NULL) {
		DBG2("UST app pid: %d session id %" PRIu64 " not found, creating it",
				app->pid, usess->id);
		ua_sess = alloc_ust_app_session();
		if (ua_sess == NULL) {
			/* Only malloc can failed so something is really wrong */
			ret = -ENOMEM;
			goto error;
		}
		shadow_copy_session(ua_sess, usess, app);
		created = 1;
	}

	switch (usess->buffer_type) {
	case LTTNG_BUFFER_PER_PID:
		/* Init local registry. */
		ret = setup_buffer_reg_pid(ua_sess, app, NULL);
		if (ret < 0) {
			delete_ust_app_session(-1, ua_sess, app);
			goto error;
		}
		break;
	case LTTNG_BUFFER_PER_UID:
		/* Look for a global registry. If none exists, create one. */
		ret = setup_buffer_reg_uid(usess, ua_sess, app, NULL);
		if (ret < 0) {
			delete_ust_app_session(-1, ua_sess, app);
			goto error;
		}
		break;
	default:
		assert(0);
		ret = -EINVAL;
		goto error;
	}

	health_code_update();

	if (ua_sess->handle == -1) {
		pthread_mutex_lock(&app->sock_lock);
		ret = lttng_ust_ctl_create_session(app->sock);
		pthread_mutex_unlock(&app->sock_lock);
		if (ret < 0) {
			if (ret == -EPIPE || ret == -LTTNG_UST_ERR_EXITING) {
				DBG("UST app creating session failed. Application is dead: pid = %d, sock = %d",
						app->pid, app->sock);
				ret = 0;
			} else if (ret == -EAGAIN) {
				DBG("UST app creating session failed. Communication time out: pid = %d, sock = %d",
						app->pid, app->sock);
				ret = 0;
			} else {
				ERR("UST app creating session failed with ret %d: pid = %d, sock =%d",
						ret, app->pid, app->sock);
			}
			delete_ust_app_session(-1, ua_sess, app);
			if (ret != -ENOMEM) {
				/*
				 * Tracer is probably gone or got an internal error so let's
				 * behave like it will soon unregister or not usable.
				 */
				ret = -ENOTCONN;
			}
			goto error;
		}

		ua_sess->handle = ret;

		/* Add ust app session to app's HT */
		lttng_ht_node_init_u64(&ua_sess->node,
				ua_sess->tracing_id);
		lttng_ht_add_unique_u64(app->sessions, &ua_sess->node);
		lttng_ht_node_init_ulong(&ua_sess->ust_objd_node, ua_sess->handle);
		lttng_ht_add_unique_ulong(app->ust_sessions_objd,
				&ua_sess->ust_objd_node);

		DBG2("UST app session created successfully with handle %d", ret);
	}

	*ua_sess_ptr = ua_sess;
	if (is_created) {
		*is_created = created;
	}

	/* Everything went well. */
	ret = 0;

error:
	health_code_update();
	return ret;
}

/*
 * Match function for a hash table lookup of ust_app_ctx.
 *
 * It matches an ust app context based on the context type and, in the case
 * of perf counters, their name.
 */
static int ht_match_ust_app_ctx(struct cds_lfht_node *node, const void *_key)
{
	struct ust_app_ctx *ctx;
	const struct lttng_ust_context_attr *key;

	assert(node);
	assert(_key);

	ctx = caa_container_of(node, struct ust_app_ctx, node.node);
	key = _key;

	/* Context type */
	if (ctx->ctx.ctx != key->ctx) {
		goto no_match;
	}

	switch(key->ctx) {
	case LTTNG_UST_ABI_CONTEXT_PERF_THREAD_COUNTER:
		if (strncmp(key->u.perf_counter.name,
				ctx->ctx.u.perf_counter.name,
				sizeof(key->u.perf_counter.name))) {
			goto no_match;
		}
		break;
	case LTTNG_UST_ABI_CONTEXT_APP_CONTEXT:
		if (strcmp(key->u.app_ctx.provider_name,
				ctx->ctx.u.app_ctx.provider_name) ||
				strcmp(key->u.app_ctx.ctx_name,
				ctx->ctx.u.app_ctx.ctx_name)) {
			goto no_match;
		}
		break;
	default:
		break;
	}

	/* Match. */
	return 1;

no_match:
	return 0;
}

/*
 * Lookup for an ust app context from an lttng_ust_context.
 *
 * Must be called while holding RCU read side lock.
 * Return an ust_app_ctx object or NULL on error.
 */
static
struct ust_app_ctx *find_ust_app_context(struct lttng_ht *ht,
		struct lttng_ust_context_attr *uctx)
{
	struct lttng_ht_iter iter;
	struct lttng_ht_node_ulong *node;
	struct ust_app_ctx *app_ctx = NULL;

	assert(uctx);
	assert(ht);

	/* Lookup using the lttng_ust_context_type and a custom match fct. */
	cds_lfht_lookup(ht->ht, ht->hash_fct((void *) uctx->ctx, lttng_ht_seed),
			ht_match_ust_app_ctx, uctx, &iter.iter);
	node = lttng_ht_iter_get_node_ulong(&iter);
	if (!node) {
		goto end;
	}

	app_ctx = caa_container_of(node, struct ust_app_ctx, node);

end:
	return app_ctx;
}

/*
 * Create a context for the channel on the tracer.
 *
 * Called with UST app session lock held and a RCU read side lock.
 */
static
int create_ust_app_channel_context(struct ust_app_channel *ua_chan,
		struct lttng_ust_context_attr *uctx,
		struct ust_app *app)
{
	int ret = 0;
	struct ust_app_ctx *ua_ctx;

	DBG2("UST app adding context to channel %s", ua_chan->name);

	ua_ctx = find_ust_app_context(ua_chan->ctx, uctx);
	if (ua_ctx) {
		ret = -EEXIST;
		goto error;
	}

	ua_ctx = alloc_ust_app_ctx(uctx);
	if (ua_ctx == NULL) {
		/* malloc failed */
		ret = -ENOMEM;
		goto error;
	}

	lttng_ht_node_init_ulong(&ua_ctx->node, (unsigned long) ua_ctx->ctx.ctx);
	lttng_ht_add_ulong(ua_chan->ctx, &ua_ctx->node);
	cds_list_add_tail(&ua_ctx->list, &ua_chan->ctx_list);

	ret = create_ust_channel_context(ua_chan, ua_ctx, app);
	if (ret < 0) {
		goto error;
	}

error:
	return ret;
}

/*
 * Enable on the tracer side a ust app event for the session and channel.
 *
 * Called with UST app session lock held.
 */
static
int enable_ust_app_event(struct ust_app_session *ua_sess,
		struct ust_app_event *ua_event, struct ust_app *app)
{
	int ret;

	ret = enable_ust_object(app, ua_event->obj);
	if (ret < 0) {
		goto error;
	}

	ua_event->enabled = 1;

error:
	return ret;
}

/*
 * Disable on the tracer side a ust app event for the session and channel.
 */
static int disable_ust_app_event(struct ust_app_session *ua_sess,
		struct ust_app_event *ua_event, struct ust_app *app)
{
	int ret;

	ret = disable_ust_object(app, ua_event->obj);
	if (ret < 0) {
		goto error;
	}

	ua_event->enabled = 0;

error:
	return ret;
}

/*
 * Lookup ust app channel for session and disable it on the tracer side.
 */
static
int disable_ust_app_channel(struct ust_app_session *ua_sess,
		struct ust_app_channel *ua_chan, struct ust_app *app)
{
	int ret;

	ret = disable_ust_channel(app, ua_sess, ua_chan);
	if (ret < 0) {
		goto error;
	}

	ua_chan->enabled = 0;

error:
	return ret;
}

/*
 * Lookup ust app channel for session and enable it on the tracer side. This
 * MUST be called with a RCU read side lock acquired.
 */
static int enable_ust_app_channel(struct ust_app_session *ua_sess,
		struct ltt_ust_channel *uchan, struct ust_app *app)
{
	int ret = 0;
	struct lttng_ht_iter iter;
	struct lttng_ht_node_str *ua_chan_node;
	struct ust_app_channel *ua_chan;

	lttng_ht_lookup(ua_sess->channels, (void *)uchan->name, &iter);
	ua_chan_node = lttng_ht_iter_get_node_str(&iter);
	if (ua_chan_node == NULL) {
		DBG2("Unable to find channel %s in ust session id %" PRIu64,
				uchan->name, ua_sess->tracing_id);
		goto error;
	}

	ua_chan = caa_container_of(ua_chan_node, struct ust_app_channel, node);

	ret = enable_ust_channel(app, ua_sess, ua_chan);
	if (ret < 0) {
		goto error;
	}

error:
	return ret;
}

/*
 * Ask the consumer to create a channel and get it if successful.
 *
 * Called with UST app session lock held.
 *
 * Return 0 on success or else a negative value.
 */
static int do_consumer_create_channel(struct ltt_ust_session *usess,
		struct ust_app_session *ua_sess, struct ust_app_channel *ua_chan,
		int bitness, struct ust_registry_session *registry,
		uint64_t trace_archive_id)
{
	int ret;
	unsigned int nb_fd = 0;
	struct consumer_socket *socket;

	assert(usess);
	assert(ua_sess);
	assert(ua_chan);
	assert(registry);

	rcu_read_lock();
	health_code_update();

	/* Get the right consumer socket for the application. */
	socket = consumer_find_socket_by_bitness(bitness, usess->consumer);
	if (!socket) {
		ret = -EINVAL;
		goto error;
	}

	health_code_update();

	/* Need one fd for the channel. */
	ret = lttng_fd_get(LTTNG_FD_APPS, 1);
	if (ret < 0) {
		ERR("Exhausted number of available FD upon create channel");
		goto error;
	}

	/*
	 * Ask consumer to create channel. The consumer will return the number of
	 * stream we have to expect.
	 */
	ret = ust_consumer_ask_channel(ua_sess, ua_chan, usess->consumer, socket,
			registry, usess->current_trace_chunk);
	if (ret < 0) {
		goto error_ask;
	}

	/*
	 * Compute the number of fd needed before receiving them. It must be 2 per
	 * stream (2 being the default value here).
	 */
	nb_fd = DEFAULT_UST_STREAM_FD_NUM * ua_chan->expected_stream_count;

	/* Reserve the amount of file descriptor we need. */
	ret = lttng_fd_get(LTTNG_FD_APPS, nb_fd);
	if (ret < 0) {
		ERR("Exhausted number of available FD upon create channel");
		goto error_fd_get_stream;
	}

	health_code_update();

	/*
	 * Now get the channel from the consumer. This call will populate the stream
	 * list of that channel and set the ust objects.
	 */
	if (usess->consumer->enabled) {
		ret = ust_consumer_get_channel(socket, ua_chan);
		if (ret < 0) {
			goto error_destroy;
		}
	}

	rcu_read_unlock();
	return 0;

error_destroy:
	lttng_fd_put(LTTNG_FD_APPS, nb_fd);
error_fd_get_stream:
	/*
	 * Initiate a destroy channel on the consumer since we had an error
	 * handling it on our side. The return value is of no importance since we
	 * already have a ret value set by the previous error that we need to
	 * return.
	 */
	(void) ust_consumer_destroy_channel(socket, ua_chan);
error_ask:
	lttng_fd_put(LTTNG_FD_APPS, 1);
error:
	health_code_update();
	rcu_read_unlock();
	return ret;
}

/*
 * Duplicate the ust data object of the ust app stream and save it in the
 * buffer registry stream.
 *
 * Return 0 on success or else a negative value.
 */
static int duplicate_stream_object(struct buffer_reg_stream *reg_stream,
		struct ust_app_stream *stream)
{
	int ret;

	assert(reg_stream);
	assert(stream);

	/* Duplicating a stream requires 2 new fds. Reserve them. */
	ret = lttng_fd_get(LTTNG_FD_APPS, 2);
	if (ret < 0) {
		ERR("Exhausted number of available FD upon duplicate stream");
		goto error;
	}

	/* Duplicate object for stream once the original is in the registry. */
	ret = lttng_ust_ctl_duplicate_ust_object_data(&stream->obj,
			reg_stream->obj.ust);
	if (ret < 0) {
		ERR("Duplicate stream obj from %p to %p failed with ret %d",
				reg_stream->obj.ust, stream->obj, ret);
		lttng_fd_put(LTTNG_FD_APPS, 2);
		goto error;
	}
	stream->handle = stream->obj->handle;

error:
	return ret;
}

/*
 * Duplicate the ust data object of the ust app. channel and save it in the
 * buffer registry channel.
 *
 * Return 0 on success or else a negative value.
 */
static int duplicate_channel_object(struct buffer_reg_channel *buf_reg_chan,
		struct ust_app_channel *ua_chan)
{
	int ret;

	assert(buf_reg_chan);
	assert(ua_chan);

	/* Duplicating a channel requires 1 new fd. Reserve it. */
	ret = lttng_fd_get(LTTNG_FD_APPS, 1);
	if (ret < 0) {
		ERR("Exhausted number of available FD upon duplicate channel");
		goto error_fd_get;
	}

	/* Duplicate object for stream once the original is in the registry. */
	ret = lttng_ust_ctl_duplicate_ust_object_data(&ua_chan->obj, buf_reg_chan->obj.ust);
	if (ret < 0) {
		ERR("Duplicate channel obj from %p to %p failed with ret: %d",
				buf_reg_chan->obj.ust, ua_chan->obj, ret);
		goto error;
	}
	ua_chan->handle = ua_chan->obj->handle;

	return 0;

error:
	lttng_fd_put(LTTNG_FD_APPS, 1);
error_fd_get:
	return ret;
}

/*
 * For a given channel buffer registry, setup all streams of the given ust
 * application channel.
 *
 * Return 0 on success or else a negative value.
 */
static int setup_buffer_reg_streams(struct buffer_reg_channel *buf_reg_chan,
		struct ust_app_channel *ua_chan,
		struct ust_app *app)
{
	int ret = 0;
	struct ust_app_stream *stream, *stmp;

	assert(buf_reg_chan);
	assert(ua_chan);

	DBG2("UST app setup buffer registry stream");

	/* Send all streams to application. */
	cds_list_for_each_entry_safe(stream, stmp, &ua_chan->streams.head, list) {
		struct buffer_reg_stream *reg_stream;

		ret = buffer_reg_stream_create(&reg_stream);
		if (ret < 0) {
			goto error;
		}

		/*
		 * Keep original pointer and nullify it in the stream so the delete
		 * stream call does not release the object.
		 */
		reg_stream->obj.ust = stream->obj;
		stream->obj = NULL;
		buffer_reg_stream_add(reg_stream, buf_reg_chan);

		/* We don't need the streams anymore. */
		cds_list_del(&stream->list);
		delete_ust_app_stream(-1, stream, app);
	}

error:
	return ret;
}

/*
 * Create a buffer registry channel for the given session registry and
 * application channel object. If regp pointer is valid, it's set with the
 * created object. Important, the created object is NOT added to the session
 * registry hash table.
 *
 * Return 0 on success else a negative value.
 */
static int create_buffer_reg_channel(struct buffer_reg_session *reg_sess,
		struct ust_app_channel *ua_chan, struct buffer_reg_channel **regp)
{
	int ret;
	struct buffer_reg_channel *buf_reg_chan = NULL;

	assert(reg_sess);
	assert(ua_chan);

	DBG2("UST app creating buffer registry channel for %s", ua_chan->name);

	/* Create buffer registry channel. */
	ret = buffer_reg_channel_create(ua_chan->tracing_channel_id, &buf_reg_chan);
	if (ret < 0) {
		goto error_create;
	}
	assert(buf_reg_chan);
	buf_reg_chan->consumer_key = ua_chan->key;
	buf_reg_chan->subbuf_size = ua_chan->attr.subbuf_size;
	buf_reg_chan->num_subbuf = ua_chan->attr.num_subbuf;

	/* Create and add a channel registry to session. */
	ret = ust_registry_channel_add(reg_sess->reg.ust,
			ua_chan->tracing_channel_id);
	if (ret < 0) {
		goto error;
	}
	buffer_reg_channel_add(reg_sess, buf_reg_chan);

	if (regp) {
		*regp = buf_reg_chan;
	}

	return 0;

error:
	/* Safe because the registry channel object was not added to any HT. */
	buffer_reg_channel_destroy(buf_reg_chan, LTTNG_DOMAIN_UST);
error_create:
	return ret;
}

/*
 * Setup buffer registry channel for the given session registry and application
 * channel object. If regp pointer is valid, it's set with the created object.
 *
 * Return 0 on success else a negative value.
 */
static int setup_buffer_reg_channel(struct buffer_reg_session *reg_sess,
		struct ust_app_channel *ua_chan, struct buffer_reg_channel *buf_reg_chan,
		struct ust_app *app)
{
	int ret;

	assert(reg_sess);
	assert(buf_reg_chan);
	assert(ua_chan);
	assert(ua_chan->obj);

	DBG2("UST app setup buffer registry channel for %s", ua_chan->name);

	/* Setup all streams for the registry. */
	ret = setup_buffer_reg_streams(buf_reg_chan, ua_chan, app);
	if (ret < 0) {
		goto error;
	}

	buf_reg_chan->obj.ust = ua_chan->obj;
	ua_chan->obj = NULL;

	return 0;

error:
	buffer_reg_channel_remove(reg_sess, buf_reg_chan);
	buffer_reg_channel_destroy(buf_reg_chan, LTTNG_DOMAIN_UST);
	return ret;
}

/*
 * Send buffer registry channel to the application.
 *
 * Return 0 on success else a negative value.
 */
static int send_channel_uid_to_ust(struct buffer_reg_channel *buf_reg_chan,
		struct ust_app *app, struct ust_app_session *ua_sess,
		struct ust_app_channel *ua_chan)
{
	int ret;
	struct buffer_reg_stream *reg_stream;

	assert(buf_reg_chan);
	assert(app);
	assert(ua_sess);
	assert(ua_chan);

	DBG("UST app sending buffer registry channel to ust sock %d", app->sock);

	ret = duplicate_channel_object(buf_reg_chan, ua_chan);
	if (ret < 0) {
		goto error;
	}

	/* Send channel to the application. */
	ret = ust_consumer_send_channel_to_ust(app, ua_sess, ua_chan);
	if (ret == -EPIPE || ret == -LTTNG_UST_ERR_EXITING) {
		ret = -ENOTCONN;	/* Caused by app exiting. */
		goto error;
	} else if (ret == -EAGAIN) {
		/* Caused by timeout. */
		WARN("Communication with application %d timed out on send_channel for channel \"%s\" of session \"%" PRIu64 "\".",
				app->pid, ua_chan->name, ua_sess->tracing_id);
		/* Treat this the same way as an application that is exiting. */
		ret = -ENOTCONN;
		goto error;
	} else if (ret < 0) {
		goto error;
	}

	health_code_update();

	/* Send all streams to application. */
	pthread_mutex_lock(&buf_reg_chan->stream_list_lock);
	cds_list_for_each_entry(reg_stream, &buf_reg_chan->streams, lnode) {
		struct ust_app_stream stream = {};

		ret = duplicate_stream_object(reg_stream, &stream);
		if (ret < 0) {
			goto error_stream_unlock;
		}

		ret = ust_consumer_send_stream_to_ust(app, ua_chan, &stream);
		if (ret < 0) {
			if (ret == -EPIPE || ret == -LTTNG_UST_ERR_EXITING) {
				ret = -ENOTCONN; /* Caused by app exiting. */
			} else if (ret == -EAGAIN) {
				/*
				 * Caused by timeout.
				 * Treat this the same way as an application
				 * that is exiting.
				 */
				WARN("Communication with application %d timed out on send_stream for stream of channel \"%s\" of session \"%" PRIu64 "\".",
						app->pid,
						ua_chan->name,
						ua_sess->tracing_id);
				ret = -ENOTCONN;
			}
			(void) release_ust_app_stream(-1, &stream, app);
			goto error_stream_unlock;
		}

		/*
		 * The return value is not important here. This function will output an
		 * error if needed.
		 */
		(void) release_ust_app_stream(-1, &stream, app);
	}
	ua_chan->is_sent = 1;

error_stream_unlock:
	pthread_mutex_unlock(&buf_reg_chan->stream_list_lock);
error:
	return ret;
}

/*
 * Create and send to the application the created buffers with per UID buffers.
 *
 * This MUST be called with a RCU read side lock acquired.
 * The session list lock and the session's lock must be acquired.
 *
 * Return 0 on success else a negative value.
 */
static int create_channel_per_uid(struct ust_app *app,
		struct ltt_ust_session *usess, struct ust_app_session *ua_sess,
		struct ust_app_channel *ua_chan)
{
	int ret;
	struct buffer_reg_uid *reg_uid;
	struct buffer_reg_channel *buf_reg_chan;
	struct ltt_session *session = NULL;
	enum lttng_error_code notification_ret;
	struct ust_registry_channel *ust_reg_chan;

	assert(app);
	assert(usess);
	assert(ua_sess);
	assert(ua_chan);

	DBG("UST app creating channel %s with per UID buffers", ua_chan->name);

	reg_uid = buffer_reg_uid_find(usess->id, app->bits_per_long, app->uid);
	/*
	 * The session creation handles the creation of this global registry
	 * object. If none can be find, there is a code flow problem or a
	 * teardown race.
	 */
	assert(reg_uid);

	buf_reg_chan = buffer_reg_channel_find(ua_chan->tracing_channel_id,
			reg_uid);
	if (buf_reg_chan) {
		goto send_channel;
	}

	/* Create the buffer registry channel object. */
	ret = create_buffer_reg_channel(reg_uid->registry, ua_chan, &buf_reg_chan);
	if (ret < 0) {
		ERR("Error creating the UST channel \"%s\" registry instance",
				ua_chan->name);
		goto error;
	}

	session = session_find_by_id(ua_sess->tracing_id);
	assert(session);
	assert(pthread_mutex_trylock(&session->lock));
	assert(session_trylock_list());

	/*
	 * Create the buffers on the consumer side. This call populates the
	 * ust app channel object with all streams and data object.
	 */
	ret = do_consumer_create_channel(usess, ua_sess, ua_chan,
			app->bits_per_long, reg_uid->registry->reg.ust,
			session->most_recent_chunk_id.value);
	if (ret < 0) {
		ERR("Error creating UST channel \"%s\" on the consumer daemon",
				ua_chan->name);

		/*
		 * Let's remove the previously created buffer registry channel so
		 * it's not visible anymore in the session registry.
		 */
		ust_registry_channel_del_free(reg_uid->registry->reg.ust,
				ua_chan->tracing_channel_id, false);
		buffer_reg_channel_remove(reg_uid->registry, buf_reg_chan);
		buffer_reg_channel_destroy(buf_reg_chan, LTTNG_DOMAIN_UST);
		goto error;
	}

	/*
	 * Setup the streams and add it to the session registry.
	 */
	ret = setup_buffer_reg_channel(reg_uid->registry,
			ua_chan, buf_reg_chan, app);
	if (ret < 0) {
		ERR("Error setting up UST channel \"%s\"", ua_chan->name);
		goto error;
	}

	/* Notify the notification subsystem of the channel's creation. */
	pthread_mutex_lock(&reg_uid->registry->reg.ust->lock);
	ust_reg_chan = ust_registry_channel_find(reg_uid->registry->reg.ust,
			ua_chan->tracing_channel_id);
	assert(ust_reg_chan);
	ust_reg_chan->consumer_key = ua_chan->key;
	ust_reg_chan = NULL;
	pthread_mutex_unlock(&reg_uid->registry->reg.ust->lock);

	notification_ret = notification_thread_command_add_channel(
			the_notification_thread_handle, session->name,
			lttng_credentials_get_uid(
					&ua_sess->effective_credentials),
			lttng_credentials_get_gid(
					&ua_sess->effective_credentials),
			ua_chan->name, ua_chan->key, LTTNG_DOMAIN_UST,
			ua_chan->attr.subbuf_size * ua_chan->attr.num_subbuf);
	if (notification_ret != LTTNG_OK) {
		ret = - (int) notification_ret;
		ERR("Failed to add channel to notification thread");
		goto error;
	}

send_channel:
	/* Send buffers to the application. */
	ret = send_channel_uid_to_ust(buf_reg_chan, app, ua_sess, ua_chan);
	if (ret < 0) {
		if (ret != -ENOTCONN) {
			ERR("Error sending channel to application");
		}
		goto error;
	}

error:
	if (session) {
		session_put(session);
	}
	return ret;
}

/*
 * Create and send to the application the created buffers with per PID buffers.
 *
 * Called with UST app session lock held.
 * The session list lock and the session's lock must be acquired.
 *
 * Return 0 on success else a negative value.
 */
static int create_channel_per_pid(struct ust_app *app,
		struct ltt_ust_session *usess, struct ust_app_session *ua_sess,
		struct ust_app_channel *ua_chan)
{
	int ret;
	struct ust_registry_session *registry;
	enum lttng_error_code cmd_ret;
	struct ltt_session *session = NULL;
	uint64_t chan_reg_key;
	struct ust_registry_channel *ust_reg_chan;

	assert(app);
	assert(usess);
	assert(ua_sess);
	assert(ua_chan);

	DBG("UST app creating channel %s with per PID buffers", ua_chan->name);

	rcu_read_lock();

	registry = get_session_registry(ua_sess);
	/* The UST app session lock is held, registry shall not be null. */
	assert(registry);

	/* Create and add a new channel registry to session. */
	ret = ust_registry_channel_add(registry, ua_chan->key);
	if (ret < 0) {
		ERR("Error creating the UST channel \"%s\" registry instance",
			ua_chan->name);
		goto error;
	}

	session = session_find_by_id(ua_sess->tracing_id);
	assert(session);

	assert(pthread_mutex_trylock(&session->lock));
	assert(session_trylock_list());

	/* Create and get channel on the consumer side. */
	ret = do_consumer_create_channel(usess, ua_sess, ua_chan,
			app->bits_per_long, registry,
			session->most_recent_chunk_id.value);
	if (ret < 0) {
		ERR("Error creating UST channel \"%s\" on the consumer daemon",
			ua_chan->name);
		goto error_remove_from_registry;
	}

	ret = send_channel_pid_to_ust(app, ua_sess, ua_chan);
	if (ret < 0) {
		if (ret != -ENOTCONN) {
			ERR("Error sending channel to application");
		}
		goto error_remove_from_registry;
	}

	chan_reg_key = ua_chan->key;
	pthread_mutex_lock(&registry->lock);
	ust_reg_chan = ust_registry_channel_find(registry, chan_reg_key);
	assert(ust_reg_chan);
	ust_reg_chan->consumer_key = ua_chan->key;
	pthread_mutex_unlock(&registry->lock);

	cmd_ret = notification_thread_command_add_channel(
			the_notification_thread_handle, session->name,
			lttng_credentials_get_uid(
					&ua_sess->effective_credentials),
			lttng_credentials_get_gid(
					&ua_sess->effective_credentials),
			ua_chan->name, ua_chan->key, LTTNG_DOMAIN_UST,
			ua_chan->attr.subbuf_size * ua_chan->attr.num_subbuf);
	if (cmd_ret != LTTNG_OK) {
		ret = - (int) cmd_ret;
		ERR("Failed to add channel to notification thread");
		goto error_remove_from_registry;
	}

error_remove_from_registry:
	if (ret) {
		ust_registry_channel_del_free(registry, ua_chan->key, false);
	}
error:
	rcu_read_unlock();
	if (session) {
		session_put(session);
	}
	return ret;
}

/*
 * From an already allocated ust app channel, create the channel buffers if
 * needed and send them to the application. This MUST be called with a RCU read
 * side lock acquired.
 *
 * Called with UST app session lock held.
 *
 * Return 0 on success or else a negative value. Returns -ENOTCONN if
 * the application exited concurrently.
 */
static int ust_app_channel_send(struct ust_app *app,
		struct ltt_ust_session *usess, struct ust_app_session *ua_sess,
		struct ust_app_channel *ua_chan)
{
	int ret;

	assert(app);
	assert(usess);
	assert(usess->active);
	assert(ua_sess);
	assert(ua_chan);

	/* Handle buffer type before sending the channel to the application. */
	switch (usess->buffer_type) {
	case LTTNG_BUFFER_PER_UID:
	{
		ret = create_channel_per_uid(app, usess, ua_sess, ua_chan);
		if (ret < 0) {
			goto error;
		}
		break;
	}
	case LTTNG_BUFFER_PER_PID:
	{
		ret = create_channel_per_pid(app, usess, ua_sess, ua_chan);
		if (ret < 0) {
			goto error;
		}
		break;
	}
	default:
		assert(0);
		ret = -EINVAL;
		goto error;
	}

	/* Initialize ust objd object using the received handle and add it. */
	lttng_ht_node_init_ulong(&ua_chan->ust_objd_node, ua_chan->handle);
	lttng_ht_add_unique_ulong(app->ust_objd, &ua_chan->ust_objd_node);

	/* If channel is not enabled, disable it on the tracer */
	if (!ua_chan->enabled) {
		ret = disable_ust_channel(app, ua_sess, ua_chan);
		if (ret < 0) {
			goto error;
		}
	}

error:
	return ret;
}

/*
 * Create UST app channel and return it through ua_chanp if not NULL.
 *
 * Called with UST app session lock and RCU read-side lock held.
 *
 * Return 0 on success or else a negative value.
 */
static int ust_app_channel_allocate(struct ust_app_session *ua_sess,
		struct ltt_ust_channel *uchan,
		enum lttng_ust_abi_chan_type type, struct ltt_ust_session *usess,
		struct ust_app_channel **ua_chanp)
{
	int ret = 0;
	struct lttng_ht_iter iter;
	struct lttng_ht_node_str *ua_chan_node;
	struct ust_app_channel *ua_chan;

	/* Lookup channel in the ust app session */
	lttng_ht_lookup(ua_sess->channels, (void *)uchan->name, &iter);
	ua_chan_node = lttng_ht_iter_get_node_str(&iter);
	if (ua_chan_node != NULL) {
		ua_chan = caa_container_of(ua_chan_node, struct ust_app_channel, node);
		goto end;
	}

	ua_chan = alloc_ust_app_channel(uchan->name, ua_sess, &uchan->attr);
	if (ua_chan == NULL) {
		/* Only malloc can fail here */
		ret = -ENOMEM;
		goto error;
	}
	shadow_copy_channel(ua_chan, uchan);

	/* Set channel type. */
	ua_chan->attr.type = type;

	/* Only add the channel if successful on the tracer side. */
	lttng_ht_add_unique_str(ua_sess->channels, &ua_chan->node);
end:
	if (ua_chanp) {
		*ua_chanp = ua_chan;
	}

	/* Everything went well. */
	return 0;

error:
	return ret;
}

/*
 * Create UST app event and create it on the tracer side.
 *
 * Must be called with the RCU read side lock held.
 * Called with ust app session mutex held.
 */
static
int create_ust_app_event(struct ust_app_session *ua_sess,
		struct ust_app_channel *ua_chan, struct ltt_ust_event *uevent,
		struct ust_app *app)
{
	int ret = 0;
	struct ust_app_event *ua_event;

	ua_event = alloc_ust_app_event(uevent->attr.name, &uevent->attr);
	if (ua_event == NULL) {
		/* Only failure mode of alloc_ust_app_event(). */
		ret = -ENOMEM;
		goto end;
	}
	shadow_copy_event(ua_event, uevent);

	/* Create it on the tracer side */
	ret = create_ust_event(app, ua_sess, ua_chan, ua_event);
	if (ret < 0) {
		/*
		 * Not found previously means that it does not exist on the
		 * tracer. If the application reports that the event existed,
		 * it means there is a bug in the sessiond or lttng-ust
		 * (or corruption, etc.)
		 */
		if (ret == -LTTNG_UST_ERR_EXIST) {
			ERR("Tracer for application reported that an event being created already existed: "
					"event_name = \"%s\", pid = %d, ppid = %d, uid = %d, gid = %d",
					uevent->attr.name,
					app->pid, app->ppid, app->uid,
					app->gid);
		}
		goto error;
	}

	add_unique_ust_app_event(ua_chan, ua_event);

	DBG2("UST app create event completed: app = '%s' pid = %d",
			app->name, app->pid);

end:
	return ret;

error:
	/* Valid. Calling here is already in a read side lock */
	delete_ust_app_event(-1, ua_event, app);
	return ret;
}

/*
 * Create UST app event notifier rule and create it on the tracer side.
 *
 * Must be called with the RCU read side lock held.
 * Called with ust app session mutex held.
 */
static
int create_ust_app_event_notifier_rule(struct lttng_trigger *trigger,
		struct ust_app *app)
{
	int ret = 0;
	struct ust_app_event_notifier_rule *ua_event_notifier_rule;

	ua_event_notifier_rule = alloc_ust_app_event_notifier_rule(trigger);
	if (ua_event_notifier_rule == NULL) {
		ret = -ENOMEM;
		goto end;
	}

	/* Create it on the tracer side. */
	ret = create_ust_event_notifier(app, ua_event_notifier_rule);
	if (ret < 0) {
		/*
		 * Not found previously means that it does not exist on the
		 * tracer. If the application reports that the event existed,
		 * it means there is a bug in the sessiond or lttng-ust
		 * (or corruption, etc.)
		 */
		if (ret == -LTTNG_UST_ERR_EXIST) {
			ERR("Tracer for application reported that an event notifier being created already exists: "
					"token = \"%" PRIu64 "\", pid = %d, ppid = %d, uid = %d, gid = %d",
					lttng_trigger_get_tracer_token(trigger),
					app->pid, app->ppid, app->uid,
					app->gid);
		}
		goto error;
	}

	lttng_ht_add_unique_u64(app->token_to_event_notifier_rule_ht,
			&ua_event_notifier_rule->node);

	DBG2("UST app create token event rule completed: app = '%s', pid = %d), token = %" PRIu64,
			app->name, app->pid, lttng_trigger_get_tracer_token(trigger));

	goto end;

error:
	/* The RCU read side lock is already being held by the caller. */
	delete_ust_app_event_notifier_rule(-1, ua_event_notifier_rule, app);
end:
	return ret;
}

/*
 * Create UST metadata and open it on the tracer side.
 *
 * Called with UST app session lock held and RCU read side lock.
 */
static int create_ust_app_metadata(struct ust_app_session *ua_sess,
		struct ust_app *app, struct consumer_output *consumer)
{
	int ret = 0;
	struct ust_app_channel *metadata;
	struct consumer_socket *socket;
	struct ust_registry_session *registry;
	struct ltt_session *session = NULL;

	assert(ua_sess);
	assert(app);
	assert(consumer);

	registry = get_session_registry(ua_sess);
	/* The UST app session is held registry shall not be null. */
	assert(registry);

	pthread_mutex_lock(&registry->lock);

	/* Metadata already exists for this registry or it was closed previously */
	if (registry->metadata_key || registry->metadata_closed) {
		ret = 0;
		goto error;
	}

	/* Allocate UST metadata */
	metadata = alloc_ust_app_channel(DEFAULT_METADATA_NAME, ua_sess, NULL);
	if (!metadata) {
		/* malloc() failed */
		ret = -ENOMEM;
		goto error;
	}

	memcpy(&metadata->attr, &ua_sess->metadata_attr, sizeof(metadata->attr));

	/* Need one fd for the channel. */
	ret = lttng_fd_get(LTTNG_FD_APPS, 1);
	if (ret < 0) {
		ERR("Exhausted number of available FD upon create metadata");
		goto error;
	}

	/* Get the right consumer socket for the application. */
	socket = consumer_find_socket_by_bitness(app->bits_per_long, consumer);
	if (!socket) {
		ret = -EINVAL;
		goto error_consumer;
	}

	/*
	 * Keep metadata key so we can identify it on the consumer side. Assign it
	 * to the registry *before* we ask the consumer so we avoid the race of the
	 * consumer requesting the metadata and the ask_channel call on our side
	 * did not returned yet.
	 */
	registry->metadata_key = metadata->key;

	session = session_find_by_id(ua_sess->tracing_id);
	assert(session);

	assert(pthread_mutex_trylock(&session->lock));
	assert(session_trylock_list());

	/*
	 * Ask the metadata channel creation to the consumer. The metadata object
	 * will be created by the consumer and kept their. However, the stream is
	 * never added or monitored until we do a first push metadata to the
	 * consumer.
	 */
	ret = ust_consumer_ask_channel(ua_sess, metadata, consumer, socket,
			registry, session->current_trace_chunk);
	if (ret < 0) {
		/* Nullify the metadata key so we don't try to close it later on. */
		registry->metadata_key = 0;
		goto error_consumer;
	}

	/*
	 * The setup command will make the metadata stream be sent to the relayd,
	 * if applicable, and the thread managing the metadatas. This is important
	 * because after this point, if an error occurs, the only way the stream
	 * can be deleted is to be monitored in the consumer.
	 */
	ret = consumer_setup_metadata(socket, metadata->key);
	if (ret < 0) {
		/* Nullify the metadata key so we don't try to close it later on. */
		registry->metadata_key = 0;
		goto error_consumer;
	}

	DBG2("UST metadata with key %" PRIu64 " created for app pid %d",
			metadata->key, app->pid);

error_consumer:
	lttng_fd_put(LTTNG_FD_APPS, 1);
	delete_ust_app_channel(-1, metadata, app);
error:
	pthread_mutex_unlock(&registry->lock);
	if (session) {
		session_put(session);
	}
	return ret;
}

/*
 * Check if a ust_app with a given pid is present. RCU read side lock MUST be
 * acquired before calling this function.
 */
bool ust_app_with_pid_exists(pid_t pid)
{
	struct lttng_ht_node_ulong *node;
	struct lttng_ht_iter iter;
	bool present = false;

	lttng_ht_lookup(ust_app_ht, (void *)((unsigned long) pid), &iter);
	node = lttng_ht_iter_get_node_ulong(&iter);
	if (node == NULL) {
		DBG2("UST app not found with pid %d", pid);
		goto end;
	}

	present = true;
end:
	return present;
}

/*
 * Allocate and init an UST app object using the registration information and
 * the command socket. This is called when the command socket connects to the
 * session daemon.
 *
 * The object is returned on success or else NULL.
 */
struct ust_app *ust_app_create(struct ust_register_msg *msg, int sock)
{
	int ret;
	struct ust_app *lta = NULL;
	struct lttng_pipe *event_notifier_event_source_pipe = NULL;

	assert(msg);
	assert(sock >= 0);

	DBG3("UST app creating application for socket %d", sock);

	if ((msg->bits_per_long == 64 &&
			    (uatomic_read(&the_ust_consumerd64_fd) ==
					    -EINVAL)) ||
			(msg->bits_per_long == 32 &&
					(uatomic_read(&the_ust_consumerd32_fd) ==
							-EINVAL))) {
		ERR("Registration failed: application \"%s\" (pid: %d) has "
				"%d-bit long, but no consumerd for this size is available.\n",
				msg->name, msg->pid, msg->bits_per_long);
		goto error;
	}

	/*
	 * Reserve the two file descriptors of the event source pipe. The write
	 * end will be closed once it is passed to the application, at which
	 * point a single 'put' will be performed.
	 */
	ret = lttng_fd_get(LTTNG_FD_APPS, 2);
	if (ret) {
		ERR("Failed to reserve two file descriptors for the event source pipe while creating a new application instance: app = '%s', pid = %d",
				msg->name, (int) msg->pid);
		goto error;
	}

	event_notifier_event_source_pipe = lttng_pipe_open(FD_CLOEXEC);
	if (!event_notifier_event_source_pipe) {
		PERROR("Failed to open application event source pipe: '%s' (pid = %d)",
				msg->name, msg->pid);
		goto error;
	}

	lta = zmalloc(sizeof(struct ust_app));
	if (lta == NULL) {
		PERROR("malloc");
		goto error_free_pipe;
	}

	lta->event_notifier_group.event_pipe = event_notifier_event_source_pipe;

	lta->ppid = msg->ppid;
	lta->uid = msg->uid;
	lta->gid = msg->gid;

	lta->bits_per_long = msg->bits_per_long;
	lta->uint8_t_alignment = msg->uint8_t_alignment;
	lta->uint16_t_alignment = msg->uint16_t_alignment;
	lta->uint32_t_alignment = msg->uint32_t_alignment;
	lta->uint64_t_alignment = msg->uint64_t_alignment;
	lta->long_alignment = msg->long_alignment;
	lta->byte_order = msg->byte_order;

	lta->v_major = msg->major;
	lta->v_minor = msg->minor;
	lta->sessions = lttng_ht_new(0, LTTNG_HT_TYPE_U64);
	lta->ust_objd = lttng_ht_new(0, LTTNG_HT_TYPE_ULONG);
	lta->ust_sessions_objd = lttng_ht_new(0, LTTNG_HT_TYPE_ULONG);
	lta->notify_sock = -1;
	lta->token_to_event_notifier_rule_ht = lttng_ht_new(0, LTTNG_HT_TYPE_U64);

	/* Copy name and make sure it's NULL terminated. */
	strncpy(lta->name, msg->name, sizeof(lta->name));
	lta->name[UST_APP_PROCNAME_LEN] = '\0';

	/*
	 * Before this can be called, when receiving the registration information,
	 * the application compatibility is checked. So, at this point, the
	 * application can work with this session daemon.
	 */
	lta->compatible = 1;

	lta->pid = msg->pid;
	lttng_ht_node_init_ulong(&lta->pid_n, (unsigned long) lta->pid);
	lta->sock = sock;
	pthread_mutex_init(&lta->sock_lock, NULL);
	lttng_ht_node_init_ulong(&lta->sock_n, (unsigned long) lta->sock);

	CDS_INIT_LIST_HEAD(&lta->teardown_head);
	return lta;

error_free_pipe:
	lttng_pipe_destroy(event_notifier_event_source_pipe);
	lttng_fd_put(LTTNG_FD_APPS, 2);
error:
	return NULL;
}

/*
 * For a given application object, add it to every hash table.
 */
void ust_app_add(struct ust_app *app)
{
	assert(app);
	assert(app->notify_sock >= 0);

	app->registration_time = time(NULL);

	rcu_read_lock();

	/*
	 * Accept duplicate pid to accommodate the possibility of multiple
	 * lttng-ust per process. Both lttng-ust instance will register
	 * themselves and be unique in term of socket.
	 * All operations on pid should expects that multiple "app" be present
	 * with the same pid.
	 */
	lttng_ht_add_ulong(ust_app_ht, &app->pid_n);

	/*
	 * The socket _should_ be unique until _we_ call close. So, a add_unique
	 * for the ust_app_ht_by_sock is used which asserts fail if the entry was
	 * already in the table.
	 */
	lttng_ht_add_unique_ulong(ust_app_ht_by_sock, &app->sock_n);

	/* Add application to the notify socket hash table. */
	lttng_ht_node_init_ulong(&app->notify_sock_n, app->notify_sock);
	lttng_ht_add_unique_ulong(ust_app_ht_by_notify_sock, &app->notify_sock_n);

	DBG("App registered with pid:%d ppid:%d uid:%d gid:%d sock =%d name:%s "
			"notify_sock =%d (version %d.%d)", app->pid, app->ppid, app->uid,
			app->gid, app->sock, app->name, app->notify_sock, app->v_major,
			app->v_minor);

	rcu_read_unlock();
}

/*
 * Set the application version into the object.
 *
 * Return 0 on success else a negative value either an errno code or a
 * LTTng-UST error code.
 */
int ust_app_version(struct ust_app *app)
{
	int ret;

	assert(app);

	pthread_mutex_lock(&app->sock_lock);
	ret = lttng_ust_ctl_tracer_version(app->sock, &app->version);
	pthread_mutex_unlock(&app->sock_lock);
	if (ret < 0) {
		if (ret == -LTTNG_UST_ERR_EXITING || ret == -EPIPE) {
			DBG3("UST app version failed. Application is dead: pid = %d, sock = %d",
					app->pid, app->sock);
		} else if (ret == -EAGAIN) {
			WARN("UST app version failed. Communication time out: pid = %d, sock = %d",
					app->pid, app->sock);
		} else {
			ERR("UST app version failed with ret %d: pid = %d, sock = %d",
					ret, app->pid, app->sock);
		}
	}

	return ret;
}

bool ust_app_supports_notifiers(const struct ust_app *app)
{
	return app->v_major >= 9;
}

bool ust_app_supports_counters(const struct ust_app *app)
{
	return app->v_major >= 9;
}

/*
 * Setup the base event notifier group.
 *
 * Return 0 on success else a negative value either an errno code or a
 * LTTng-UST error code.
 */
int ust_app_setup_event_notifier_group(struct ust_app *app)
{
	int ret;
	int event_pipe_write_fd;
	struct lttng_ust_abi_object_data *event_notifier_group = NULL;
	enum lttng_error_code lttng_ret;
	enum event_notifier_error_accounting_status event_notifier_error_accounting_status;

	assert(app);

	if (!ust_app_supports_notifiers(app)) {
		ret = -ENOSYS;
		goto error;
	}

	/* Get the write side of the pipe. */
	event_pipe_write_fd = lttng_pipe_get_writefd(
			app->event_notifier_group.event_pipe);

	pthread_mutex_lock(&app->sock_lock);
	ret = lttng_ust_ctl_create_event_notifier_group(app->sock,
			event_pipe_write_fd, &event_notifier_group);
	pthread_mutex_unlock(&app->sock_lock);
	if (ret < 0) {
		if (ret == -EPIPE || ret == -LTTNG_UST_ERR_EXITING) {
			ret = 0;
			DBG3("UST app create event notifier group failed. Application is dead: pid = %d, sock = %d",
					app->pid, app->sock);
		} else if (ret == -EAGAIN) {
			ret = 0;
			WARN("UST app create event notifier group failed. Communication time out: pid = %d, sock = %d",
					app->pid, app->sock);
		} else {
			ERR("UST app create event notifier group failed with ret %d: pid = %d, sock = %d, event_pipe_write_fd: %d",
					ret, app->pid, app->sock, event_pipe_write_fd);
		}
		goto error;
	}

	ret = lttng_pipe_write_close(app->event_notifier_group.event_pipe);
	if (ret) {
		ERR("Failed to close write end of the application's event source pipe: app = '%s' (pid = %d)",
				app->name, app->pid);
		goto error;
	}

	/*
	 * Release the file descriptor that was reserved for the write-end of
	 * the pipe.
	 */
	lttng_fd_put(LTTNG_FD_APPS, 1);

	lttng_ret = notification_thread_command_add_tracer_event_source(
			the_notification_thread_handle,
			lttng_pipe_get_readfd(
					app->event_notifier_group.event_pipe),
			LTTNG_DOMAIN_UST);
	if (lttng_ret != LTTNG_OK) {
		ERR("Failed to add tracer event source to notification thread");
		ret = - 1;
		goto error;
	}

	/* Assign handle only when the complete setup is valid. */
	app->event_notifier_group.object = event_notifier_group;

	event_notifier_error_accounting_status =
			event_notifier_error_accounting_register_app(app);
	switch (event_notifier_error_accounting_status) {
	case EVENT_NOTIFIER_ERROR_ACCOUNTING_STATUS_OK:
		break;
	case EVENT_NOTIFIER_ERROR_ACCOUNTING_STATUS_UNSUPPORTED:
		DBG3("Failed to setup event notifier error accounting (application does not support notifier error accounting): app socket fd = %d, app name = '%s', app pid = %d",
				app->sock, app->name, (int) app->pid);
		ret = 0;
		goto error_accounting;
	case EVENT_NOTIFIER_ERROR_ACCOUNTING_STATUS_APP_DEAD:
		DBG3("Failed to setup event notifier error accounting (application is dead): app socket fd = %d, app name = '%s', app pid = %d",
				app->sock, app->name, (int) app->pid);
		ret = 0;
		goto error_accounting;
	default:
		ERR("Failed to setup event notifier error accounting for app");
		ret = -1;
		goto error_accounting;
	}

	return ret;

error_accounting:
	lttng_ret = notification_thread_command_remove_tracer_event_source(
			the_notification_thread_handle,
			lttng_pipe_get_readfd(
					app->event_notifier_group.event_pipe));
	if (lttng_ret != LTTNG_OK) {
		ERR("Failed to remove application tracer event source from notification thread");
	}

error:
	lttng_ust_ctl_release_object(app->sock, app->event_notifier_group.object);
	free(app->event_notifier_group.object);
	app->event_notifier_group.object = NULL;
	return ret;
}

/*
 * Unregister app by removing it from the global traceable app list and freeing
 * the data struct.
 *
 * The socket is already closed at this point so no close to sock.
 */
void ust_app_unregister(int sock)
{
	struct ust_app *lta;
	struct lttng_ht_node_ulong *node;
	struct lttng_ht_iter ust_app_sock_iter;
	struct lttng_ht_iter iter;
	struct ust_app_session *ua_sess;
	int ret;

	rcu_read_lock();

	/* Get the node reference for a call_rcu */
	lttng_ht_lookup(ust_app_ht_by_sock, (void *)((unsigned long) sock), &ust_app_sock_iter);
	node = lttng_ht_iter_get_node_ulong(&ust_app_sock_iter);
	assert(node);

	lta = caa_container_of(node, struct ust_app, sock_n);
	DBG("PID %d unregistering with sock %d", lta->pid, sock);

	/*
	 * For per-PID buffers, perform "push metadata" and flush all
	 * application streams before removing app from hash tables,
	 * ensuring proper behavior of data_pending check.
	 * Remove sessions so they are not visible during deletion.
	 */
	cds_lfht_for_each_entry(lta->sessions->ht, &iter.iter, ua_sess,
			node.node) {
		struct ust_registry_session *registry;

		ret = lttng_ht_del(lta->sessions, &iter);
		if (ret) {
			/* The session was already removed so scheduled for teardown. */
			continue;
		}

		if (ua_sess->buffer_type == LTTNG_BUFFER_PER_PID) {
			(void) ust_app_flush_app_session(lta, ua_sess);
		}

		/*
		 * Add session to list for teardown. This is safe since at this point we
		 * are the only one using this list.
		 */
		pthread_mutex_lock(&ua_sess->lock);

		if (ua_sess->deleted) {
			pthread_mutex_unlock(&ua_sess->lock);
			continue;
		}

		/*
		 * Normally, this is done in the delete session process which is
		 * executed in the call rcu below. However, upon registration we can't
		 * afford to wait for the grace period before pushing data or else the
		 * data pending feature can race between the unregistration and stop
		 * command where the data pending command is sent *before* the grace
		 * period ended.
		 *
		 * The close metadata below nullifies the metadata pointer in the
		 * session so the delete session will NOT push/close a second time.
		 */
		registry = get_session_registry(ua_sess);
		if (registry) {
			/* Push metadata for application before freeing the application. */
			(void) push_metadata(registry, ua_sess->consumer);

			/*
			 * Don't ask to close metadata for global per UID buffers. Close
			 * metadata only on destroy trace session in this case. Also, the
			 * previous push metadata could have flag the metadata registry to
			 * close so don't send a close command if closed.
			 */
			if (ua_sess->buffer_type != LTTNG_BUFFER_PER_UID) {
				/* And ask to close it for this session registry. */
				(void) close_metadata(registry, ua_sess->consumer);
			}
		}
		cds_list_add(&ua_sess->teardown_node, &lta->teardown_head);

		pthread_mutex_unlock(&ua_sess->lock);
	}

	/* Remove application from PID hash table */
	ret = lttng_ht_del(ust_app_ht_by_sock, &ust_app_sock_iter);
	assert(!ret);

	/*
	 * Remove application from notify hash table. The thread handling the
	 * notify socket could have deleted the node so ignore on error because
	 * either way it's valid. The close of that socket is handled by the
	 * apps_notify_thread.
	 */
	iter.iter.node = &lta->notify_sock_n.node;
	(void) lttng_ht_del(ust_app_ht_by_notify_sock, &iter);

	iter.iter.node = &lta->pid_n.node;
	ret = lttng_ht_del(ust_app_ht, &iter);
	assert(!ret);

	/* Free memory */
	call_rcu(&lta->pid_n.head, delete_ust_app_rcu);

	rcu_read_unlock();
	return;
}

/*
 * Fill events array with all events name of all registered apps.
 */
int ust_app_list_events(struct lttng_event **events)
{
	int ret, handle;
	size_t nbmem, count = 0;
	struct lttng_ht_iter iter;
	struct ust_app *app;
	struct lttng_event *tmp_event;

	nbmem = UST_APP_EVENT_LIST_SIZE;
	tmp_event = zmalloc(nbmem * sizeof(struct lttng_event));
	if (tmp_event == NULL) {
		PERROR("zmalloc ust app events");
		ret = -ENOMEM;
		goto error;
	}

	rcu_read_lock();

	cds_lfht_for_each_entry(ust_app_ht->ht, &iter.iter, app, pid_n.node) {
		struct lttng_ust_abi_tracepoint_iter uiter;

		health_code_update();

		if (!app->compatible) {
			/*
			 * TODO: In time, we should notice the caller of this error by
			 * telling him that this is a version error.
			 */
			continue;
		}
		pthread_mutex_lock(&app->sock_lock);
		handle = lttng_ust_ctl_tracepoint_list(app->sock);
		if (handle < 0) {
			if (handle != -EPIPE && handle != -LTTNG_UST_ERR_EXITING) {
				ERR("UST app list events getting handle failed for app pid %d",
						app->pid);
			}
			pthread_mutex_unlock(&app->sock_lock);
			continue;
		}

		while ((ret = lttng_ust_ctl_tracepoint_list_get(app->sock, handle,
					&uiter)) != -LTTNG_UST_ERR_NOENT) {
			/* Handle ustctl error. */
			if (ret < 0) {
				int release_ret;

				if (ret != -LTTNG_UST_ERR_EXITING && ret != -EPIPE) {
					ERR("UST app tp list get failed for app %d with ret %d",
							app->sock, ret);
				} else {
					DBG3("UST app tp list get failed. Application is dead");
					break;
				}
				free(tmp_event);
				release_ret = lttng_ust_ctl_release_handle(app->sock, handle);
				if (release_ret < 0 &&
						release_ret != -LTTNG_UST_ERR_EXITING &&
						release_ret != -EPIPE) {
					ERR("Error releasing app handle for app %d with ret %d", app->sock, release_ret);
				}
				pthread_mutex_unlock(&app->sock_lock);
				goto rcu_error;
			}

			health_code_update();
			if (count >= nbmem) {
				/* In case the realloc fails, we free the memory */
				struct lttng_event *new_tmp_event;
				size_t new_nbmem;

				new_nbmem = nbmem << 1;
				DBG2("Reallocating event list from %zu to %zu entries",
						nbmem, new_nbmem);
				new_tmp_event = realloc(tmp_event,
					new_nbmem * sizeof(struct lttng_event));
				if (new_tmp_event == NULL) {
					int release_ret;

					PERROR("realloc ust app events");
					free(tmp_event);
					ret = -ENOMEM;
					release_ret = lttng_ust_ctl_release_handle(app->sock, handle);
					if (release_ret < 0 &&
							release_ret != -LTTNG_UST_ERR_EXITING &&
							release_ret != -EPIPE) {
						ERR("Error releasing app handle for app %d with ret %d", app->sock, release_ret);
					}
					pthread_mutex_unlock(&app->sock_lock);
					goto rcu_error;
				}
				/* Zero the new memory */
				memset(new_tmp_event + nbmem, 0,
					(new_nbmem - nbmem) * sizeof(struct lttng_event));
				nbmem = new_nbmem;
				tmp_event = new_tmp_event;
			}
			memcpy(tmp_event[count].name, uiter.name, LTTNG_UST_ABI_SYM_NAME_LEN);
			tmp_event[count].loglevel = uiter.loglevel;
			tmp_event[count].type = (enum lttng_event_type) LTTNG_UST_ABI_TRACEPOINT;
			tmp_event[count].pid = app->pid;
			tmp_event[count].enabled = -1;
			count++;
		}
		ret = lttng_ust_ctl_release_handle(app->sock, handle);
		pthread_mutex_unlock(&app->sock_lock);
		if (ret < 0) {
			if (ret == -EPIPE || ret == -LTTNG_UST_ERR_EXITING) {
				DBG3("Error releasing app handle. Application died: pid = %d, sock = %d",
						app->pid, app->sock);
			} else if (ret == -EAGAIN) {
				WARN("Error releasing app handle. Communication time out: pid = %d, sock = %d",
						app->pid, app->sock);
			} else {
				ERR("Error releasing app handle with ret %d: pid = %d, sock = %d",
						ret, app->pid, app->sock);
			}
		}
	}

	ret = count;
	*events = tmp_event;

	DBG2("UST app list events done (%zu events)", count);

rcu_error:
	rcu_read_unlock();
error:
	health_code_update();
	return ret;
}

/*
 * Fill events array with all events name of all registered apps.
 */
int ust_app_list_event_fields(struct lttng_event_field **fields)
{
	int ret, handle;
	size_t nbmem, count = 0;
	struct lttng_ht_iter iter;
	struct ust_app *app;
	struct lttng_event_field *tmp_event;

	nbmem = UST_APP_EVENT_LIST_SIZE;
	tmp_event = zmalloc(nbmem * sizeof(struct lttng_event_field));
	if (tmp_event == NULL) {
		PERROR("zmalloc ust app event fields");
		ret = -ENOMEM;
		goto error;
	}

	rcu_read_lock();

	cds_lfht_for_each_entry(ust_app_ht->ht, &iter.iter, app, pid_n.node) {
		struct lttng_ust_abi_field_iter uiter;

		health_code_update();

		if (!app->compatible) {
			/*
			 * TODO: In time, we should notice the caller of this error by
			 * telling him that this is a version error.
			 */
			continue;
		}
		pthread_mutex_lock(&app->sock_lock);
		handle = lttng_ust_ctl_tracepoint_field_list(app->sock);
		if (handle < 0) {
			if (handle != -EPIPE && handle != -LTTNG_UST_ERR_EXITING) {
				ERR("UST app list field getting handle failed for app pid %d",
						app->pid);
			}
			pthread_mutex_unlock(&app->sock_lock);
			continue;
		}

		while ((ret = lttng_ust_ctl_tracepoint_field_list_get(app->sock, handle,
					&uiter)) != -LTTNG_UST_ERR_NOENT) {
			/* Handle ustctl error. */
			if (ret < 0) {
				int release_ret;

				if (ret != -LTTNG_UST_ERR_EXITING && ret != -EPIPE) {
					ERR("UST app tp list field failed for app %d with ret %d",
							app->sock, ret);
				} else {
					DBG3("UST app tp list field failed. Application is dead");
					break;
				}
				free(tmp_event);
				release_ret = lttng_ust_ctl_release_handle(app->sock, handle);
				pthread_mutex_unlock(&app->sock_lock);
				if (release_ret < 0 &&
						release_ret != -LTTNG_UST_ERR_EXITING &&
						release_ret != -EPIPE) {
					ERR("Error releasing app handle for app %d with ret %d", app->sock, release_ret);
				}
				goto rcu_error;
			}

			health_code_update();
			if (count >= nbmem) {
				/* In case the realloc fails, we free the memory */
				struct lttng_event_field *new_tmp_event;
				size_t new_nbmem;

				new_nbmem = nbmem << 1;
				DBG2("Reallocating event field list from %zu to %zu entries",
						nbmem, new_nbmem);
				new_tmp_event = realloc(tmp_event,
					new_nbmem * sizeof(struct lttng_event_field));
				if (new_tmp_event == NULL) {
					int release_ret;

					PERROR("realloc ust app event fields");
					free(tmp_event);
					ret = -ENOMEM;
					release_ret = lttng_ust_ctl_release_handle(app->sock, handle);
					pthread_mutex_unlock(&app->sock_lock);
					if (release_ret &&
							release_ret != -LTTNG_UST_ERR_EXITING &&
							release_ret != -EPIPE) {
						ERR("Error releasing app handle for app %d with ret %d", app->sock, release_ret);
					}
					goto rcu_error;
				}
				/* Zero the new memory */
				memset(new_tmp_event + nbmem, 0,
					(new_nbmem - nbmem) * sizeof(struct lttng_event_field));
				nbmem = new_nbmem;
				tmp_event = new_tmp_event;
			}

			memcpy(tmp_event[count].field_name, uiter.field_name, LTTNG_UST_ABI_SYM_NAME_LEN);
			/* Mapping between these enums matches 1 to 1. */
			tmp_event[count].type = (enum lttng_event_field_type) uiter.type;
			tmp_event[count].nowrite = uiter.nowrite;

			memcpy(tmp_event[count].event.name, uiter.event_name, LTTNG_UST_ABI_SYM_NAME_LEN);
			tmp_event[count].event.loglevel = uiter.loglevel;
			tmp_event[count].event.type = LTTNG_EVENT_TRACEPOINT;
			tmp_event[count].event.pid = app->pid;
			tmp_event[count].event.enabled = -1;
			count++;
		}
		ret = lttng_ust_ctl_release_handle(app->sock, handle);
		pthread_mutex_unlock(&app->sock_lock);
		if (ret < 0 &&
				ret != -LTTNG_UST_ERR_EXITING &&
				ret != -EPIPE) {
			ERR("Error releasing app handle for app %d with ret %d", app->sock, ret);
		}
	}

	ret = count;
	*fields = tmp_event;

	DBG2("UST app list event fields done (%zu events)", count);

rcu_error:
	rcu_read_unlock();
error:
	health_code_update();
	return ret;
}

/*
 * Free and clean all traceable apps of the global list.
 *
 * Should _NOT_ be called with RCU read-side lock held.
 */
void ust_app_clean_list(void)
{
	int ret;
	struct ust_app *app;
	struct lttng_ht_iter iter;

	DBG2("UST app cleaning registered apps hash table");

	rcu_read_lock();

	/* Cleanup notify socket hash table */
	if (ust_app_ht_by_notify_sock) {
		cds_lfht_for_each_entry(ust_app_ht_by_notify_sock->ht, &iter.iter, app,
				notify_sock_n.node) {
			/*
			 * Assert that all notifiers are gone as all triggers
			 * are unregistered prior to this clean-up.
			 */
			assert(lttng_ht_get_count(app->token_to_event_notifier_rule_ht) == 0);

			ust_app_notify_sock_unregister(app->notify_sock);
		}
	}

	if (ust_app_ht) {
		cds_lfht_for_each_entry(ust_app_ht->ht, &iter.iter, app, pid_n.node) {
			ret = lttng_ht_del(ust_app_ht, &iter);
			assert(!ret);
			call_rcu(&app->pid_n.head, delete_ust_app_rcu);
		}
	}

	/* Cleanup socket hash table */
	if (ust_app_ht_by_sock) {
		cds_lfht_for_each_entry(ust_app_ht_by_sock->ht, &iter.iter, app,
				sock_n.node) {
			ret = lttng_ht_del(ust_app_ht_by_sock, &iter);
			assert(!ret);
		}
	}

	rcu_read_unlock();

	/* Destroy is done only when the ht is empty */
	if (ust_app_ht) {
		ht_cleanup_push(ust_app_ht);
	}
	if (ust_app_ht_by_sock) {
		ht_cleanup_push(ust_app_ht_by_sock);
	}
	if (ust_app_ht_by_notify_sock) {
		ht_cleanup_push(ust_app_ht_by_notify_sock);
	}
}

/*
 * Init UST app hash table.
 */
int ust_app_ht_alloc(void)
{
	ust_app_ht = lttng_ht_new(0, LTTNG_HT_TYPE_ULONG);
	if (!ust_app_ht) {
		return -1;
	}
	ust_app_ht_by_sock = lttng_ht_new(0, LTTNG_HT_TYPE_ULONG);
	if (!ust_app_ht_by_sock) {
		return -1;
	}
	ust_app_ht_by_notify_sock = lttng_ht_new(0, LTTNG_HT_TYPE_ULONG);
	if (!ust_app_ht_by_notify_sock) {
		return -1;
	}
	return 0;
}

/*
 * For a specific UST session, disable the channel for all registered apps.
 */
int ust_app_disable_channel_glb(struct ltt_ust_session *usess,
		struct ltt_ust_channel *uchan)
{
	int ret = 0;
	struct lttng_ht_iter iter;
	struct lttng_ht_node_str *ua_chan_node;
	struct ust_app *app;
	struct ust_app_session *ua_sess;
	struct ust_app_channel *ua_chan;

	assert(usess->active);
	DBG2("UST app disabling channel %s from global domain for session id %" PRIu64,
			uchan->name, usess->id);

	rcu_read_lock();

	/* For every registered applications */
	cds_lfht_for_each_entry(ust_app_ht->ht, &iter.iter, app, pid_n.node) {
		struct lttng_ht_iter uiter;
		if (!app->compatible) {
			/*
			 * TODO: In time, we should notice the caller of this error by
			 * telling him that this is a version error.
			 */
			continue;
		}
		ua_sess = lookup_session_by_app(usess, app);
		if (ua_sess == NULL) {
			continue;
		}

		/* Get channel */
		lttng_ht_lookup(ua_sess->channels, (void *)uchan->name, &uiter);
		ua_chan_node = lttng_ht_iter_get_node_str(&uiter);
		/* If the session if found for the app, the channel must be there */
		assert(ua_chan_node);

		ua_chan = caa_container_of(ua_chan_node, struct ust_app_channel, node);
		/* The channel must not be already disabled */
		assert(ua_chan->enabled == 1);

		/* Disable channel onto application */
		ret = disable_ust_app_channel(ua_sess, ua_chan, app);
		if (ret < 0) {
			/* XXX: We might want to report this error at some point... */
			continue;
		}
	}

	rcu_read_unlock();
	return ret;
}

/*
 * For a specific UST session, enable the channel for all registered apps.
 */
int ust_app_enable_channel_glb(struct ltt_ust_session *usess,
		struct ltt_ust_channel *uchan)
{
	int ret = 0;
	struct lttng_ht_iter iter;
	struct ust_app *app;
	struct ust_app_session *ua_sess;

	assert(usess->active);
	DBG2("UST app enabling channel %s to global domain for session id %" PRIu64,
			uchan->name, usess->id);

	rcu_read_lock();

	/* For every registered applications */
	cds_lfht_for_each_entry(ust_app_ht->ht, &iter.iter, app, pid_n.node) {
		if (!app->compatible) {
			/*
			 * TODO: In time, we should notice the caller of this error by
			 * telling him that this is a version error.
			 */
			continue;
		}
		ua_sess = lookup_session_by_app(usess, app);
		if (ua_sess == NULL) {
			continue;
		}

		/* Enable channel onto application */
		ret = enable_ust_app_channel(ua_sess, uchan, app);
		if (ret < 0) {
			/* XXX: We might want to report this error at some point... */
			continue;
		}
	}

	rcu_read_unlock();
	return ret;
}

/*
 * Disable an event in a channel and for a specific session.
 */
int ust_app_disable_event_glb(struct ltt_ust_session *usess,
		struct ltt_ust_channel *uchan, struct ltt_ust_event *uevent)
{
	int ret = 0;
	struct lttng_ht_iter iter, uiter;
	struct lttng_ht_node_str *ua_chan_node;
	struct ust_app *app;
	struct ust_app_session *ua_sess;
	struct ust_app_channel *ua_chan;
	struct ust_app_event *ua_event;

	assert(usess->active);
	DBG("UST app disabling event %s for all apps in channel "
			"%s for session id %" PRIu64,
			uevent->attr.name, uchan->name, usess->id);

	rcu_read_lock();

	/* For all registered applications */
	cds_lfht_for_each_entry(ust_app_ht->ht, &iter.iter, app, pid_n.node) {
		if (!app->compatible) {
			/*
			 * TODO: In time, we should notice the caller of this error by
			 * telling him that this is a version error.
			 */
			continue;
		}
		ua_sess = lookup_session_by_app(usess, app);
		if (ua_sess == NULL) {
			/* Next app */
			continue;
		}

		/* Lookup channel in the ust app session */
		lttng_ht_lookup(ua_sess->channels, (void *)uchan->name, &uiter);
		ua_chan_node = lttng_ht_iter_get_node_str(&uiter);
		if (ua_chan_node == NULL) {
			DBG2("Channel %s not found in session id %" PRIu64 " for app pid %d."
					"Skipping", uchan->name, usess->id, app->pid);
			continue;
		}
		ua_chan = caa_container_of(ua_chan_node, struct ust_app_channel, node);

		ua_event = find_ust_app_event(ua_chan->events, uevent->attr.name,
				uevent->filter, uevent->attr.loglevel,
				uevent->exclusion);
		if (ua_event == NULL) {
			DBG2("Event %s not found in channel %s for app pid %d."
					"Skipping", uevent->attr.name, uchan->name, app->pid);
			continue;
		}

		ret = disable_ust_app_event(ua_sess, ua_event, app);
		if (ret < 0) {
			/* XXX: Report error someday... */
			continue;
		}
	}

	rcu_read_unlock();
	return ret;
}

/* The ua_sess lock must be held by the caller.  */
static
int ust_app_channel_create(struct ltt_ust_session *usess,
		struct ust_app_session *ua_sess,
		struct ltt_ust_channel *uchan, struct ust_app *app,
		struct ust_app_channel **_ua_chan)
{
	int ret = 0;
	struct ust_app_channel *ua_chan = NULL;

	assert(ua_sess);
	ASSERT_LOCKED(ua_sess->lock);

	if (!strncmp(uchan->name, DEFAULT_METADATA_NAME,
		     sizeof(uchan->name))) {
		copy_channel_attr_to_ustctl(&ua_sess->metadata_attr,
			&uchan->attr);
		ret = 0;
	} else {
		struct ltt_ust_context *uctx = NULL;

		/*
		 * Create channel onto application and synchronize its
		 * configuration.
		 */
		ret = ust_app_channel_allocate(ua_sess, uchan,
			LTTNG_UST_ABI_CHAN_PER_CPU, usess,
			&ua_chan);
		if (ret < 0) {
			goto error;
		}

		ret = ust_app_channel_send(app, usess,
			ua_sess, ua_chan);
		if (ret) {
			goto error;
		}

		/* Add contexts. */
		cds_list_for_each_entry(uctx, &uchan->ctx_list, list) {
			ret = create_ust_app_channel_context(ua_chan,
				&uctx->ctx, app);
			if (ret) {
				goto error;
			}
		}
	}

error:
	if (ret < 0) {
		switch (ret) {
		case -ENOTCONN:
			/*
			 * The application's socket is not valid. Either a bad socket
			 * or a timeout on it. We can't inform the caller that for a
			 * specific app, the session failed so lets continue here.
			 */
			ret = 0;	/* Not an error. */
			break;
		case -ENOMEM:
		default:
			break;
		}
	}

	if (ret == 0 && _ua_chan) {
		/*
		 * Only return the application's channel on success. Note
		 * that the channel can still be part of the application's
		 * channel hashtable on error.
		 */
		*_ua_chan = ua_chan;
	}
	return ret;
}

/*
 * Enable event for a specific session and channel on the tracer.
 */
int ust_app_enable_event_glb(struct ltt_ust_session *usess,
		struct ltt_ust_channel *uchan, struct ltt_ust_event *uevent)
{
	int ret = 0;
	struct lttng_ht_iter iter, uiter;
	struct lttng_ht_node_str *ua_chan_node;
	struct ust_app *app;
	struct ust_app_session *ua_sess;
	struct ust_app_channel *ua_chan;
	struct ust_app_event *ua_event;

	assert(usess->active);
	DBG("UST app enabling event %s for all apps for session id %" PRIu64,
			uevent->attr.name, usess->id);

	/*
	 * NOTE: At this point, this function is called only if the session and
	 * channel passed are already created for all apps. and enabled on the
	 * tracer also.
	 */

	rcu_read_lock();

	/* For all registered applications */
	cds_lfht_for_each_entry(ust_app_ht->ht, &iter.iter, app, pid_n.node) {
		if (!app->compatible) {
			/*
			 * TODO: In time, we should notice the caller of this error by
			 * telling him that this is a version error.
			 */
			continue;
		}
		ua_sess = lookup_session_by_app(usess, app);
		if (!ua_sess) {
			/* The application has problem or is probably dead. */
			continue;
		}

		pthread_mutex_lock(&ua_sess->lock);

		if (ua_sess->deleted) {
			pthread_mutex_unlock(&ua_sess->lock);
			continue;
		}

		/* Lookup channel in the ust app session */
		lttng_ht_lookup(ua_sess->channels, (void *)uchan->name, &uiter);
		ua_chan_node = lttng_ht_iter_get_node_str(&uiter);
		/*
		 * It is possible that the channel cannot be found is
		 * the channel/event creation occurs concurrently with
		 * an application exit.
		 */
		if (!ua_chan_node) {
			pthread_mutex_unlock(&ua_sess->lock);
			continue;
		}

		ua_chan = caa_container_of(ua_chan_node, struct ust_app_channel, node);

		/* Get event node */
		ua_event = find_ust_app_event(ua_chan->events, uevent->attr.name,
				uevent->filter, uevent->attr.loglevel, uevent->exclusion);
		if (ua_event == NULL) {
			DBG3("UST app enable event %s not found for app PID %d."
					"Skipping app", uevent->attr.name, app->pid);
			goto next_app;
		}

		ret = enable_ust_app_event(ua_sess, ua_event, app);
		if (ret < 0) {
			pthread_mutex_unlock(&ua_sess->lock);
			goto error;
		}
	next_app:
		pthread_mutex_unlock(&ua_sess->lock);
	}

error:
	rcu_read_unlock();
	return ret;
}

/*
 * For a specific existing UST session and UST channel, creates the event for
 * all registered apps.
 */
int ust_app_create_event_glb(struct ltt_ust_session *usess,
		struct ltt_ust_channel *uchan, struct ltt_ust_event *uevent)
{
	int ret = 0;
	struct lttng_ht_iter iter, uiter;
	struct lttng_ht_node_str *ua_chan_node;
	struct ust_app *app;
	struct ust_app_session *ua_sess;
	struct ust_app_channel *ua_chan;

	assert(usess->active);
	DBG("UST app creating event %s for all apps for session id %" PRIu64,
			uevent->attr.name, usess->id);

	rcu_read_lock();

	/* For all registered applications */
	cds_lfht_for_each_entry(ust_app_ht->ht, &iter.iter, app, pid_n.node) {
		if (!app->compatible) {
			/*
			 * TODO: In time, we should notice the caller of this error by
			 * telling him that this is a version error.
			 */
			continue;
		}
		ua_sess = lookup_session_by_app(usess, app);
		if (!ua_sess) {
			/* The application has problem or is probably dead. */
			continue;
		}

		pthread_mutex_lock(&ua_sess->lock);

		if (ua_sess->deleted) {
			pthread_mutex_unlock(&ua_sess->lock);
			continue;
		}

		/* Lookup channel in the ust app session */
		lttng_ht_lookup(ua_sess->channels, (void *)uchan->name, &uiter);
		ua_chan_node = lttng_ht_iter_get_node_str(&uiter);
		/* If the channel is not found, there is a code flow error */
		assert(ua_chan_node);

		ua_chan = caa_container_of(ua_chan_node, struct ust_app_channel, node);

		ret = create_ust_app_event(ua_sess, ua_chan, uevent, app);
		pthread_mutex_unlock(&ua_sess->lock);
		if (ret < 0) {
			if (ret != -LTTNG_UST_ERR_EXIST) {
				/* Possible value at this point: -ENOMEM. If so, we stop! */
				break;
			}
			DBG2("UST app event %s already exist on app PID %d",
					uevent->attr.name, app->pid);
			continue;
		}
	}

	rcu_read_unlock();
	return ret;
}

/*
 * Start tracing for a specific UST session and app.
 *
 * Called with UST app session lock held.
 *
 */
static
int ust_app_start_trace(struct ltt_ust_session *usess, struct ust_app *app)
{
	int ret = 0;
	struct ust_app_session *ua_sess;

	DBG("Starting tracing for ust app pid %d", app->pid);

	rcu_read_lock();

	if (!app->compatible) {
		goto end;
	}

	ua_sess = lookup_session_by_app(usess, app);
	if (ua_sess == NULL) {
		/* The session is in teardown process. Ignore and continue. */
		goto end;
	}

	pthread_mutex_lock(&ua_sess->lock);

	if (ua_sess->deleted) {
		pthread_mutex_unlock(&ua_sess->lock);
		goto end;
	}

	if (ua_sess->enabled) {
		pthread_mutex_unlock(&ua_sess->lock);
		goto end;
	}

	/* Upon restart, we skip the setup, already done */
	if (ua_sess->started) {
		goto skip_setup;
	}

	health_code_update();

skip_setup:
	/* This starts the UST tracing */
	pthread_mutex_lock(&app->sock_lock);
	ret = lttng_ust_ctl_start_session(app->sock, ua_sess->handle);
	pthread_mutex_unlock(&app->sock_lock);
	if (ret < 0) {
		if (ret == -EPIPE || ret == -LTTNG_UST_ERR_EXITING) {
			DBG3("UST app start session failed. Application is dead: pid = %d, sock = %d",
					app->pid, app->sock);
			pthread_mutex_unlock(&ua_sess->lock);
			goto end;
		} else if (ret == -EAGAIN) {
			WARN("UST app start session failed. Communication time out: pid = %d, sock = %d",
					app->pid, app->sock);
			pthread_mutex_unlock(&ua_sess->lock);
			goto end;

		} else {
			ERR("UST app start session failed with ret %d: pid = %d, sock = %d",
					ret, app->pid, app->sock);
		}
		goto error_unlock;
	}

	/* Indicate that the session has been started once */
	ua_sess->started = 1;
	ua_sess->enabled = 1;

	pthread_mutex_unlock(&ua_sess->lock);

	health_code_update();

	/* Quiescent wait after starting trace */
	pthread_mutex_lock(&app->sock_lock);
	ret = lttng_ust_ctl_wait_quiescent(app->sock);
	pthread_mutex_unlock(&app->sock_lock);
	if (ret < 0) {
		if (ret == -EPIPE || ret == -LTTNG_UST_ERR_EXITING) {
			DBG3("UST app wait quiescent failed. Application is dead: pid = %d, sock = %d",
					app->pid, app->sock);
		} else if (ret == -EAGAIN) {
			WARN("UST app wait quiescent failed. Communication time out: pid =  %d, sock = %d",
					app->pid, app->sock);
		} else {
			ERR("UST app wait quiescent failed with ret %d: pid %d, sock = %d",
					ret, app->pid, app->sock);
		}
	}

end:
	rcu_read_unlock();
	health_code_update();
	return 0;

error_unlock:
	pthread_mutex_unlock(&ua_sess->lock);
	rcu_read_unlock();
	health_code_update();
	return -1;
}

/*
 * Stop tracing for a specific UST session and app.
 */
static
int ust_app_stop_trace(struct ltt_ust_session *usess, struct ust_app *app)
{
	int ret = 0;
	struct ust_app_session *ua_sess;
	struct ust_registry_session *registry;

	DBG("Stopping tracing for ust app pid %d", app->pid);

	rcu_read_lock();

	if (!app->compatible) {
		goto end_no_session;
	}

	ua_sess = lookup_session_by_app(usess, app);
	if (ua_sess == NULL) {
		goto end_no_session;
	}

	pthread_mutex_lock(&ua_sess->lock);

	if (ua_sess->deleted) {
		pthread_mutex_unlock(&ua_sess->lock);
		goto end_no_session;
	}

	/*
	 * If started = 0, it means that stop trace has been called for a session
	 * that was never started. It's possible since we can have a fail start
	 * from either the application manager thread or the command thread. Simply
	 * indicate that this is a stop error.
	 */
	if (!ua_sess->started) {
		goto error_rcu_unlock;
	}

	health_code_update();

	/* This inhibits UST tracing */
	pthread_mutex_lock(&app->sock_lock);
	ret = lttng_ust_ctl_stop_session(app->sock, ua_sess->handle);
	pthread_mutex_unlock(&app->sock_lock);
	if (ret < 0) {
		if (ret == -EPIPE || ret == -LTTNG_UST_ERR_EXITING) {
			DBG3("UST app stop session failed. Application is dead: pid = %d, sock = %d",
					app->pid, app->sock);
			goto end_unlock;
		} else if (ret == -EAGAIN) {
			WARN("UST app stop session failed. Communication time out: pid = %d, sock = %d",
					app->pid, app->sock);
			goto end_unlock;

		} else {
			ERR("UST app stop session failed with ret %d: pid = %d, sock = %d",
					ret, app->pid, app->sock);
		}
		goto error_rcu_unlock;
	}

	health_code_update();
	ua_sess->enabled = 0;

	/* Quiescent wait after stopping trace */
	pthread_mutex_lock(&app->sock_lock);
	ret = lttng_ust_ctl_wait_quiescent(app->sock);
	pthread_mutex_unlock(&app->sock_lock);
	if (ret < 0) {
		if (ret == -EPIPE || ret == -LTTNG_UST_ERR_EXITING) {
			DBG3("UST app wait quiescent failed. Application is dead: pid= %d, sock = %d)",
					app->pid, app->sock);
		} else if (ret == -EAGAIN) {
			WARN("UST app wait quiescent failed. Communication time out: pid= %d, sock = %d)",
					app->pid, app->sock);
		} else {
			ERR("UST app wait quiescent failed with ret %d: pid= %d, sock = %d)",
					ret, app->pid, app->sock);
		}
	}

	health_code_update();

	registry = get_session_registry(ua_sess);

	/* The UST app session is held registry shall not be null. */
	assert(registry);

	/* Push metadata for application before freeing the application. */
	(void) push_metadata(registry, ua_sess->consumer);

end_unlock:
	pthread_mutex_unlock(&ua_sess->lock);
end_no_session:
	rcu_read_unlock();
	health_code_update();
	return 0;

error_rcu_unlock:
	pthread_mutex_unlock(&ua_sess->lock);
	rcu_read_unlock();
	health_code_update();
	return -1;
}

static
int ust_app_flush_app_session(struct ust_app *app,
		struct ust_app_session *ua_sess)
{
	int ret, retval = 0;
	struct lttng_ht_iter iter;
	struct ust_app_channel *ua_chan;
	struct consumer_socket *socket;

	DBG("Flushing app session buffers for ust app pid %d", app->pid);

	rcu_read_lock();

	if (!app->compatible) {
		goto end_not_compatible;
	}

	pthread_mutex_lock(&ua_sess->lock);

	if (ua_sess->deleted) {
		goto end_deleted;
	}

	health_code_update();

	/* Flushing buffers */
	socket = consumer_find_socket_by_bitness(app->bits_per_long,
			ua_sess->consumer);

	/* Flush buffers and push metadata. */
	switch (ua_sess->buffer_type) {
	case LTTNG_BUFFER_PER_PID:
		cds_lfht_for_each_entry(ua_sess->channels->ht, &iter.iter, ua_chan,
				node.node) {
			health_code_update();
			ret = consumer_flush_channel(socket, ua_chan->key);
			if (ret) {
				ERR("Error flushing consumer channel");
				retval = -1;
				continue;
			}
		}
		break;
	case LTTNG_BUFFER_PER_UID:
	default:
		assert(0);
		break;
	}

	health_code_update();

end_deleted:
	pthread_mutex_unlock(&ua_sess->lock);

end_not_compatible:
	rcu_read_unlock();
	health_code_update();
	return retval;
}

/*
 * Flush buffers for all applications for a specific UST session.
 * Called with UST session lock held.
 */
static
int ust_app_flush_session(struct ltt_ust_session *usess)

{
	int ret = 0;

	DBG("Flushing session buffers for all ust apps");

	rcu_read_lock();

	/* Flush buffers and push metadata. */
	switch (usess->buffer_type) {
	case LTTNG_BUFFER_PER_UID:
	{
		struct buffer_reg_uid *reg;
		struct lttng_ht_iter iter;

		/* Flush all per UID buffers associated to that session. */
		cds_list_for_each_entry(reg, &usess->buffer_reg_uid_list, lnode) {
			struct ust_registry_session *ust_session_reg;
			struct buffer_reg_channel *buf_reg_chan;
			struct consumer_socket *socket;

			/* Get consumer socket to use to push the metadata.*/
			socket = consumer_find_socket_by_bitness(reg->bits_per_long,
					usess->consumer);
			if (!socket) {
				/* Ignore request if no consumer is found for the session. */
				continue;
			}

			cds_lfht_for_each_entry(reg->registry->channels->ht, &iter.iter,
					buf_reg_chan, node.node) {
				/*
				 * The following call will print error values so the return
				 * code is of little importance because whatever happens, we
				 * have to try them all.
				 */
				(void) consumer_flush_channel(socket, buf_reg_chan->consumer_key);
			}

			ust_session_reg = reg->registry->reg.ust;
			/* Push metadata. */
			(void) push_metadata(ust_session_reg, usess->consumer);
		}
		break;
	}
	case LTTNG_BUFFER_PER_PID:
	{
		struct ust_app_session *ua_sess;
		struct lttng_ht_iter iter;
		struct ust_app *app;

		cds_lfht_for_each_entry(ust_app_ht->ht, &iter.iter, app, pid_n.node) {
			ua_sess = lookup_session_by_app(usess, app);
			if (ua_sess == NULL) {
				continue;
			}
			(void) ust_app_flush_app_session(app, ua_sess);
		}
		break;
	}
	default:
		ret = -1;
		assert(0);
		break;
	}

	rcu_read_unlock();
	health_code_update();
	return ret;
}

static
int ust_app_clear_quiescent_app_session(struct ust_app *app,
		struct ust_app_session *ua_sess)
{
	int ret = 0;
	struct lttng_ht_iter iter;
	struct ust_app_channel *ua_chan;
	struct consumer_socket *socket;

	DBG("Clearing stream quiescent state for ust app pid %d", app->pid);

	rcu_read_lock();

	if (!app->compatible) {
		goto end_not_compatible;
	}

	pthread_mutex_lock(&ua_sess->lock);

	if (ua_sess->deleted) {
		goto end_unlock;
	}

	health_code_update();

	socket = consumer_find_socket_by_bitness(app->bits_per_long,
			ua_sess->consumer);
	if (!socket) {
		ERR("Failed to find consumer (%" PRIu32 ") socket",
				app->bits_per_long);
		ret = -1;
		goto end_unlock;
	}

	/* Clear quiescent state. */
	switch (ua_sess->buffer_type) {
	case LTTNG_BUFFER_PER_PID:
		cds_lfht_for_each_entry(ua_sess->channels->ht, &iter.iter,
				ua_chan, node.node) {
			health_code_update();
			ret = consumer_clear_quiescent_channel(socket,
					ua_chan->key);
			if (ret) {
				ERR("Error clearing quiescent state for consumer channel");
				ret = -1;
				continue;
			}
		}
		break;
	case LTTNG_BUFFER_PER_UID:
	default:
		assert(0);
		ret = -1;
		break;
	}

	health_code_update();

end_unlock:
	pthread_mutex_unlock(&ua_sess->lock);

end_not_compatible:
	rcu_read_unlock();
	health_code_update();
	return ret;
}

/*
 * Clear quiescent state in each stream for all applications for a
 * specific UST session.
 * Called with UST session lock held.
 */
static
int ust_app_clear_quiescent_session(struct ltt_ust_session *usess)

{
	int ret = 0;

	DBG("Clearing stream quiescent state for all ust apps");

	rcu_read_lock();

	switch (usess->buffer_type) {
	case LTTNG_BUFFER_PER_UID:
	{
		struct lttng_ht_iter iter;
		struct buffer_reg_uid *reg;

		/*
		 * Clear quiescent for all per UID buffers associated to
		 * that session.
		 */
		cds_list_for_each_entry(reg, &usess->buffer_reg_uid_list, lnode) {
			struct consumer_socket *socket;
			struct buffer_reg_channel *buf_reg_chan;

			/* Get associated consumer socket.*/
			socket = consumer_find_socket_by_bitness(
					reg->bits_per_long, usess->consumer);
			if (!socket) {
				/*
				 * Ignore request if no consumer is found for
				 * the session.
				 */
				continue;
			}

			cds_lfht_for_each_entry(reg->registry->channels->ht,
					&iter.iter, buf_reg_chan, node.node) {
				/*
				 * The following call will print error values so
				 * the return code is of little importance
				 * because whatever happens, we have to try them
				 * all.
				 */
				(void) consumer_clear_quiescent_channel(socket,
						buf_reg_chan->consumer_key);
			}
		}
		break;
	}
	case LTTNG_BUFFER_PER_PID:
	{
		struct ust_app_session *ua_sess;
		struct lttng_ht_iter iter;
		struct ust_app *app;

		cds_lfht_for_each_entry(ust_app_ht->ht, &iter.iter, app,
				pid_n.node) {
			ua_sess = lookup_session_by_app(usess, app);
			if (ua_sess == NULL) {
				continue;
			}
			(void) ust_app_clear_quiescent_app_session(app,
					ua_sess);
		}
		break;
	}
	default:
		ret = -1;
		assert(0);
		break;
	}

	rcu_read_unlock();
	health_code_update();
	return ret;
}

/*
 * Destroy a specific UST session in apps.
 */
static int destroy_trace(struct ltt_ust_session *usess, struct ust_app *app)
{
	int ret;
	struct ust_app_session *ua_sess;
	struct lttng_ht_iter iter;
	struct lttng_ht_node_u64 *node;

	DBG("Destroy tracing for ust app pid %d", app->pid);

	rcu_read_lock();

	if (!app->compatible) {
		goto end;
	}

	__lookup_session_by_app(usess, app, &iter);
	node = lttng_ht_iter_get_node_u64(&iter);
	if (node == NULL) {
		/* Session is being or is deleted. */
		goto end;
	}
	ua_sess = caa_container_of(node, struct ust_app_session, node);

	health_code_update();
	destroy_app_session(app, ua_sess);

	health_code_update();

	/* Quiescent wait after stopping trace */
	pthread_mutex_lock(&app->sock_lock);
	ret = lttng_ust_ctl_wait_quiescent(app->sock);
	pthread_mutex_unlock(&app->sock_lock);
	if (ret < 0) {
		if (ret == -EPIPE || ret == -LTTNG_UST_ERR_EXITING) {
			DBG3("UST app wait quiescent failed. Application is dead: pid= %d, sock = %d)",
					app->pid, app->sock);
		} else if (ret == -EAGAIN) {
			WARN("UST app wait quiescent failed. Communication time out: pid= %d, sock = %d)",
					app->pid, app->sock);
		} else {
			ERR("UST app wait quiescent failed with ret %d: pid= %d, sock = %d)",
					ret, app->pid, app->sock);
		}
	}
end:
	rcu_read_unlock();
	health_code_update();
	return 0;
}

/*
 * Start tracing for the UST session.
 */
int ust_app_start_trace_all(struct ltt_ust_session *usess)
{
	struct lttng_ht_iter iter;
	struct ust_app *app;

	DBG("Starting all UST traces");

	/*
	 * Even though the start trace might fail, flag this session active so
	 * other application coming in are started by default.
	 */
	usess->active = 1;

	rcu_read_lock();

	/*
	 * In a start-stop-start use-case, we need to clear the quiescent state
	 * of each channel set by the prior stop command, thus ensuring that a
	 * following stop or destroy is sure to grab a timestamp_end near those
	 * operations, even if the packet is empty.
	 */
	(void) ust_app_clear_quiescent_session(usess);

	cds_lfht_for_each_entry(ust_app_ht->ht, &iter.iter, app, pid_n.node) {
		ust_app_global_update(usess, app);
	}

	rcu_read_unlock();

	return 0;
}

/*
 * Start tracing for the UST session.
 * Called with UST session lock held.
 */
int ust_app_stop_trace_all(struct ltt_ust_session *usess)
{
	int ret = 0;
	struct lttng_ht_iter iter;
	struct ust_app *app;

	DBG("Stopping all UST traces");

	/*
	 * Even though the stop trace might fail, flag this session inactive so
	 * other application coming in are not started by default.
	 */
	usess->active = 0;

	rcu_read_lock();

	cds_lfht_for_each_entry(ust_app_ht->ht, &iter.iter, app, pid_n.node) {
		ret = ust_app_stop_trace(usess, app);
		if (ret < 0) {
			/* Continue to next apps even on error */
			continue;
		}
	}

	(void) ust_app_flush_session(usess);

	rcu_read_unlock();

	return 0;
}

/*
 * Destroy app UST session.
 */
int ust_app_destroy_trace_all(struct ltt_ust_session *usess)
{
	int ret = 0;
	struct lttng_ht_iter iter;
	struct ust_app *app;

	DBG("Destroy all UST traces");

	rcu_read_lock();

	cds_lfht_for_each_entry(ust_app_ht->ht, &iter.iter, app, pid_n.node) {
		ret = destroy_trace(usess, app);
		if (ret < 0) {
			/* Continue to next apps even on error */
			continue;
		}
	}

	rcu_read_unlock();

	return 0;
}

/* The ua_sess lock must be held by the caller. */
static
int find_or_create_ust_app_channel(
		struct ltt_ust_session *usess,
		struct ust_app_session *ua_sess,
		struct ust_app *app,
		struct ltt_ust_channel *uchan,
		struct ust_app_channel **ua_chan)
{
	int ret = 0;
	struct lttng_ht_iter iter;
	struct lttng_ht_node_str *ua_chan_node;

	lttng_ht_lookup(ua_sess->channels, (void *) uchan->name, &iter);
	ua_chan_node = lttng_ht_iter_get_node_str(&iter);
	if (ua_chan_node) {
		*ua_chan = caa_container_of(ua_chan_node,
			struct ust_app_channel, node);
		goto end;
	}

	ret = ust_app_channel_create(usess, ua_sess, uchan, app, ua_chan);
	if (ret) {
		goto end;
	}
end:
	return ret;
}

static
int ust_app_channel_synchronize_event(struct ust_app_channel *ua_chan,
		struct ltt_ust_event *uevent, struct ust_app_session *ua_sess,
		struct ust_app *app)
{
	int ret = 0;
	struct ust_app_event *ua_event = NULL;

	ua_event = find_ust_app_event(ua_chan->events, uevent->attr.name,
		uevent->filter, uevent->attr.loglevel, uevent->exclusion);
	if (!ua_event) {
		ret = create_ust_app_event(ua_sess, ua_chan, uevent, app);
		if (ret < 0) {
			goto end;
		}
	} else {
		if (ua_event->enabled != uevent->enabled) {
			ret = uevent->enabled ?
				enable_ust_app_event(ua_sess, ua_event, app) :
				disable_ust_app_event(ua_sess, ua_event, app);
		}
	}

end:
	return ret;
}

/* Called with RCU read-side lock held. */
static
void ust_app_synchronize_event_notifier_rules(struct ust_app *app)
{
	int ret = 0;
	enum lttng_error_code ret_code;
	enum lttng_trigger_status t_status;
	struct lttng_ht_iter app_trigger_iter;
	struct lttng_triggers *triggers = NULL;
	struct ust_app_event_notifier_rule *event_notifier_rule;
	unsigned int count, i;

	if (!ust_app_supports_notifiers(app)) {
		goto end;
	}

	/*
	 * Currrently, registering or unregistering a trigger with an
	 * event rule condition causes a full synchronization of the event
	 * notifiers.
	 *
	 * The first step attempts to add an event notifier for all registered
	 * triggers that apply to the user space tracers. Then, the
	 * application's event notifiers rules are all checked against the list
	 * of registered triggers. Any event notifier that doesn't have a
	 * matching trigger can be assumed to have been disabled.
	 *
	 * All of this is inefficient, but is put in place to get the feature
	 * rolling as it is simpler at this moment. It will be optimized Soon™
	 * to allow the state of enabled
	 * event notifiers to be synchronized in a piece-wise way.
	 */

	/* Get all triggers using uid 0 (root) */
	ret_code = notification_thread_command_list_triggers(
			the_notification_thread_handle, 0, &triggers);
	if (ret_code != LTTNG_OK) {
		goto end;
	}

	assert(triggers);

	t_status = lttng_triggers_get_count(triggers, &count);
	if (t_status != LTTNG_TRIGGER_STATUS_OK) {
		goto end;
	}

	for (i = 0; i < count; i++) {
		struct lttng_condition *condition;
		struct lttng_event_rule *event_rule;
		struct lttng_trigger *trigger;
		const struct ust_app_event_notifier_rule *looked_up_event_notifier_rule;
		enum lttng_condition_status condition_status;
		uint64_t token;

		trigger = lttng_triggers_borrow_mutable_at_index(triggers, i);
		assert(trigger);

		token = lttng_trigger_get_tracer_token(trigger);
		condition = lttng_trigger_get_condition(trigger);

		if (lttng_condition_get_type(condition) !=
				LTTNG_CONDITION_TYPE_EVENT_RULE_MATCHES) {
			/* Does not apply */
			continue;
		}

		condition_status =
				lttng_condition_event_rule_matches_borrow_rule_mutable(
						condition, &event_rule);
		assert(condition_status == LTTNG_CONDITION_STATUS_OK);

		if (lttng_event_rule_get_domain_type(event_rule) == LTTNG_DOMAIN_KERNEL) {
			/* Skip kernel related triggers. */
			continue;
		}

		/*
		 * Find or create the associated token event rule. The caller
		 * holds the RCU read lock, so this is safe to call without
		 * explicitly acquiring it here.
		 */
		looked_up_event_notifier_rule = find_ust_app_event_notifier_rule(
				app->token_to_event_notifier_rule_ht, token);
		if (!looked_up_event_notifier_rule) {
			ret = create_ust_app_event_notifier_rule(trigger, app);
			if (ret < 0) {
				goto end;
			}
		}
	}

	rcu_read_lock();
	/* Remove all unknown event sources from the app. */
	cds_lfht_for_each_entry (app->token_to_event_notifier_rule_ht->ht,
			&app_trigger_iter.iter, event_notifier_rule,
			node.node) {
		const uint64_t app_token = event_notifier_rule->token;
		bool found = false;

		/*
		 * Check if the app event trigger still exists on the
		 * notification side.
		 */
		for (i = 0; i < count; i++) {
			uint64_t notification_thread_token;
			const struct lttng_trigger *trigger =
					lttng_triggers_get_at_index(
							triggers, i);

			assert(trigger);

			notification_thread_token =
					lttng_trigger_get_tracer_token(trigger);

			if (notification_thread_token == app_token) {
				found = true;
				break;
			}
		}

		if (found) {
			/* Still valid. */
			continue;
		}

		/*
		 * This trigger was unregistered, disable it on the tracer's
		 * side.
		 */
		ret = lttng_ht_del(app->token_to_event_notifier_rule_ht,
				&app_trigger_iter);
		assert(ret == 0);

		/* Callee logs errors. */
		(void) disable_ust_object(app, event_notifier_rule->obj);

		delete_ust_app_event_notifier_rule(
				app->sock, event_notifier_rule, app);
	}

	rcu_read_unlock();

end:
	lttng_triggers_destroy(triggers);
	return;
}

/*
 * RCU read lock must be held by the caller.
 */
static
void ust_app_synchronize_all_channels(struct ltt_ust_session *usess,
		struct ust_app_session *ua_sess,
		struct ust_app *app)
{
	int ret = 0;
	struct cds_lfht_iter uchan_iter;
	struct ltt_ust_channel *uchan;

	assert(usess);
	assert(ua_sess);
	assert(app);

	cds_lfht_for_each_entry(usess->domain_global.channels->ht, &uchan_iter,
			uchan, node.node) {
		struct ust_app_channel *ua_chan;
		struct cds_lfht_iter uevent_iter;
		struct ltt_ust_event *uevent;

		/*
		 * Search for a matching ust_app_channel. If none is found,
		 * create it. Creating the channel will cause the ua_chan
		 * structure to be allocated, the channel buffers to be
		 * allocated (if necessary) and sent to the application, and
		 * all enabled contexts will be added to the channel.
		 */
		ret = find_or_create_ust_app_channel(usess, ua_sess,
			app, uchan, &ua_chan);
		if (ret) {
			/* Tracer is probably gone or ENOMEM. */
			goto end;
		}

		if (!ua_chan) {
			/* ua_chan will be NULL for the metadata channel */
			continue;
		}

		cds_lfht_for_each_entry(uchan->events->ht, &uevent_iter, uevent,
				node.node) {
			ret = ust_app_channel_synchronize_event(ua_chan,
				uevent, ua_sess, app);
			if (ret) {
				goto end;
			}
		}

		if (ua_chan->enabled != uchan->enabled) {
			ret = uchan->enabled ?
				enable_ust_app_channel(ua_sess, uchan, app) :
				disable_ust_app_channel(ua_sess, ua_chan, app);
			if (ret) {
				goto end;
			}
		}
	}
end:
	return;
}

/*
 * The caller must ensure that the application is compatible and is tracked
 * by the process attribute trackers.
 */
static
void ust_app_synchronize(struct ltt_ust_session *usess,
		struct ust_app *app)
{
	int ret = 0;
	struct ust_app_session *ua_sess = NULL;

	/*
	 * The application's configuration should only be synchronized for
	 * active sessions.
	 */
	assert(usess->active);

	ret = find_or_create_ust_app_session(usess, app, &ua_sess, NULL);
	if (ret < 0) {
		/* Tracer is probably gone or ENOMEM. */
		if (ua_sess) {
			destroy_app_session(app, ua_sess);
		}
		goto end;
	}
	assert(ua_sess);

	pthread_mutex_lock(&ua_sess->lock);
	if (ua_sess->deleted) {
		goto deleted_session;
	}

	rcu_read_lock();

	ust_app_synchronize_all_channels(usess, ua_sess, app);

	/*
	 * Create the metadata for the application. This returns gracefully if a
	 * metadata was already set for the session.
	 *
	 * The metadata channel must be created after the data channels as the
	 * consumer daemon assumes this ordering. When interacting with a relay
	 * daemon, the consumer will use this assumption to send the
	 * "STREAMS_SENT" message to the relay daemon.
	 */
	ret = create_ust_app_metadata(ua_sess, app, usess->consumer);
	if (ret < 0) {
		ERR("Metadata creation failed for app sock %d for session id %" PRIu64,
				app->sock, usess->id);
	}

	rcu_read_unlock();

deleted_session:
	pthread_mutex_unlock(&ua_sess->lock);
end:
	return;
}

static
void ust_app_global_destroy(struct ltt_ust_session *usess, struct ust_app *app)
{
	struct ust_app_session *ua_sess;

	ua_sess = lookup_session_by_app(usess, app);
	if (ua_sess == NULL) {
		return;
	}
	destroy_app_session(app, ua_sess);
}

/*
 * Add channels/events from UST global domain to registered apps at sock.
 *
 * Called with session lock held.
 * Called with RCU read-side lock held.
 */
void ust_app_global_update(struct ltt_ust_session *usess, struct ust_app *app)
{
	assert(usess);
	assert(usess->active);

	DBG2("UST app global update for app sock %d for session id %" PRIu64,
			app->sock, usess->id);

	if (!app->compatible) {
		return;
	}
	if (trace_ust_id_tracker_lookup(LTTNG_PROCESS_ATTR_VIRTUAL_PROCESS_ID,
			    usess, app->pid) &&
			trace_ust_id_tracker_lookup(
					LTTNG_PROCESS_ATTR_VIRTUAL_USER_ID,
					usess, app->uid) &&
			trace_ust_id_tracker_lookup(
					LTTNG_PROCESS_ATTR_VIRTUAL_GROUP_ID,
					usess, app->gid)) {
		/*
		 * Synchronize the application's internal tracing configuration
		 * and start tracing.
		 */
		ust_app_synchronize(usess, app);
		ust_app_start_trace(usess, app);
	} else {
		ust_app_global_destroy(usess, app);
	}
}

/*
 * Add all event notifiers to an application.
 *
 * Called with session lock held.
 * Called with RCU read-side lock held.
 */
void ust_app_global_update_event_notifier_rules(struct ust_app *app)
{
	DBG2("UST application global event notifier rules update: app = '%s', pid = %d)",
			app->name, app->pid);

	if (!app->compatible || !ust_app_supports_notifiers(app)) {
		return;
	}

	if (app->event_notifier_group.object == NULL) {
		WARN("UST app global update of event notifiers for app skipped since communication handle is null: app = '%s' pid = %d)",
				app->name, app->pid);
		return;
	}

	ust_app_synchronize_event_notifier_rules(app);
}

/*
 * Called with session lock held.
 */
void ust_app_global_update_all(struct ltt_ust_session *usess)
{
	struct lttng_ht_iter iter;
	struct ust_app *app;

	rcu_read_lock();
	cds_lfht_for_each_entry(ust_app_ht->ht, &iter.iter, app, pid_n.node) {
		ust_app_global_update(usess, app);
	}
	rcu_read_unlock();
}

void ust_app_global_update_all_event_notifier_rules(void)
{
	struct lttng_ht_iter iter;
	struct ust_app *app;

	rcu_read_lock();
	cds_lfht_for_each_entry(ust_app_ht->ht, &iter.iter, app, pid_n.node) {
		ust_app_global_update_event_notifier_rules(app);
	}

	rcu_read_unlock();
}

/*
 * Add context to a specific channel for global UST domain.
 */
int ust_app_add_ctx_channel_glb(struct ltt_ust_session *usess,
		struct ltt_ust_channel *uchan, struct ltt_ust_context *uctx)
{
	int ret = 0;
	struct lttng_ht_node_str *ua_chan_node;
	struct lttng_ht_iter iter, uiter;
	struct ust_app_channel *ua_chan = NULL;
	struct ust_app_session *ua_sess;
	struct ust_app *app;

	assert(usess->active);

	rcu_read_lock();
	cds_lfht_for_each_entry(ust_app_ht->ht, &iter.iter, app, pid_n.node) {
		if (!app->compatible) {
			/*
			 * TODO: In time, we should notice the caller of this error by
			 * telling him that this is a version error.
			 */
			continue;
		}
		ua_sess = lookup_session_by_app(usess, app);
		if (ua_sess == NULL) {
			continue;
		}

		pthread_mutex_lock(&ua_sess->lock);

		if (ua_sess->deleted) {
			pthread_mutex_unlock(&ua_sess->lock);
			continue;
		}

		/* Lookup channel in the ust app session */
		lttng_ht_lookup(ua_sess->channels, (void *)uchan->name, &uiter);
		ua_chan_node = lttng_ht_iter_get_node_str(&uiter);
		if (ua_chan_node == NULL) {
			goto next_app;
		}
		ua_chan = caa_container_of(ua_chan_node, struct ust_app_channel,
				node);
		ret = create_ust_app_channel_context(ua_chan, &uctx->ctx, app);
		if (ret < 0) {
			goto next_app;
		}
	next_app:
		pthread_mutex_unlock(&ua_sess->lock);
	}

	rcu_read_unlock();
	return ret;
}

/*
 * Receive registration and populate the given msg structure.
 *
 * On success return 0 else a negative value returned by the ustctl call.
 */
int ust_app_recv_registration(int sock, struct ust_register_msg *msg)
{
	int ret;
	uint32_t pid, ppid, uid, gid;

	assert(msg);

	ret = lttng_ust_ctl_recv_reg_msg(sock, &msg->type, &msg->major, &msg->minor,
			&pid, &ppid, &uid, &gid,
			&msg->bits_per_long,
			&msg->uint8_t_alignment,
			&msg->uint16_t_alignment,
			&msg->uint32_t_alignment,
			&msg->uint64_t_alignment,
			&msg->long_alignment,
			&msg->byte_order,
			msg->name);
	if (ret < 0) {
		switch (-ret) {
		case EPIPE:
		case ECONNRESET:
		case LTTNG_UST_ERR_EXITING:
			DBG3("UST app recv reg message failed. Application died");
			break;
		case LTTNG_UST_ERR_UNSUP_MAJOR:
			ERR("UST app recv reg unsupported version %d.%d. Supporting %d.%d",
					msg->major, msg->minor, LTTNG_UST_ABI_MAJOR_VERSION,
					LTTNG_UST_ABI_MINOR_VERSION);
			break;
		default:
			ERR("UST app recv reg message failed with ret %d", ret);
			break;
		}
		goto error;
	}
	msg->pid = (pid_t) pid;
	msg->ppid = (pid_t) ppid;
	msg->uid = (uid_t) uid;
	msg->gid = (gid_t) gid;

error:
	return ret;
}

/*
 * Return a ust app session object using the application object and the
 * session object descriptor has a key. If not found, NULL is returned.
 * A RCU read side lock MUST be acquired when calling this function.
*/
static struct ust_app_session *find_session_by_objd(struct ust_app *app,
		int objd)
{
	struct lttng_ht_node_ulong *node;
	struct lttng_ht_iter iter;
	struct ust_app_session *ua_sess = NULL;

	assert(app);

	lttng_ht_lookup(app->ust_sessions_objd, (void *)((unsigned long) objd), &iter);
	node = lttng_ht_iter_get_node_ulong(&iter);
	if (node == NULL) {
		DBG2("UST app session find by objd %d not found", objd);
		goto error;
	}

	ua_sess = caa_container_of(node, struct ust_app_session, ust_objd_node);

error:
	return ua_sess;
}

/*
 * Return a ust app channel object using the application object and the channel
 * object descriptor has a key. If not found, NULL is returned. A RCU read side
 * lock MUST be acquired before calling this function.
 */
static struct ust_app_channel *find_channel_by_objd(struct ust_app *app,
		int objd)
{
	struct lttng_ht_node_ulong *node;
	struct lttng_ht_iter iter;
	struct ust_app_channel *ua_chan = NULL;

	assert(app);

	lttng_ht_lookup(app->ust_objd, (void *)((unsigned long) objd), &iter);
	node = lttng_ht_iter_get_node_ulong(&iter);
	if (node == NULL) {
		DBG2("UST app channel find by objd %d not found", objd);
		goto error;
	}

	ua_chan = caa_container_of(node, struct ust_app_channel, ust_objd_node);

error:
	return ua_chan;
}

/*
 * Fixup legacy context fields for comparison:
 * - legacy array becomes array_nestable,
 * - legacy struct becomes struct_nestable,
 * - legacy variant becomes variant_nestable,
 * legacy sequences are not emitted in LTTng-UST contexts.
 */
static int ust_app_fixup_legacy_context_fields(size_t *_nr_fields,
		struct lttng_ust_ctl_field **_fields)
{
	struct lttng_ust_ctl_field *fields = *_fields, *new_fields = NULL;
	size_t nr_fields = *_nr_fields, new_nr_fields = 0, i, j;
	bool found = false;
	int ret = 0;

	for (i = 0; i < nr_fields; i++) {
		const struct lttng_ust_ctl_field *field = &fields[i];

		switch (field->type.atype) {
		case lttng_ust_ctl_atype_sequence:
			ERR("Unexpected legacy sequence context.");
			ret = -EINVAL;
			goto end;
		case lttng_ust_ctl_atype_array:
			switch (field->type.u.legacy.array.elem_type.atype) {
			case lttng_ust_ctl_atype_integer:
				break;
			default:
				ERR("Unexpected legacy array element type in context.");
				ret = -EINVAL;
				goto end;
			}
			found = true;
			/* One field for array_nested, one field for elem type. */
			new_nr_fields += 2;
			break;

		case lttng_ust_ctl_atype_struct:	/* Fallthrough */
		case lttng_ust_ctl_atype_variant:
			found = true;
			new_nr_fields++;
			break;
		default:
			new_nr_fields++;
			break;
		}
	}
	if (!found) {
		goto end;
	}
	new_fields = (struct lttng_ust_ctl_field *) zmalloc(sizeof(*new_fields) * new_nr_fields);
	if (!new_fields) {
		ret = -ENOMEM;
		goto end;
	}
	for (i = 0, j = 0; i < nr_fields; i++, j++) {
		const struct lttng_ust_ctl_field *field = &fields[i];
		struct lttng_ust_ctl_field *new_field = &new_fields[j];

		switch (field->type.atype) {
		case lttng_ust_ctl_atype_array:
			/* One field for array_nested, one field for elem type. */
			strncpy(new_field->name, field->name, LTTNG_UST_ABI_SYM_NAME_LEN - 1);
			new_field->type.atype = lttng_ust_ctl_atype_array_nestable;
			new_field->type.u.array_nestable.length = field->type.u.legacy.array.length;
			new_field->type.u.array_nestable.alignment = 0;
			new_field = &new_fields[++j];	/* elem type */
			new_field->type.atype = field->type.u.legacy.array.elem_type.atype;
			assert(new_field->type.atype == lttng_ust_ctl_atype_integer);
			new_field->type.u.integer = field->type.u.legacy.array.elem_type.u.basic.integer;
			break;
		case lttng_ust_ctl_atype_struct:
			strncpy(new_field->name, field->name, LTTNG_UST_ABI_SYM_NAME_LEN - 1);
			new_field->type.atype = lttng_ust_ctl_atype_struct_nestable;
			new_field->type.u.struct_nestable.nr_fields = field->type.u.legacy._struct.nr_fields;
			new_field->type.u.struct_nestable.alignment = 0;
			break;
		case lttng_ust_ctl_atype_variant:
			strncpy(new_field->name, field->name, LTTNG_UST_ABI_SYM_NAME_LEN - 1);
			new_field->type.atype = lttng_ust_ctl_atype_variant_nestable;
			new_field->type.u.variant_nestable.nr_choices = field->type.u.legacy.variant.nr_choices;
			strncpy(new_field->type.u.variant_nestable.tag_name,
				field->type.u.legacy.variant.tag_name,
				LTTNG_UST_ABI_SYM_NAME_LEN - 1);
			new_field->type.u.variant_nestable.alignment = 0;
			break;
		default:
			*new_field = *field;
			break;
		}
	}
	free(fields);
	*_fields = new_fields;
	*_nr_fields = new_nr_fields;
end:
	return ret;
}

/*
 * Reply to a register channel notification from an application on the notify
 * socket. The channel metadata is also created.
 *
 * The session UST registry lock is acquired in this function.
 *
 * On success 0 is returned else a negative value.
 */
static int reply_ust_register_channel(int sock, int cobjd,
		size_t nr_fields, struct lttng_ust_ctl_field *fields)
{
	int ret, ret_code = 0;
	uint32_t chan_id;
	uint64_t chan_reg_key;
	enum lttng_ust_ctl_channel_header type = LTTNG_UST_CTL_CHANNEL_HEADER_UNKNOWN;
	struct ust_app *app;
	struct ust_app_channel *ua_chan;
	struct ust_app_session *ua_sess;
	struct ust_registry_session *registry;
	struct ust_registry_channel *ust_reg_chan;

	rcu_read_lock();

	/* Lookup application. If not found, there is a code flow error. */
	app = find_app_by_notify_sock(sock);
	if (!app) {
		DBG("Application socket %d is being torn down. Abort event notify",
				sock);
		ret = -1;
		goto error_rcu_unlock;
	}

	/* Lookup channel by UST object descriptor. */
	ua_chan = find_channel_by_objd(app, cobjd);
	if (!ua_chan) {
		DBG("Application channel is being torn down. Abort event notify");
		ret = 0;
		goto error_rcu_unlock;
	}

	assert(ua_chan->session);
	ua_sess = ua_chan->session;

	/* Get right session registry depending on the session buffer type. */
	registry = get_session_registry(ua_sess);
	if (!registry) {
		DBG("Application session is being torn down. Abort event notify");
		ret = 0;
		goto error_rcu_unlock;
	};

	/* Depending on the buffer type, a different channel key is used. */
	if (ua_sess->buffer_type == LTTNG_BUFFER_PER_UID) {
		chan_reg_key = ua_chan->tracing_channel_id;
	} else {
		chan_reg_key = ua_chan->key;
	}

	pthread_mutex_lock(&registry->lock);

	ust_reg_chan = ust_registry_channel_find(registry, chan_reg_key);
	assert(ust_reg_chan);

	/* Channel id is set during the object creation. */
	chan_id = ust_reg_chan->chan_id;

	ret = ust_app_fixup_legacy_context_fields(&nr_fields, &fields);
	if (ret < 0) {
		ERR("Registering application channel due to legacy context fields fixup error: pid = %d, sock = %d",
			app->pid, app->sock);
		ret_code = -EINVAL;
		goto reply;
	}
	if (!ust_reg_chan->register_done) {
		/*
		 * TODO: eventually use the registry event count for
		 * this channel to better guess header type for per-pid
		 * buffers.
		 */
		type = LTTNG_UST_CTL_CHANNEL_HEADER_LARGE;
		ust_reg_chan->nr_ctx_fields = nr_fields;
		ust_reg_chan->ctx_fields = fields;
		fields = NULL;
		ust_reg_chan->header_type = type;
	} else {
		/* Get current already assigned values. */
		type = ust_reg_chan->header_type;
		/*
		 * Validate that the context fields match between
		 * registry and newcoming application.
		 */
		if (!match_lttng_ust_ctl_field_array(ust_reg_chan->ctx_fields,
				ust_reg_chan->nr_ctx_fields,
				fields, nr_fields)) {
			ERR("Registering application channel due to context field mismatch: pid = %d, sock = %d",
				app->pid, app->sock);
			ret_code = -EINVAL;
			goto reply;
		}
	}

	/* Append to metadata */
	if (!ust_reg_chan->metadata_dumped) {
		ret_code = ust_metadata_channel_statedump(registry, ust_reg_chan);
		if (ret_code) {
			ERR("Error appending channel metadata (errno = %d)", ret_code);
			goto reply;
		}
	}

reply:
	DBG3("UST app replying to register channel key %" PRIu64
			" with id %u, type = %d, ret = %d", chan_reg_key, chan_id, type,
			ret_code);

	ret = lttng_ust_ctl_reply_register_channel(sock, chan_id, type, ret_code);
	if (ret < 0) {
		if (ret == -EPIPE || ret == -LTTNG_UST_ERR_EXITING) {
			DBG3("UST app reply channel failed. Application died: pid = %d, sock = %d",
					app->pid, app->sock);
		} else if (ret == -EAGAIN) {
			WARN("UST app reply channel failed. Communication time out: pid = %d, sock = %d",
					app->pid, app->sock);
		} else {
			ERR("UST app reply channel failed with ret %d: pid = %d, sock = %d",
					ret, app->pid, app->sock);
		}
		goto error;
	}

	/* This channel registry registration is completed. */
	ust_reg_chan->register_done = 1;

error:
	pthread_mutex_unlock(&registry->lock);
error_rcu_unlock:
	rcu_read_unlock();
	free(fields);
	return ret;
}

/*
 * Add event to the UST channel registry. When the event is added to the
 * registry, the metadata is also created. Once done, this replies to the
 * application with the appropriate error code.
 *
 * The session UST registry lock is acquired in the function.
 *
 * On success 0 is returned else a negative value.
 */
static int add_event_ust_registry(int sock, int sobjd, int cobjd, char *name,
		char *sig, size_t nr_fields, struct lttng_ust_ctl_field *fields,
		int loglevel_value, char *model_emf_uri)
{
	int ret, ret_code;
	uint32_t event_id = 0;
	uint64_t chan_reg_key;
	struct ust_app *app;
	struct ust_app_channel *ua_chan;
	struct ust_app_session *ua_sess;
	struct ust_registry_session *registry;

	rcu_read_lock();

	/* Lookup application. If not found, there is a code flow error. */
	app = find_app_by_notify_sock(sock);
	if (!app) {
		DBG("Application socket %d is being torn down. Abort event notify",
				sock);
		ret = -1;
		goto error_rcu_unlock;
	}

	/* Lookup channel by UST object descriptor. */
	ua_chan = find_channel_by_objd(app, cobjd);
	if (!ua_chan) {
		DBG("Application channel is being torn down. Abort event notify");
		ret = 0;
		goto error_rcu_unlock;
	}

	assert(ua_chan->session);
	ua_sess = ua_chan->session;

	registry = get_session_registry(ua_sess);
	if (!registry) {
		DBG("Application session is being torn down. Abort event notify");
		ret = 0;
		goto error_rcu_unlock;
	}

	if (ua_sess->buffer_type == LTTNG_BUFFER_PER_UID) {
		chan_reg_key = ua_chan->tracing_channel_id;
	} else {
		chan_reg_key = ua_chan->key;
	}

	pthread_mutex_lock(&registry->lock);

	/*
	 * From this point on, this call acquires the ownership of the sig, fields
	 * and model_emf_uri meaning any free are done inside it if needed. These
	 * three variables MUST NOT be read/write after this.
	 */
	ret_code = ust_registry_create_event(registry, chan_reg_key,
			sobjd, cobjd, name, sig, nr_fields, fields,
			loglevel_value, model_emf_uri, ua_sess->buffer_type,
			&event_id, app);
	sig = NULL;
	fields = NULL;
	model_emf_uri = NULL;

	/*
	 * The return value is returned to ustctl so in case of an error, the
	 * application can be notified. In case of an error, it's important not to
	 * return a negative error or else the application will get closed.
	 */
	ret = lttng_ust_ctl_reply_register_event(sock, event_id, ret_code);
	if (ret < 0) {
		if (ret == -EPIPE || ret == -LTTNG_UST_ERR_EXITING) {
			DBG3("UST app reply event failed. Application died: pid = %d, sock = %d.",
					app->pid, app->sock);
		} else if (ret == -EAGAIN) {
			WARN("UST app reply event failed. Communication time out: pid = %d, sock = %d",
					app->pid, app->sock);
		} else {
			ERR("UST app reply event failed with ret %d: pid = %d, sock = %d",
					ret, app->pid, app->sock);
		}
		/*
		 * No need to wipe the create event since the application socket will
		 * get close on error hence cleaning up everything by itself.
		 */
		goto error;
	}

	DBG3("UST registry event %s with id %" PRId32 " added successfully",
			name, event_id);

error:
	pthread_mutex_unlock(&registry->lock);
error_rcu_unlock:
	rcu_read_unlock();
	free(sig);
	free(fields);
	free(model_emf_uri);
	return ret;
}

/*
 * Add enum to the UST session registry. Once done, this replies to the
 * application with the appropriate error code.
 *
 * The session UST registry lock is acquired within this function.
 *
 * On success 0 is returned else a negative value.
 */
static int add_enum_ust_registry(int sock, int sobjd, char *name,
		struct lttng_ust_ctl_enum_entry *entries, size_t nr_entries)
{
	int ret = 0, ret_code;
	struct ust_app *app;
	struct ust_app_session *ua_sess;
	struct ust_registry_session *registry;
	uint64_t enum_id = -1ULL;

	rcu_read_lock();

	/* Lookup application. If not found, there is a code flow error. */
	app = find_app_by_notify_sock(sock);
	if (!app) {
		/* Return an error since this is not an error */
		DBG("Application socket %d is being torn down. Aborting enum registration",
				sock);
		free(entries);
		ret = -1;
		goto error_rcu_unlock;
	}

	/* Lookup session by UST object descriptor. */
	ua_sess = find_session_by_objd(app, sobjd);
	if (!ua_sess) {
		/* Return an error since this is not an error */
		DBG("Application session is being torn down (session not found). Aborting enum registration.");
		free(entries);
		goto error_rcu_unlock;
	}

	registry = get_session_registry(ua_sess);
	if (!registry) {
		DBG("Application session is being torn down (registry not found). Aborting enum registration.");
		free(entries);
		goto error_rcu_unlock;
	}

	pthread_mutex_lock(&registry->lock);

	/*
	 * From this point on, the callee acquires the ownership of
	 * entries. The variable entries MUST NOT be read/written after
	 * call.
	 */
	ret_code = ust_registry_create_or_find_enum(registry, sobjd, name,
			entries, nr_entries, &enum_id);
	entries = NULL;

	/*
	 * The return value is returned to ustctl so in case of an error, the
	 * application can be notified. In case of an error, it's important not to
	 * return a negative error or else the application will get closed.
	 */
	ret = lttng_ust_ctl_reply_register_enum(sock, enum_id, ret_code);
	if (ret < 0) {
		if (ret == -EPIPE || ret == -LTTNG_UST_ERR_EXITING) {
			DBG3("UST app reply enum failed. Application died: pid = %d, sock = %d",
					app->pid, app->sock);
		} else if (ret == -EAGAIN) {
			WARN("UST app reply enum failed. Communication time out: pid = %d, sock = %d",
					app->pid, app->sock);
		} else {
			ERR("UST app reply enum failed with ret %d: pid = %d, sock = %d",
					ret, app->pid, app->sock);
		}
		/*
		 * No need to wipe the create enum since the application socket will
		 * get close on error hence cleaning up everything by itself.
		 */
		goto error;
	}

	DBG3("UST registry enum %s added successfully or already found", name);

error:
	pthread_mutex_unlock(&registry->lock);
error_rcu_unlock:
	rcu_read_unlock();
	return ret;
}

/*
 * Handle application notification through the given notify socket.
 *
 * Return 0 on success or else a negative value.
 */
int ust_app_recv_notify(int sock)
{
	int ret;
	enum lttng_ust_ctl_notify_cmd cmd;

	DBG3("UST app receiving notify from sock %d", sock);

	ret = lttng_ust_ctl_recv_notify(sock, &cmd);
	if (ret < 0) {
		if (ret == -EPIPE || ret == -LTTNG_UST_ERR_EXITING) {
			DBG3("UST app recv notify failed. Application died: sock = %d",
					sock);
		} else if (ret == -EAGAIN) {
			WARN("UST app recv notify failed. Communication time out: sock = %d",
					sock);
		} else {
			ERR("UST app recv notify failed with ret %d: sock = %d",
					ret, sock);
		}
		goto error;
	}

	switch (cmd) {
	case LTTNG_UST_CTL_NOTIFY_CMD_EVENT:
	{
		int sobjd, cobjd, loglevel_value;
		char name[LTTNG_UST_ABI_SYM_NAME_LEN], *sig, *model_emf_uri;
		size_t nr_fields;
		struct lttng_ust_ctl_field *fields;

		DBG2("UST app ustctl register event received");

		ret = lttng_ust_ctl_recv_register_event(sock, &sobjd, &cobjd, name,
				&loglevel_value, &sig, &nr_fields, &fields,
				&model_emf_uri);
		if (ret < 0) {
			if (ret == -EPIPE || ret == -LTTNG_UST_ERR_EXITING) {
				DBG3("UST app recv event failed. Application died: sock = %d",
						sock);
			} else if (ret == -EAGAIN) {
				WARN("UST app recv event failed. Communication time out: sock = %d",
						sock);
			} else {
				ERR("UST app recv event failed with ret %d: sock = %d",
						ret, sock);
			}
			goto error;
		}

		/*
		 * Add event to the UST registry coming from the notify socket. This
		 * call will free if needed the sig, fields and model_emf_uri. This
		 * code path loses the ownsership of these variables and transfer them
		 * to the this function.
		 */
		ret = add_event_ust_registry(sock, sobjd, cobjd, name, sig, nr_fields,
				fields, loglevel_value, model_emf_uri);
		if (ret < 0) {
			goto error;
		}

		break;
	}
	case LTTNG_UST_CTL_NOTIFY_CMD_CHANNEL:
	{
		int sobjd, cobjd;
		size_t nr_fields;
		struct lttng_ust_ctl_field *fields;

		DBG2("UST app ustctl register channel received");

		ret = lttng_ust_ctl_recv_register_channel(sock, &sobjd, &cobjd, &nr_fields,
				&fields);
		if (ret < 0) {
			if (ret == -EPIPE || ret == -LTTNG_UST_ERR_EXITING) {
				DBG3("UST app recv channel failed. Application died: sock = %d",
						sock);
			} else if (ret == -EAGAIN) {
				WARN("UST app recv channel failed. Communication time out: sock = %d",
						sock);
			} else {
				ERR("UST app recv channel failed with ret %d: sock = %d)",
						ret, sock);
			}
			goto error;
		}

		/*
		 * The fields ownership are transfered to this function call meaning
		 * that if needed it will be freed. After this, it's invalid to access
		 * fields or clean it up.
		 */
		ret = reply_ust_register_channel(sock, cobjd, nr_fields,
				fields);
		if (ret < 0) {
			goto error;
		}

		break;
	}
	case LTTNG_UST_CTL_NOTIFY_CMD_ENUM:
	{
		int sobjd;
		char name[LTTNG_UST_ABI_SYM_NAME_LEN];
		size_t nr_entries;
		struct lttng_ust_ctl_enum_entry *entries;

		DBG2("UST app ustctl register enum received");

		ret = lttng_ust_ctl_recv_register_enum(sock, &sobjd, name,
				&entries, &nr_entries);
		if (ret < 0) {
			if (ret == -EPIPE || ret == -LTTNG_UST_ERR_EXITING) {
				DBG3("UST app recv enum failed. Application died: sock = %d",
						sock);
			} else if (ret == -EAGAIN) {
				WARN("UST app recv enum failed. Communication time out: sock = %d",
						sock);
			} else {
				ERR("UST app recv enum failed with ret %d: sock = %d",
						ret, sock);
			}
			goto error;
		}

		/* Callee assumes ownership of entries */
		ret = add_enum_ust_registry(sock, sobjd, name,
				entries, nr_entries);
		if (ret < 0) {
			goto error;
		}

		break;
	}
	default:
		/* Should NEVER happen. */
		assert(0);
	}

error:
	return ret;
}

/*
 * Once the notify socket hangs up, this is called. First, it tries to find the
 * corresponding application. On failure, the call_rcu to close the socket is
 * executed. If an application is found, it tries to delete it from the notify
 * socket hash table. Whathever the result, it proceeds to the call_rcu.
 *
 * Note that an object needs to be allocated here so on ENOMEM failure, the
 * call RCU is not done but the rest of the cleanup is.
 */
void ust_app_notify_sock_unregister(int sock)
{
	int err_enomem = 0;
	struct lttng_ht_iter iter;
	struct ust_app *app;
	struct ust_app_notify_sock_obj *obj;

	assert(sock >= 0);

	rcu_read_lock();

	obj = zmalloc(sizeof(*obj));
	if (!obj) {
		/*
		 * An ENOMEM is kind of uncool. If this strikes we continue the
		 * procedure but the call_rcu will not be called. In this case, we
		 * accept the fd leak rather than possibly creating an unsynchronized
		 * state between threads.
		 *
		 * TODO: The notify object should be created once the notify socket is
		 * registered and stored independantely from the ust app object. The
		 * tricky part is to synchronize the teardown of the application and
		 * this notify object. Let's keep that in mind so we can avoid this
		 * kind of shenanigans with ENOMEM in the teardown path.
		 */
		err_enomem = 1;
	} else {
		obj->fd = sock;
	}

	DBG("UST app notify socket unregister %d", sock);

	/*
	 * Lookup application by notify socket. If this fails, this means that the
	 * hash table delete has already been done by the application
	 * unregistration process so we can safely close the notify socket in a
	 * call RCU.
	 */
	app = find_app_by_notify_sock(sock);
	if (!app) {
		goto close_socket;
	}

	iter.iter.node = &app->notify_sock_n.node;

	/*
	 * Whatever happens here either we fail or succeed, in both cases we have
	 * to close the socket after a grace period to continue to the call RCU
	 * here. If the deletion is successful, the application is not visible
	 * anymore by other threads and is it fails it means that it was already
	 * deleted from the hash table so either way we just have to close the
	 * socket.
	 */
	(void) lttng_ht_del(ust_app_ht_by_notify_sock, &iter);

close_socket:
	rcu_read_unlock();

	/*
	 * Close socket after a grace period to avoid for the socket to be reused
	 * before the application object is freed creating potential race between
	 * threads trying to add unique in the global hash table.
	 */
	if (!err_enomem) {
		call_rcu(&obj->head, close_notify_sock_rcu);
	}
}

/*
 * Destroy a ust app data structure and free its memory.
 */
void ust_app_destroy(struct ust_app *app)
{
	if (!app) {
		return;
	}

	call_rcu(&app->pid_n.head, delete_ust_app_rcu);
}

/*
 * Take a snapshot for a given UST session. The snapshot is sent to the given
 * output.
 *
 * Returns LTTNG_OK on success or a LTTNG_ERR error code.
 */
enum lttng_error_code ust_app_snapshot_record(
		const struct ltt_ust_session *usess,
		const struct consumer_output *output, int wait,
		uint64_t nb_packets_per_stream)
{
	int ret = 0;
	enum lttng_error_code status = LTTNG_OK;
	struct lttng_ht_iter iter;
	struct ust_app *app;
	char *trace_path = NULL;

	assert(usess);
	assert(output);

	rcu_read_lock();

	switch (usess->buffer_type) {
	case LTTNG_BUFFER_PER_UID:
	{
		struct buffer_reg_uid *reg;

		cds_list_for_each_entry(reg, &usess->buffer_reg_uid_list, lnode) {
			struct buffer_reg_channel *buf_reg_chan;
			struct consumer_socket *socket;
			char pathname[PATH_MAX];
			size_t consumer_path_offset = 0;

			if (!reg->registry->reg.ust->metadata_key) {
				/* Skip since no metadata is present */
				continue;
			}

			/* Get consumer socket to use to push the metadata.*/
			socket = consumer_find_socket_by_bitness(reg->bits_per_long,
					usess->consumer);
			if (!socket) {
				status = LTTNG_ERR_INVALID;
				goto error;
			}

			memset(pathname, 0, sizeof(pathname));
			ret = snprintf(pathname, sizeof(pathname),
					DEFAULT_UST_TRACE_UID_PATH,
					reg->uid, reg->bits_per_long);
			if (ret < 0) {
				PERROR("snprintf snapshot path");
				status = LTTNG_ERR_INVALID;
				goto error;
			}
			/* Free path allowed on previous iteration. */
			free(trace_path);
			trace_path = setup_channel_trace_path(usess->consumer, pathname,
						&consumer_path_offset);
			if (!trace_path) {
				status = LTTNG_ERR_INVALID;
				goto error;
			}
			/* Add the UST default trace dir to path. */
			cds_lfht_for_each_entry(reg->registry->channels->ht, &iter.iter,
					buf_reg_chan, node.node) {
				status = consumer_snapshot_channel(socket,
						buf_reg_chan->consumer_key,
						output, 0, usess->uid,
						usess->gid, &trace_path[consumer_path_offset], wait,
						nb_packets_per_stream);
				if (status != LTTNG_OK) {
					goto error;
				}
			}
			status = consumer_snapshot_channel(socket,
					reg->registry->reg.ust->metadata_key, output, 1,
					usess->uid, usess->gid, &trace_path[consumer_path_offset],
					wait, 0);
			if (status != LTTNG_OK) {
				goto error;
			}
		}
		break;
	}
	case LTTNG_BUFFER_PER_PID:
	{
		cds_lfht_for_each_entry(ust_app_ht->ht, &iter.iter, app, pid_n.node) {
			struct consumer_socket *socket;
			struct lttng_ht_iter chan_iter;
			struct ust_app_channel *ua_chan;
			struct ust_app_session *ua_sess;
			struct ust_registry_session *registry;
			char pathname[PATH_MAX];
			size_t consumer_path_offset = 0;

			ua_sess = lookup_session_by_app(usess, app);
			if (!ua_sess) {
				/* Session not associated with this app. */
				continue;
			}

			/* Get the right consumer socket for the application. */
			socket = consumer_find_socket_by_bitness(app->bits_per_long,
					output);
			if (!socket) {
				status = LTTNG_ERR_INVALID;
				goto error;
			}

			/* Add the UST default trace dir to path. */
			memset(pathname, 0, sizeof(pathname));
			ret = snprintf(pathname, sizeof(pathname), "%s",
					ua_sess->path);
			if (ret < 0) {
				status = LTTNG_ERR_INVALID;
				PERROR("snprintf snapshot path");
				goto error;
			}
			/* Free path allowed on previous iteration. */
			free(trace_path);
			trace_path = setup_channel_trace_path(usess->consumer, pathname,
					&consumer_path_offset);
			if (!trace_path) {
				status = LTTNG_ERR_INVALID;
				goto error;
			}
			cds_lfht_for_each_entry(ua_sess->channels->ht, &chan_iter.iter,
					ua_chan, node.node) {
				status = consumer_snapshot_channel(socket,
						ua_chan->key, output, 0,
						lttng_credentials_get_uid(&ua_sess->effective_credentials),
						lttng_credentials_get_gid(&ua_sess->effective_credentials),
						&trace_path[consumer_path_offset], wait,
						nb_packets_per_stream);
				switch (status) {
				case LTTNG_OK:
					break;
				case LTTNG_ERR_CHAN_NOT_FOUND:
					continue;
				default:
					goto error;
				}
			}

			registry = get_session_registry(ua_sess);
			if (!registry) {
				DBG("Application session is being torn down. Skip application.");
				continue;
			}
			status = consumer_snapshot_channel(socket,
					registry->metadata_key, output, 1,
					lttng_credentials_get_uid(&ua_sess->effective_credentials),
					lttng_credentials_get_gid(&ua_sess->effective_credentials),
					&trace_path[consumer_path_offset], wait, 0);
			switch (status) {
			case LTTNG_OK:
				break;
			case LTTNG_ERR_CHAN_NOT_FOUND:
				continue;
			default:
				goto error;
			}
		}
		break;
	}
	default:
		assert(0);
		break;
	}

error:
	free(trace_path);
	rcu_read_unlock();
	return status;
}

/*
 * Return the size taken by one more packet per stream.
 */
uint64_t ust_app_get_size_one_more_packet_per_stream(
		const struct ltt_ust_session *usess, uint64_t cur_nr_packets)
{
	uint64_t tot_size = 0;
	struct ust_app *app;
	struct lttng_ht_iter iter;

	assert(usess);

	switch (usess->buffer_type) {
	case LTTNG_BUFFER_PER_UID:
	{
		struct buffer_reg_uid *reg;

		cds_list_for_each_entry(reg, &usess->buffer_reg_uid_list, lnode) {
			struct buffer_reg_channel *buf_reg_chan;

			rcu_read_lock();
			cds_lfht_for_each_entry(reg->registry->channels->ht, &iter.iter,
					buf_reg_chan, node.node) {
				if (cur_nr_packets >= buf_reg_chan->num_subbuf) {
					/*
					 * Don't take channel into account if we
					 * already grab all its packets.
					 */
					continue;
				}
				tot_size += buf_reg_chan->subbuf_size * buf_reg_chan->stream_count;
			}
			rcu_read_unlock();
		}
		break;
	}
	case LTTNG_BUFFER_PER_PID:
	{
		rcu_read_lock();
		cds_lfht_for_each_entry(ust_app_ht->ht, &iter.iter, app, pid_n.node) {
			struct ust_app_channel *ua_chan;
			struct ust_app_session *ua_sess;
			struct lttng_ht_iter chan_iter;

			ua_sess = lookup_session_by_app(usess, app);
			if (!ua_sess) {
				/* Session not associated with this app. */
				continue;
			}

			cds_lfht_for_each_entry(ua_sess->channels->ht, &chan_iter.iter,
					ua_chan, node.node) {
				if (cur_nr_packets >= ua_chan->attr.num_subbuf) {
					/*
					 * Don't take channel into account if we
					 * already grab all its packets.
					 */
					continue;
				}
				tot_size += ua_chan->attr.subbuf_size * ua_chan->streams.count;
			}
		}
		rcu_read_unlock();
		break;
	}
	default:
		assert(0);
		break;
	}

	return tot_size;
}

int ust_app_uid_get_channel_runtime_stats(uint64_t ust_session_id,
		struct cds_list_head *buffer_reg_uid_list,
		struct consumer_output *consumer, uint64_t uchan_id,
		int overwrite, uint64_t *discarded, uint64_t *lost)
{
	int ret;
	uint64_t consumer_chan_key;

	*discarded = 0;
	*lost = 0;

	ret = buffer_reg_uid_consumer_channel_key(
			buffer_reg_uid_list, uchan_id, &consumer_chan_key);
	if (ret < 0) {
		/* Not found */
		ret = 0;
		goto end;
	}

	if (overwrite) {
		ret = consumer_get_lost_packets(ust_session_id,
				consumer_chan_key, consumer, lost);
	} else {
		ret = consumer_get_discarded_events(ust_session_id,
				consumer_chan_key, consumer, discarded);
	}

end:
	return ret;
}

int ust_app_pid_get_channel_runtime_stats(struct ltt_ust_session *usess,
		struct ltt_ust_channel *uchan,
		struct consumer_output *consumer, int overwrite,
		uint64_t *discarded, uint64_t *lost)
{
	int ret = 0;
	struct lttng_ht_iter iter;
	struct lttng_ht_node_str *ua_chan_node;
	struct ust_app *app;
	struct ust_app_session *ua_sess;
	struct ust_app_channel *ua_chan;

	*discarded = 0;
	*lost = 0;

	rcu_read_lock();
	/*
	 * Iterate over every registered applications. Sum counters for
	 * all applications containing requested session and channel.
	 */
	cds_lfht_for_each_entry(ust_app_ht->ht, &iter.iter, app, pid_n.node) {
		struct lttng_ht_iter uiter;

		ua_sess = lookup_session_by_app(usess, app);
		if (ua_sess == NULL) {
			continue;
		}

		/* Get channel */
		lttng_ht_lookup(ua_sess->channels, (void *) uchan->name, &uiter);
		ua_chan_node = lttng_ht_iter_get_node_str(&uiter);
		/* If the session is found for the app, the channel must be there */
		assert(ua_chan_node);

		ua_chan = caa_container_of(ua_chan_node, struct ust_app_channel, node);

		if (overwrite) {
			uint64_t _lost;

			ret = consumer_get_lost_packets(usess->id, ua_chan->key,
					consumer, &_lost);
			if (ret < 0) {
				break;
			}
			(*lost) += _lost;
		} else {
			uint64_t _discarded;

			ret = consumer_get_discarded_events(usess->id,
					ua_chan->key, consumer, &_discarded);
			if (ret < 0) {
				break;
			}
			(*discarded) += _discarded;
		}
	}

	rcu_read_unlock();
	return ret;
}

static
int ust_app_regenerate_statedump(struct ltt_ust_session *usess,
		struct ust_app *app)
{
	int ret = 0;
	struct ust_app_session *ua_sess;

	DBG("Regenerating the metadata for ust app pid %d", app->pid);

	rcu_read_lock();

	ua_sess = lookup_session_by_app(usess, app);
	if (ua_sess == NULL) {
		/* The session is in teardown process. Ignore and continue. */
		goto end;
	}

	pthread_mutex_lock(&ua_sess->lock);

	if (ua_sess->deleted) {
		goto end_unlock;
	}

	pthread_mutex_lock(&app->sock_lock);
	ret = lttng_ust_ctl_regenerate_statedump(app->sock, ua_sess->handle);
	pthread_mutex_unlock(&app->sock_lock);

end_unlock:
	pthread_mutex_unlock(&ua_sess->lock);

end:
	rcu_read_unlock();
	health_code_update();
	return ret;
}

/*
 * Regenerate the statedump for each app in the session.
 */
int ust_app_regenerate_statedump_all(struct ltt_ust_session *usess)
{
	int ret = 0;
	struct lttng_ht_iter iter;
	struct ust_app *app;

	DBG("Regenerating the metadata for all UST apps");

	rcu_read_lock();

	cds_lfht_for_each_entry(ust_app_ht->ht, &iter.iter, app, pid_n.node) {
		if (!app->compatible) {
			continue;
		}

		ret = ust_app_regenerate_statedump(usess, app);
		if (ret < 0) {
			/* Continue to the next app even on error */
			continue;
		}
	}

	rcu_read_unlock();

	return 0;
}

/*
 * Rotate all the channels of a session.
 *
 * Return LTTNG_OK on success or else an LTTng error code.
 */
enum lttng_error_code ust_app_rotate_session(struct ltt_session *session)
{
	int ret;
	enum lttng_error_code cmd_ret = LTTNG_OK;
	struct lttng_ht_iter iter;
	struct ust_app *app;
	struct ltt_ust_session *usess = session->ust_session;

	assert(usess);

	rcu_read_lock();

	switch (usess->buffer_type) {
	case LTTNG_BUFFER_PER_UID:
	{
		struct buffer_reg_uid *reg;

		cds_list_for_each_entry(reg, &usess->buffer_reg_uid_list, lnode) {
			struct buffer_reg_channel *buf_reg_chan;
			struct consumer_socket *socket;

			/* Get consumer socket to use to push the metadata.*/
			socket = consumer_find_socket_by_bitness(reg->bits_per_long,
					usess->consumer);
			if (!socket) {
				cmd_ret = LTTNG_ERR_INVALID;
				goto error;
			}

			/* Rotate the data channels. */
			cds_lfht_for_each_entry(reg->registry->channels->ht, &iter.iter,
					buf_reg_chan, node.node) {
				ret = consumer_rotate_channel(socket,
						buf_reg_chan->consumer_key,
						usess->uid, usess->gid,
						usess->consumer,
						/* is_metadata_channel */ false);
				if (ret < 0) {
					cmd_ret = LTTNG_ERR_ROTATION_FAIL_CONSUMER;
					goto error;
				}
			}

			/*
			 * The metadata channel might not be present.
			 *
			 * Consumer stream allocation can be done
			 * asynchronously and can fail on intermediary
			 * operations (i.e add context) and lead to data
			 * channels created with no metadata channel.
			 */
			if (!reg->registry->reg.ust->metadata_key) {
				/* Skip since no metadata is present. */
				continue;
			}

			(void) push_metadata(reg->registry->reg.ust, usess->consumer);

			ret = consumer_rotate_channel(socket,
					reg->registry->reg.ust->metadata_key,
					usess->uid, usess->gid,
					usess->consumer,
					/* is_metadata_channel */ true);
			if (ret < 0) {
				cmd_ret = LTTNG_ERR_ROTATION_FAIL_CONSUMER;
				goto error;
			}
		}
		break;
	}
	case LTTNG_BUFFER_PER_PID:
	{
		cds_lfht_for_each_entry(ust_app_ht->ht, &iter.iter, app, pid_n.node) {
			struct consumer_socket *socket;
			struct lttng_ht_iter chan_iter;
			struct ust_app_channel *ua_chan;
			struct ust_app_session *ua_sess;
			struct ust_registry_session *registry;

			ua_sess = lookup_session_by_app(usess, app);
			if (!ua_sess) {
				/* Session not associated with this app. */
				continue;
			}

			/* Get the right consumer socket for the application. */
			socket = consumer_find_socket_by_bitness(app->bits_per_long,
					usess->consumer);
			if (!socket) {
				cmd_ret = LTTNG_ERR_INVALID;
				goto error;
			}

			registry = get_session_registry(ua_sess);
			if (!registry) {
				DBG("Application session is being torn down. Skip application.");
				continue;
			}

			/* Rotate the data channels. */
			cds_lfht_for_each_entry(ua_sess->channels->ht, &chan_iter.iter,
					ua_chan, node.node) {
				ret = consumer_rotate_channel(socket,
						ua_chan->key,
						lttng_credentials_get_uid(&ua_sess->effective_credentials),
						lttng_credentials_get_gid(&ua_sess->effective_credentials),
						ua_sess->consumer,
						/* is_metadata_channel */ false);
				if (ret < 0) {
					/* Per-PID buffer and application going away. */
					if (ret == -LTTNG_ERR_CHAN_NOT_FOUND)
						continue;
					cmd_ret = LTTNG_ERR_ROTATION_FAIL_CONSUMER;
					goto error;
				}
			}

			/* Rotate the metadata channel. */
			(void) push_metadata(registry, usess->consumer);
			ret = consumer_rotate_channel(socket,
					registry->metadata_key,
					lttng_credentials_get_uid(&ua_sess->effective_credentials),
					lttng_credentials_get_gid(&ua_sess->effective_credentials),
					ua_sess->consumer,
					/* is_metadata_channel */ true);
			if (ret < 0) {
				/* Per-PID buffer and application going away. */
				if (ret == -LTTNG_ERR_CHAN_NOT_FOUND)
					continue;
				cmd_ret = LTTNG_ERR_ROTATION_FAIL_CONSUMER;
				goto error;
			}
		}
		break;
	}
	default:
		assert(0);
		break;
	}

	cmd_ret = LTTNG_OK;

error:
	rcu_read_unlock();
	return cmd_ret;
}

enum lttng_error_code ust_app_create_channel_subdirectories(
		const struct ltt_ust_session *usess)
{
	enum lttng_error_code ret = LTTNG_OK;
	struct lttng_ht_iter iter;
	enum lttng_trace_chunk_status chunk_status;
	char *pathname_index;
	int fmt_ret;

	assert(usess->current_trace_chunk);
	rcu_read_lock();

	switch (usess->buffer_type) {
	case LTTNG_BUFFER_PER_UID:
	{
		struct buffer_reg_uid *reg;

		cds_list_for_each_entry(reg, &usess->buffer_reg_uid_list, lnode) {
			fmt_ret = asprintf(&pathname_index,
				       DEFAULT_UST_TRACE_DIR "/" DEFAULT_UST_TRACE_UID_PATH "/" DEFAULT_INDEX_DIR,
				       reg->uid, reg->bits_per_long);
			if (fmt_ret < 0) {
				ERR("Failed to format channel index directory");
				ret = LTTNG_ERR_CREATE_DIR_FAIL;
				goto error;
			}

			/*
			 * Create the index subdirectory which will take care
			 * of implicitly creating the channel's path.
			 */
			chunk_status = lttng_trace_chunk_create_subdirectory(
					usess->current_trace_chunk,
					pathname_index);
			free(pathname_index);
			if (chunk_status != LTTNG_TRACE_CHUNK_STATUS_OK) {
				ret = LTTNG_ERR_CREATE_DIR_FAIL;
				goto error;
			}
		}
		break;
	}
	case LTTNG_BUFFER_PER_PID:
	{
		struct ust_app *app;

		/*
		 * Create the toplevel ust/ directory in case no apps are running.
		 */
		chunk_status = lttng_trace_chunk_create_subdirectory(
				usess->current_trace_chunk,
				DEFAULT_UST_TRACE_DIR);
		if (chunk_status != LTTNG_TRACE_CHUNK_STATUS_OK) {
			ret = LTTNG_ERR_CREATE_DIR_FAIL;
			goto error;
		}

		cds_lfht_for_each_entry(ust_app_ht->ht, &iter.iter, app,
				pid_n.node) {
			struct ust_app_session *ua_sess;
			struct ust_registry_session *registry;

			ua_sess = lookup_session_by_app(usess, app);
			if (!ua_sess) {
				/* Session not associated with this app. */
				continue;
			}

			registry = get_session_registry(ua_sess);
			if (!registry) {
				DBG("Application session is being torn down. Skip application.");
				continue;
			}

			fmt_ret = asprintf(&pathname_index,
					DEFAULT_UST_TRACE_DIR "/%s/" DEFAULT_INDEX_DIR,
					ua_sess->path);
			if (fmt_ret < 0) {
				ERR("Failed to format channel index directory");
				ret = LTTNG_ERR_CREATE_DIR_FAIL;
				goto error;
			}
			/*
			 * Create the index subdirectory which will take care
			 * of implicitly creating the channel's path.
			 */
			chunk_status = lttng_trace_chunk_create_subdirectory(
					usess->current_trace_chunk,
					pathname_index);
			free(pathname_index);
			if (chunk_status != LTTNG_TRACE_CHUNK_STATUS_OK) {
				ret = LTTNG_ERR_CREATE_DIR_FAIL;
				goto error;
			}
		}
		break;
	}
	default:
		abort();
	}

	ret = LTTNG_OK;
error:
	rcu_read_unlock();
	return ret;
}

/*
 * Clear all the channels of a session.
 *
 * Return LTTNG_OK on success or else an LTTng error code.
 */
enum lttng_error_code ust_app_clear_session(struct ltt_session *session)
{
	int ret;
	enum lttng_error_code cmd_ret = LTTNG_OK;
	struct lttng_ht_iter iter;
	struct ust_app *app;
	struct ltt_ust_session *usess = session->ust_session;

	assert(usess);

	rcu_read_lock();

	if (usess->active) {
		ERR("Expecting inactive session %s (%" PRIu64 ")", session->name, session->id);
		cmd_ret = LTTNG_ERR_FATAL;
		goto end;
	}

	switch (usess->buffer_type) {
	case LTTNG_BUFFER_PER_UID:
	{
		struct buffer_reg_uid *reg;

		cds_list_for_each_entry(reg, &usess->buffer_reg_uid_list, lnode) {
			struct buffer_reg_channel *buf_reg_chan;
			struct consumer_socket *socket;

			/* Get consumer socket to use to push the metadata.*/
			socket = consumer_find_socket_by_bitness(reg->bits_per_long,
					usess->consumer);
			if (!socket) {
				cmd_ret = LTTNG_ERR_INVALID;
				goto error_socket;
			}

			/* Clear the data channels. */
			cds_lfht_for_each_entry(reg->registry->channels->ht, &iter.iter,
					buf_reg_chan, node.node) {
				ret = consumer_clear_channel(socket,
						buf_reg_chan->consumer_key);
				if (ret < 0) {
					goto error;
				}
			}

			(void) push_metadata(reg->registry->reg.ust, usess->consumer);

			/*
			 * Clear the metadata channel.
			 * Metadata channel is not cleared per se but we still need to
			 * perform a rotation operation on it behind the scene.
			 */
			ret = consumer_clear_channel(socket,
					reg->registry->reg.ust->metadata_key);
			if (ret < 0) {
				goto error;
			}
		}
		break;
	}
	case LTTNG_BUFFER_PER_PID:
	{
		cds_lfht_for_each_entry(ust_app_ht->ht, &iter.iter, app, pid_n.node) {
			struct consumer_socket *socket;
			struct lttng_ht_iter chan_iter;
			struct ust_app_channel *ua_chan;
			struct ust_app_session *ua_sess;
			struct ust_registry_session *registry;

			ua_sess = lookup_session_by_app(usess, app);
			if (!ua_sess) {
				/* Session not associated with this app. */
				continue;
			}

			/* Get the right consumer socket for the application. */
			socket = consumer_find_socket_by_bitness(app->bits_per_long,
					usess->consumer);
			if (!socket) {
				cmd_ret = LTTNG_ERR_INVALID;
				goto error_socket;
			}

			registry = get_session_registry(ua_sess);
			if (!registry) {
				DBG("Application session is being torn down. Skip application.");
				continue;
			}

			/* Clear the data channels. */
			cds_lfht_for_each_entry(ua_sess->channels->ht, &chan_iter.iter,
					ua_chan, node.node) {
				ret = consumer_clear_channel(socket, ua_chan->key);
				if (ret < 0) {
					/* Per-PID buffer and application going away. */
					if (ret == -LTTNG_ERR_CHAN_NOT_FOUND) {
						continue;
					}
					goto error;
				}
			}

			(void) push_metadata(registry, usess->consumer);

			/*
			 * Clear the metadata channel.
			 * Metadata channel is not cleared per se but we still need to
			 * perform rotation operation on it behind the scene.
			 */
			ret = consumer_clear_channel(socket, registry->metadata_key);
			if (ret < 0) {
				/* Per-PID buffer and application going away. */
				if (ret == -LTTNG_ERR_CHAN_NOT_FOUND) {
					continue;
				}
				goto error;
			}
		}
		break;
	}
	default:
		assert(0);
		break;
	}

	cmd_ret = LTTNG_OK;
	goto end;

error:
	switch (-ret) {
	case LTTCOMM_CONSUMERD_RELAYD_CLEAR_DISALLOWED:
		cmd_ret = LTTNG_ERR_CLEAR_RELAY_DISALLOWED;
		break;
	default:
		cmd_ret = LTTNG_ERR_CLEAR_FAIL_CONSUMER;
	}

error_socket:
end:
	rcu_read_unlock();
	return cmd_ret;
}

/*
 * This function skips the metadata channel as the begin/end timestamps of a
 * metadata packet are useless.
 *
 * Moreover, opening a packet after a "clear" will cause problems for live
 * sessions as it will introduce padding that was not part of the first trace
 * chunk. The relay daemon expects the content of the metadata stream of
 * successive metadata trace chunks to be strict supersets of one another.
 *
 * For example, flushing a packet at the beginning of the metadata stream of
 * a trace chunk resulting from a "clear" session command will cause the
 * size of the metadata stream of the new trace chunk to not match the size of
 * the metadata stream of the original chunk. This will confuse the relay
 * daemon as the same "offset" in a metadata stream will no longer point
 * to the same content.
 */
enum lttng_error_code ust_app_open_packets(struct ltt_session *session)
{
	enum lttng_error_code ret = LTTNG_OK;
	struct lttng_ht_iter iter;
	struct ltt_ust_session *usess = session->ust_session;

	assert(usess);

	rcu_read_lock();

	switch (usess->buffer_type) {
	case LTTNG_BUFFER_PER_UID:
	{
		struct buffer_reg_uid *reg;

		cds_list_for_each_entry (
				reg, &usess->buffer_reg_uid_list, lnode) {
			struct buffer_reg_channel *buf_reg_chan;
			struct consumer_socket *socket;

			socket = consumer_find_socket_by_bitness(
					reg->bits_per_long, usess->consumer);
			if (!socket) {
				ret = LTTNG_ERR_FATAL;
				goto error;
			}

			cds_lfht_for_each_entry(reg->registry->channels->ht,
					&iter.iter, buf_reg_chan, node.node) {
				const int open_ret =
						consumer_open_channel_packets(
							socket,
							buf_reg_chan->consumer_key);

				if (open_ret < 0) {
					ret = LTTNG_ERR_UNK;
					goto error;
				}
			}
		}
		break;
	}
	case LTTNG_BUFFER_PER_PID:
	{
		struct ust_app *app;

		cds_lfht_for_each_entry (
				ust_app_ht->ht, &iter.iter, app, pid_n.node) {
			struct consumer_socket *socket;
			struct lttng_ht_iter chan_iter;
			struct ust_app_channel *ua_chan;
			struct ust_app_session *ua_sess;
			struct ust_registry_session *registry;

			ua_sess = lookup_session_by_app(usess, app);
			if (!ua_sess) {
				/* Session not associated with this app. */
				continue;
			}

			/* Get the right consumer socket for the application. */
			socket = consumer_find_socket_by_bitness(
					app->bits_per_long, usess->consumer);
			if (!socket) {
				ret = LTTNG_ERR_FATAL;
				goto error;
			}

			registry = get_session_registry(ua_sess);
			if (!registry) {
				DBG("Application session is being torn down. Skip application.");
				continue;
			}

			cds_lfht_for_each_entry(ua_sess->channels->ht,
					&chan_iter.iter, ua_chan, node.node) {
				const int open_ret =
						consumer_open_channel_packets(
							socket,
							ua_chan->key);

				if (open_ret < 0) {
					/*
					 * Per-PID buffer and application going
					 * away.
					 */
					if (open_ret == -LTTNG_ERR_CHAN_NOT_FOUND) {
						continue;
					}

					ret = LTTNG_ERR_UNK;
					goto error;
				}
			}
		}
		break;
	}
	default:
		abort();
		break;
	}

error:
	rcu_read_unlock();
	return ret;
}
