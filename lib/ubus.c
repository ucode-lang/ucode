/*
 * Copyright (C) 2020-2021 Jo-Philipp Wich <jo@mein.io>
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

/**
 * # Ubus IPC
 *
 * The `ubus` module provides functions for OpenWrt inter-process
 * communication, including access to ubus registered modules and their
 * methods, as well as monitoring and publish/subscribe activity on the
 * ubus message bus.
 *
 * Functions can be individually imported using named import syntax:
 *
 * ```js
 * import { connect } from 'ubus';
 *
 * const ubus = connect();
 * const result = ubus.call("session", "get", { key: "value" });
 * ```
 *
 * Alternatively, the module namespace can be imported using a wildcard
 * import:
 *
 * ```js
 * import * as ubus from 'ubus';
 *
 * const ctx = ubus.connect();
 * ```
 *
 * The `ubus` module may also be loaded via the `-lubus` interpreter switch.
 *
 * ## Architecture
 *
 * Ubus uses a broker pattern architecture with three main components:
 * - **`ubusd`**: The central message router/broker that manages
 *   registrations and forwards messages between objects
 * - **Server objects**: Interfaces/daemons that register methods for
 *   clients to call
 * - **Client objects**: Callers that invoke server object methods
 *
 * All connections go through `ubusd`, significantly reducing the number
 * of IPC connections compared to traditional client-server models.
 *
 * ## Communication Schemes
 *
 * Ubus provides three delivery schemes for IPC:
 *
 * 1. **Invoke** (one-to-one): Direct method calls to a specific object
 *    by ID
 * 2. **Subscribe/Notify** (one-to-many, group by object): Notifications
 *    sent to all subscribers of a particular object
 * 3. **Event Broadcast** (one-to-many, group by event): Events broadcast
 *    to all listeners registered for a matching event pattern
 *
 * ## Roles in Ubus
 *
 * - **Object**: Process registered to `ubusd`, including services and
 *   service callers
 * - **Method**: Procedures provided by objects; servers can provide
 *   multiple methods
 * - **Data**: Information in JSON format carried by requests or replies
 * - **Subscriber**: Object subscribed to a target service; notified when
 *   the target sends notifications
 * - **Event**: Identified by a string event pattern; objects can register
 *   to events and send data with matching patterns
 * - **Event Registrant**: Object registered to an event pattern; receives
 *   forwarded data when matching messages are received
 *
 * ## Data Format
 *
 * All data is transferred in JSON format via `blobmsg`. Method calls,
 * requests, and replies all use JSON for data serialization.
 *
 * ## Usage Examples
 *
 * ### Basic connection and method call
 * ```js
 * const ubus = require("ubus");
 *
 * // Connect to ubus and call a method
 * const conn = ubus.connect();
 * if (conn) {
 *     const result = conn.call("network.interface", "status", {});
 *     printf("Interface status: %.J\n", result);
 *     conn.disconnect();
 * }
 * ```
 *
 * ### Asynchronous method invocation with callback
 * ```js
 * const ubus = require("ubus");
 *
 * // Typical pattern: async call with callback
 * const conn = ubus.connect();
 *
 * conn.defer("some.object", "some_method", {}, (rc, result) => {
 *     if (rc == 0) {
 *         printf("Result: %.J\n", result);
 *     }
 * });
 * ```
 *
 * ### Persistent connection pattern
 * ```js
 * const ubus = require("ubus");
 *
 * // Keep connection alive to prevent GC
 * const ubus_conn = ubus.connect();
 *
 * function handle_request(request) {
 *     ubus_conn.defer("some.object", "some_method", {}, (rc, data) => {
 *         request.reply({ result: data });
 *     });
 * }
 * ```
 *
 * ### Publishing an object
 * ```js
 * const ubus = require("ubus");
 *
 * const conn = ubus.connect();
 * const obj = conn.publish("my.service", {
 *     "hello": (req, msg) => {
 *         req.reply({ message: "Hello from " + msg.name });
 *     }
 * });
 * ```
 *
 * ### Event broadcasting
 * ```js
 * const ubus = require("ubus");
 *
 * const conn = ubus.connect();
 *
 * // Register as event listener
 * const listener = conn.listener("my.event.*", (pattern, data) => {
 *     printf("Received event: %s %.J\n", pattern, data);
 * });
 *
 * // Send an event
 * conn.event("my.event.test", { data: "test payload" });
 * ```
 *
 * @module ubus
 * @see https://openwrt.org/docs/techref/ubus
 */

/**
 * Represents a connection to the ubus bus.
 *
 * A connection is established via
 * {@link module:ubus#connect|connect()} and serves as the primary
 * interface for all ubus operations. Through a connection, you can:
 *
 * - Discover and invoke methods on remote objects
 *   ({@link module:ubus#list|list()},
 *   {@link module:ubus#call|call()})
 * - Publish your own objects to the bus
 *   ({@link module:ubus#publish|publish()})
 * - Subscribe to notifications from other objects
 *   ({@link module:ubus#subscriber|subscriber()})
 * - Register event listeners for pattern-based events
 *   ({@link module:ubus#listener|listener()})
 * - Send broadcast events to other listeners
 *   ({@link module:ubus#event|event()})
 *
 * The connection uses the broker pattern, routing all communication
 * through ubusd. It supports both synchronous (call) and asynchronous
 * (defer) method invocations.
 *
 * @class module:ubus.connection
 * @hideconstructor
 *
 * @borrows module:ubus#error as module:ubus#error
 *
 * @see {@link module:ubus#connect|connect()}
 * @see {@link module:ubus#open_channel|open_channel()}
 *
 * @example
 *
 * const conn = connect();
 *
 * conn.list();
 * conn.call(…);
 * conn.defer(…);
 * conn.publish(…);
 * conn.remove(…);
 * conn.listener(…);
 * conn.subscriber(…);
 * conn.event(…);
 * conn.disconnect();
 *
 * conn.error();
 */

/**
 * Represents a channel connection to the ubus bus.
 *
 * Channels provide bidirectional communication between two ubus objects
 * through file descriptors. They are created via
 * {@link module:ubus#open_channel|open_channel()} or from an incoming
 * request via
 * {@link module:ubus.request#new_channel|new_channel()}.
 *
 * Channels are useful for:
 * - Establishing dedicated communication paths between specific objects
 * - Streaming data or multiple requests over a single connection
 * - File descriptor passing between processes
 *
 * @class module:ubus.channel
 * @hideconstructor
 *
 * @borrows module:ubus#error as module:ubus.channel#error
 *
 * @see {@link module:ubus#open_channel|open_channel()}
 * @see {@link module:ubus.request#new_channel|new_channel()}
 *
 * @example
 *
 * const chan = open_channel(…);
 *
 * chan.request(…);
 * chan.defer(…);
 * chan.disconnect();
 *
 * chan.error();
 */

/**
 * Represents a deferred ubus request.
 *
 * A deferred request is created when invoking a method asynchronously
 * using {@link module:ubus#defer|defer()} or
 * {@link module:ubus.channel#defer|defer()}. Instead of
 * blocking and waiting for the result, the operation returns immediately
 * with a deferred object that can be used to:
 *
 * - Check if the request has completed
 *   ({@link module:ubus.deferred#completed|completed()})
 * - Wait synchronously for completion
 *   ({@link module:ubus.deferred#await|await()})
 * - Abort the pending request if no longer needed
 *   ({@link module:ubus.deferred#abort|abort()})
 *
 * This pattern is useful for:
 * - Non-blocking operations in event-driven applications
 * - Timeout handling and request cancellation
 * - Concurrent execution of multiple ubus method calls
 *
 * @class module:ubus.deferred
 * @hideconstructor
 *
 * @see {@link module:ubus#defer|defer()}
 * @see {@link module:ubus.channel#defer|defer()}
 *
 * @example
 *
 * const req = defer(…);
 *
 * req.await();
 * req.completed();
 * req.abort();
 *
 * @example
 * // Typical async pattern with callback
 * const req = conn.defer("system", "info", {}, (rc, data) => {
 *     if (rc == 0)
 *         printf("Info: %.J\n", data);
 * });
 */

/**
 * Represents a ubus object published on the bus.
 *
 * A published object is a service registered with ubusd that provides
 * methods for other processes to call. Objects are created via
 * {@link module:ubus#publish|publish()} and can:
 *
 * - Expose multiple methods for remote invocation
 * - Receive notifications from subscribers
 *   ({@link module:ubus.object#subscribed|subscribed()},
 *   {@link module:ubus.object#notify|notify()})
 * - Be removed from the bus when no longer needed
 *   ({@link module:ubus.object#remove|remove()})
 *
 * Objects are identified by their path (e.g., `system`,
 * `network.interface`) and can be discovered by other processes using
 * {@link module:ubus#list|list()}.
 *
 * @class module:ubus.object
 * @hideconstructor
 *
 * @see {@link module:ubus#publish|publish()}
 * @see {@link module:ubus#subscriber|subscriber()}
 *
 * @example
 *
 * const obj = publish(…, { … });
 *
 * obj.subscribed();
 * obj.notify(…);
 * obj.remove();
 */

/**
 * Represents a deferred ubus method call context.
 *
 * A request object is created when a published object method is invoked
 * asynchronously. It provides the server-side interface for handling
 * incoming method calls and sending responses.
 *
 * The request context allows the method handler to:
 * - Send a successful reply with data
 *   ({@link module:ubus.request#reply|reply()})
 * - Report an error condition
 *   ({@link module:ubus.request#error|error()})
 * - Defer completion for asynchronous processing
 *   ({@link module:ubus.request#defer|defer()})
 * - Exchange file descriptors with the caller
 *   ({@link module:ubus.request#get_fd|get_fd()},
 *   {@link module:ubus.request#set_fd|set_fd()})
 * - Establish channel-based communication
 *   ({@link module:ubus.request#new_channel|new_channel()})
 *
 * This is used internally by the ubus module when a published object's
 * method is called by a client.
 *
 * @class module:ubus.request
 * @hideconstructor
 *
 * @see {@link module:ubus#publish|publish()}
 *
 * @example
 *
 * // Method handler receives request as second argument
 * const obj = publish("my.service", {
 *     hello: (req, msg) => {
 *         req.reply({ message: "Hello" });
 *     }
 * });
 */

/**
 * Represents an asynchronous notification request.
 *
 * A notify object is created when sending a notification to subscribers
 * via {@link module:ubus.object#notify|notify()}. It allows
 * tracking the delivery status of the notification and provides the
 * ability to abort if needed.
 *
 * Notifications are delivered to all subscribers of an object in the
 * subscribe/notify communication scheme. The notify object provides
 * non-blocking status checking and cancellation capabilities.
 *
 * @class module:ubus.notify
 * @hideconstructor
 *
 * @see {@link module:ubus.object#notify|notify()}
 * @see {@link module:ubus#subscriber|subscriber()}
 *
 * @example
 *
 * const n = notify(…);
 *
 * n.completed();
 * n.abort();
 */

/**
 * Represents an event listener for pattern-based events.
 *
 * A listener is registered via
 * {@link module:ubus#listener|listener()} to receive events
 * matching a specific pattern. Listeners are part of the event broadcast
 * communication scheme, where events are sent to all registered listeners
 * with matching event patterns.
 *
 * Event patterns support wildcards (e.g., `` `system.*` ``,
 * `` `network.interface.*` ``) allowing flexible event routing and
 * subscription.
 *
 * @class module:ubus.listener
 * @hideconstructor
 *
 * @see {@link module:ubus#listener|listener()}
 * @see {@link module:ubus#event|event()}
 *
 * @example
 *
 * const listener = listener("event.*", (pattern, data) => { … });
 *
 * listener.remove();
 */

/**
 * Represents a subscriber to an object's notifications.
 *
 * A subscriber is registered via
 * {@link module:ubus#subscriber|subscriber()} to receive
 * notifications from a specific object. Subscribers are part of the
 * subscribe/notify communication scheme, where the target object can send
 * notifications that are delivered to all registered subscribers.
 *
 * When a subscriber is registered, the target object receives a
 * notification about the new subscription. Similarly, when a subscriber
 * is removed, the target object is notified of the unsubscription.
 *
 * @class module:ubus.subscriber
 * @hideconstructor
 *
 * @see {@link module:ubus#subscriber|subscriber()}
 * @see {@link module:ubus.object#notify|notify()}
 * @see {@link module:ubus.object#subscribed|subscribed()}
 *
 * @example
 *
 * const sub = subscriber(objid, (method, data) => { … });
 *
 * sub.subscribe();
 * sub.unsubscribe();
 * sub.remove();
 */

#include <limits.h>
#include <fnmatch.h>
#include <libubus.h>

#include "ucode/module.h"

#define ok_return(expr) do { set_error(0, NULL); return (expr); } while(0)
#define err_return(err, ...) do { set_error(err, __VA_ARGS__); return NULL; } while(0)
#define errval_return(err, ...) do { set_error(err, __VA_ARGS__); return err; } while(0)

#define REQUIRED	0
#define OPTIONAL	1
#define NAMED		2

static struct {
	enum ubus_msg_status code;
	char *msg;
} last_error;

__attribute__((format(printf, 2, 3))) static void
set_error(int errcode, const char *fmt, ...)
{
	va_list ap;

	free(last_error.msg);

	last_error.code = errcode;
	last_error.msg = NULL;

	if (fmt) {
		va_start(ap, fmt);
		xvasprintf(&last_error.msg, fmt, ap);
		va_end(ap);
	}
}

static char *
_arg_type(uc_type_t type)
{
	switch (type) {
	case UC_INTEGER:   return "an integer value";
	case UC_BOOLEAN:   return "a boolean value";
	case UC_STRING:    return "a string value";
	case UC_DOUBLE:    return "a double value";
	case UC_ARRAY:     return "an array";
	case UC_OBJECT:    return "an object";
	case UC_REGEXP:    return "a regular expression";
	case UC_CLOSURE:   return "a function";
	default:           return "the expected type";
	}
}

static bool
_args_get(uc_vm_t *vm, bool named, size_t nargs, ...)
{
	uc_value_t **ptr, *arg, *obj = NULL;
	uc_type_t type, t;
	const char *name;
	size_t index = 0;
	va_list ap;
	int opt;

	if (named) {
		obj = uc_fn_arg(0);

		if (nargs != 1 || ucv_type(obj) != UC_OBJECT)
			named = false;
	}

	va_start(ap, nargs);

	while (true) {
		name = va_arg(ap, const char *);

		if (!name)
			break;

		type = va_arg(ap, uc_type_t);
		opt = va_arg(ap, int);
		ptr = va_arg(ap, uc_value_t **);

		if (named)
			arg = ucv_object_get(obj, name, NULL);
		else if (opt != NAMED)
			arg = uc_fn_arg(index++);
		else
			arg = NULL;

		if (opt == REQUIRED && !arg)
			err_return(UBUS_STATUS_INVALID_ARGUMENT, "Argument %s is required", name);

		t = ucv_type(arg);

		if (t == UC_CFUNCTION)
			t = UC_CLOSURE;

		if (arg && type && t != type)
			err_return(UBUS_STATUS_INVALID_ARGUMENT, "Argument %s is not %s", name, _arg_type(type));

		*ptr = arg;
	}

	va_end(ap);

	ok_return(true);
}

#define args_get_named(vm, nargs, ...) do { if (!_args_get(vm, true, nargs, __VA_ARGS__, NULL)) return NULL; } while(0)
#define args_get(vm, nargs, ...) do { if (!_args_get(vm, false, nargs, __VA_ARGS__, NULL)) return NULL; } while(0)

static struct blob_buf buf;

typedef struct {
	struct ubus_context ctx;
	struct blob_buf buf;
	int timeout;
	bool fd_handle;

	uc_vm_t *vm;
	uc_value_t *res;
} uc_ubus_connection_t;

typedef struct {
	struct ubus_request request;
	struct uloop_timeout timeout;
	struct ubus_context *ctx;
	bool complete;
	uc_vm_t *vm;
	uc_value_t *res;
	uc_value_t *fd_callback;
	uc_value_t *response;
} uc_ubus_deferred_t;

typedef struct {
	struct ubus_object obj;
	struct ubus_object_type type;
	struct ubus_context *ctx;
	uc_vm_t *vm;
	uc_value_t *res;
	struct ubus_method methods[];
} uc_ubus_object_t;

typedef struct {
	struct ubus_request_data req;
	struct uloop_timeout timeout;
	struct ubus_context *ctx;
	uc_value_t *res;
	uc_vm_t *vm;
	bool deferred;
	bool replied;
} uc_ubus_request_t;

typedef struct {
	struct ubus_notify_request req;
	struct ubus_context *ctx;
	uc_vm_t *vm;
	uc_value_t *res;
	bool complete;
} uc_ubus_notify_t;

typedef struct {
	struct ubus_event_handler ev;
	struct ubus_context *ctx;
	uc_vm_t *vm;
	uc_value_t *res;
} uc_ubus_listener_t;

typedef struct {
	struct ubus_subscriber sub;
	struct ubus_context *ctx;
	uc_vm_t *vm;
	uc_value_t *res;
} uc_ubus_subscriber_t;

typedef struct {
	bool mret;
	uc_value_t *res;
} uc_ubus_call_res_t;

/**
 * Query ubus error information.
 *
 * Returns a string containing a description of the last ubus error when
 * the *numeric* argument is absent or false.
 *
 * Returns a ubus status code number when the *numeric* argument is `true`.
 *
 * Returns `null` if there is no error information.
 *
 * @function module:ubus#error
 *
 * @param {boolean} [numeric]
 * Whether to return a numeric status code (`true`) or a human readable
 * error message (false).
 *
 * @returns {?string|?number}
 */
static uc_value_t *
uc_ubus_error(uc_vm_t *vm, size_t nargs)
{
	uc_value_t *numeric = uc_fn_arg(0), *rv;
	uc_stringbuf_t *buf;
	const char *s;

	if (last_error.code == 0)
		return NULL;

	if (ucv_is_truish(numeric)) {
		rv = ucv_int64_new(last_error.code);
	}
	else {
		buf = ucv_stringbuf_new();

		if (last_error.code == UBUS_STATUS_UNKNOWN_ERROR && last_error.msg) {
			ucv_stringbuf_addstr(buf, last_error.msg, strlen(last_error.msg));
		}
		else {
			s = ubus_strerror(last_error.code);

			ucv_stringbuf_addstr(buf, s, strlen(s));

			if (last_error.msg)
				ucv_stringbuf_printf(buf, ": %s", last_error.msg);
		}

		rv = ucv_stringbuf_finish(buf);
	}

	set_error(0, NULL);

	return rv;
}

enum {
	CONN_RES_FD,
	CONN_RES_CB,
	CONN_RES_DISCONNECT_CB,
	__CONN_RES_MAX
};

enum {
	DEFER_RES_CONN,
	DEFER_RES_CB,
	DEFER_RES_DATA_CB,
	DEFER_RES_FD_CB,
	DEFER_RES_FD,
	DEFER_RES_RESPONSE,
	__DEFER_RES_MAX
};

enum {
	OBJ_RES_CONN,
	OBJ_RES_METHODS,
	OBJ_RES_SUB_CB,
	__OBJ_RES_MAX
};

enum {
	NOTIFY_RES_CONN,
	NOTIFY_RES_CB,
	NOTIFY_RES_DATA_CB,
	NOTIFY_RES_STATUS_CB,
	__NOTIFY_RES_MAX,
};

enum {
	SUB_RES_NOTIFY_CB,
	SUB_RES_REMOVE_CB,
	SUB_RES_PATTERNS,
	__SUB_RES_MAX,
};

/* Largest value slot count across all ubus resource types. Keep in sync
 * with the largest __*_RES_MAX above; used to sweep a resource's value
 * slots without needing to know its specific type. */
enum {
	UBUS_RES_MAX = __DEFER_RES_MAX
};

static void
uc_ubus_put_res(uc_value_t **rp)
{
	uc_value_t *res = *rp;
	size_t i;

	*rp = NULL;

	if (!res)
		return;

	/* drop the stored references (including a per-instance prototype, if
	 * present) to break resource <-> closure reference cycles
	 * deterministically, without waiting for a gc pass. The setter
	 * ignores indexes beyond the resource's own value count, so it is
	 * safe to sweep up to the largest ubus resource slot count. */
	for (i = 0; i < UBUS_RES_MAX; i++)
		ucv_resource_value_set(res, i, NULL);

	ucv_resource_proto_set(res, NULL);

	ucv_resource_persistent_set(res, false);
	ucv_put(res);
}

static uc_value_t *
blob_to_ucv(uc_vm_t *vm, struct blob_attr *attr, bool table, const char **name);

static uc_value_t *
blob_array_to_ucv(uc_vm_t *vm, struct blob_attr *attr, size_t len, bool table)
{
	uc_value_t *o = table ? ucv_object_new(vm) : ucv_array_new(vm);
	uc_value_t *v;
	struct blob_attr *pos;
	size_t rem = len;
	const char *name;

	if (!o)
		return NULL;

	__blob_for_each_attr(pos, attr, rem) {
		name = NULL;
		v = blob_to_ucv(vm, pos, table, &name);

		if (table && name)
			ucv_object_add(o, name, v);
		else if (!table)
			ucv_array_push(o, v);
		else
			ucv_put(v);
	}

	return o;
}

static uc_value_t *
blob_to_ucv(uc_vm_t *vm, struct blob_attr *attr, bool table, const char **name)
{
	void *data;
	int len;

	if (!blobmsg_check_attr(attr, false))
		return NULL;

	if (table && blobmsg_name(attr)[0])
		*name = blobmsg_name(attr);

	data = blobmsg_data(attr);
	len = blobmsg_data_len(attr);

	switch (blob_id(attr)) {
	case BLOBMSG_TYPE_BOOL:
		return ucv_boolean_new(*(uint8_t *)data);

	case BLOBMSG_TYPE_INT16:
		return ucv_int64_new((int16_t)be16_to_cpu(*(uint16_t *)data));

	case BLOBMSG_TYPE_INT32:
		return ucv_int64_new((int32_t)be32_to_cpu(*(uint32_t *)data));

	case BLOBMSG_TYPE_INT64:
		return ucv_int64_new((int64_t)be64_to_cpu(*(uint64_t *)data));

	case BLOBMSG_TYPE_DOUBLE:
		;
		union {
			double d;
			uint64_t u64;
		} v;

		v.u64 = be64_to_cpu(*(uint64_t *)data);

		return ucv_double_new(v.d);

	case BLOBMSG_TYPE_STRING:
		return ucv_string_new_length(data, len - 1);

	case BLOBMSG_TYPE_ARRAY:
		return blob_array_to_ucv(vm, data, len, false);

	case BLOBMSG_TYPE_TABLE:
		return blob_array_to_ucv(vm, data, len, true);

	default:
		return NULL;
	}
}

static void
ucv_array_to_blob(uc_value_t *val, struct blob_buf *blob);

static void
ucv_object_to_blob(uc_value_t *val, struct blob_buf *blob);

static void
ucv_to_blob(const char *name, uc_value_t *val, struct blob_buf *blob)
{
	int64_t n;
	void *c;

	switch (ucv_type(val)) {
	case UC_NULL:
		blobmsg_add_field(blob, BLOBMSG_TYPE_UNSPEC, name, NULL, 0);
		break;

	case UC_BOOLEAN:
		blobmsg_add_u8(blob, name, ucv_boolean_get(val));
		break;

	case UC_INTEGER:
		n = ucv_int64_get(val);

		if (errno == ERANGE)
			blobmsg_add_u64(blob, name, ucv_uint64_get(val));
		else if (n >= INT32_MIN && n <= INT32_MAX)
			blobmsg_add_u32(blob, name, n);
		else
			blobmsg_add_u64(blob, name, n);

		break;

	case UC_DOUBLE:
		blobmsg_add_double(blob, name, ucv_double_get(val));
		break;

	case UC_STRING:
		blobmsg_add_field(blob, BLOBMSG_TYPE_STRING, name,
				  ucv_string_get(val), ucv_string_length(val) + 1);
		break;

	case UC_ARRAY:
		c = blobmsg_open_array(blob, name);
		ucv_array_to_blob(val, blob);
		blobmsg_close_array(blob, c);
		break;

	case UC_OBJECT:
		c = blobmsg_open_table(blob, name);
		ucv_object_to_blob(val, blob);
		blobmsg_close_table(blob, c);
		break;

	default:
		break;
	}
}

static void
ucv_array_to_blob(uc_value_t *val, struct blob_buf *blob)
{
	size_t i;

	for (i = 0; i < ucv_array_length(val); i++)
		ucv_to_blob(NULL, ucv_array_get(val, i), blob);
}

static void
ucv_object_to_blob(uc_value_t *val, struct blob_buf *blob)
{
	ucv_object_foreach(val, k, v)
		ucv_to_blob(k, v, blob);
}


static uc_ubus_connection_t *
uc_ubus_conn_alloc(uc_vm_t *vm, uc_value_t *timeout, const char *type)
{
	uc_ubus_connection_t *c = NULL;
	uc_value_t *res;

	res = ucv_resource_create_ex(vm, type, (void **)&c, __CONN_RES_MAX, sizeof(*c));
	if (!c)
		err_return(UBUS_STATUS_UNKNOWN_ERROR, "Out of memory");

	c->vm = vm;
	c->res = res;
	c->timeout = timeout ? ucv_int64_get(timeout) : 30;
	if (c->timeout < 0)
		c->timeout = 30;

	return c;
}

/**
 * Establish a connection to the ubus bus.
 *
 * Connects to the specified ubus socket path or the default socket if none
 * is provided.
 *
 * Returns a connection resource on success.
 *
 * Returns `null` if the connection could not be established.
 *
 * @function module:ubus#connect
 *
 * @param {string} [socket]
 * The path to the ubus socket to connect to. If omitted, the default socket
 * path is used.
 *
 * @param {number} [timeout=30]
 * The timeout in seconds to use for subsequent ubus operations.
 *
 * @returns {?module:ubus.connection}
 */
static uc_value_t *
uc_ubus_connect(uc_vm_t *vm, size_t nargs)
{
	uc_value_t *socket, *timeout;
	uc_ubus_connection_t *c;

	args_get(vm, nargs,
	         "socket", UC_STRING, true, &socket,
	         "timeout", UC_INTEGER, true, &timeout);

	c = uc_ubus_conn_alloc(vm, timeout, "ubus.connection");

	if (!c)
		return NULL;

	if (ubus_connect_ctx(&c->ctx, socket ? ucv_string_get(socket) : NULL)) {
		ucv_put(c->res);
		err_return(UBUS_STATUS_UNKNOWN_ERROR, "Unable to connect to ubus socket");
	}

	if (c->timeout < 0)
		c->timeout = 30;

	ubus_add_uloop(&c->ctx);

	ok_return(ucv_get(c->res));
}

static void
uc_ubus_signatures_cb(struct ubus_context *c, struct ubus_object_data *o, void *p)
{
	uc_value_t *arr = p;
	uc_value_t *sig;

	if (!o->signature)
		return;

	sig = blob_array_to_ucv(NULL, blob_data(o->signature), blob_len(o->signature), true);

	if (sig)
		ucv_array_push(arr, sig);
}

static void
uc_ubus_objects_cb(struct ubus_context *c, struct ubus_object_data *o, void *p)
{
	uc_value_t *arr = p;

	ucv_array_push(arr, ucv_string_new(o->path));
}

static bool
_conn_get(uc_vm_t *vm, uc_ubus_connection_t **conn)
{
	uc_ubus_connection_t *c;
	uc_value_t *res;

	if (ucv_type(_uc_fn_this_res(vm)) == UC_OBJECT) {
		res = uc_vm_registry_get(vm, "ubus.connection");
		c = ucv_resource_data(res, "ubus.connection");

		if (c && c->ctx.sock.fd >= 0)
			goto out;

		c = uc_ubus_conn_alloc(vm, NULL, "ubus.connection");
		if (!c)
			return NULL;

		if (ubus_connect_ctx(&c->ctx, NULL)) {
			ucv_put(c->res);
			err_return(UBUS_STATUS_UNKNOWN_ERROR, "Unable to connect to ubus socket");
		}

		ubus_add_uloop(&c->ctx);

		uc_vm_registry_set(vm, "ubus.connection", ucv_get(c->res));
	}
	else {
		c = uc_fn_thisval("ubus.connection");
		if (!c)
			c = uc_fn_thisval("ubus.channel");

		if (!c)
			err_return(UBUS_STATUS_INVALID_ARGUMENT, "Invalid connection context");

		if (c->ctx.sock.fd < 0)
			err_return(UBUS_STATUS_CONNECTION_FAILED, "Connection is closed");
	}

out:
	*conn = c;

	ok_return(true);
}

#define conn_get(vm, ptr) do { if (!_conn_get(vm, ptr)) return NULL; } while(0)

/**
 * List available ubus objects.
 *
 * Queries the ubus bus for registered objects. If an object name pattern
 * is provided, returns signatures for matching objects. Otherwise, returns
 * a list of all registered object paths.
 *
 * Returns an array of object paths or object signatures.
 *
 * Returns `null` if the list operation failed.
 *
 * @function module:ubus.connection#list
 *
 * @param {string} [object_name]
 * Optional object name pattern to filter results. When provided, returns
 * signatures for matching objects; otherwise returns all object paths.
 *
 * @returns {?string[]}
 */
static uc_value_t *
uc_ubus_list(uc_vm_t *vm, size_t nargs)
{
	uc_ubus_connection_t *c;
	uc_value_t *objname, *res = NULL;
	enum ubus_msg_status rv;

	conn_get(vm, &c);

	args_get(vm, nargs,
	         "object name", UC_STRING, true, &objname);

	res = ucv_array_new(vm);

	rv = ubus_lookup(&c->ctx,
	                 objname ? ucv_string_get(objname) : NULL,
	                 objname ? uc_ubus_signatures_cb : uc_ubus_objects_cb,
	                 res);

	if (rv != UBUS_STATUS_OK) {
		ucv_put(res);
		err_return(rv, NULL);
	}

	ok_return(res);
}

static void
uc_ubus_call_cb(struct ubus_request *req, int type, struct blob_attr *msg)
{
	uc_ubus_call_res_t *res = req->priv;
	uc_value_t *val;

	val = msg ? blob_array_to_ucv(NULL, blob_data(msg), blob_len(msg), true) : NULL;

	if (res->mret) {
		if (!res->res)
			res->res = ucv_array_new(NULL);

		ucv_array_push(res->res, val);
	}
	else if (!res->res) {
		res->res = val;
	}
}

static void
uc_ubus_vm_handle_exception(uc_vm_t *vm)
{
	uc_value_t *exh, *val;

	exh = uc_vm_registry_get(vm, "ubus.ex_handler");
	if (!ucv_is_callable(exh))
		goto error;

	val = uc_vm_exception_object(vm);
	uc_vm_stack_push(vm, ucv_get(exh));
	uc_vm_stack_push(vm, val);

	if (uc_vm_call(vm, false, 1) != EXCEPTION_NONE)
		goto error;

	ucv_put(uc_vm_stack_pop(vm));
	return;

error:
	uloop_end();
}

static bool
uc_ubus_vm_call(uc_vm_t *vm, bool mcall, size_t nargs)
{
	if (uc_vm_call(vm, mcall, nargs) == EXCEPTION_NONE)
		return true;

	uc_ubus_vm_handle_exception(vm);

	return false;
}

static void
uc_ubus_call_user_cb(uc_ubus_deferred_t *defer, int ret, uc_value_t *reply)
{
	uc_value_t *this = ucv_get(defer->res);
	uc_vm_t *vm = defer->vm;
	uc_value_t *func;

	func = ucv_resource_value_get(this, DEFER_RES_CB);

	if (ucv_is_callable(func)) {
		uc_vm_stack_push(vm, ucv_get(this));
		uc_vm_stack_push(vm, ucv_get(func));
		uc_vm_stack_push(vm, ucv_int64_new(ret));
		uc_vm_stack_push(vm, ucv_get(reply));

		if (uc_vm_call(vm, true, 2) == EXCEPTION_NONE)
			ucv_put(uc_vm_stack_pop(vm));
	}

	uc_ubus_put_res(&defer->res);
	ucv_put(this);
}

static void
uc_ubus_call_data_cb(struct ubus_request *req, int type, struct blob_attr *msg)
{
	uc_ubus_deferred_t *defer = container_of(req, uc_ubus_deferred_t, request);

	if (defer->response != NULL)
		return;

	defer->response = blob_array_to_ucv(defer->vm, blob_data(msg), blob_len(msg), true);
	ucv_resource_value_set(defer->res, DEFER_RES_RESPONSE, defer->response);
}

static void
uc_ubus_call_data_user_cb(struct ubus_request *req, int type, struct blob_attr *msg)
{
	uc_ubus_deferred_t *defer = container_of(req, uc_ubus_deferred_t, request);
	uc_vm_t *vm = defer->vm;
	uc_value_t *func, *reply;

	func = ucv_resource_value_get(defer->res, DEFER_RES_DATA_CB);

	if (ucv_is_callable(func)) {
		reply = blob_array_to_ucv(vm, blob_data(msg), blob_len(msg), true);

		uc_vm_stack_push(vm, ucv_get(defer->res));
		uc_vm_stack_push(vm, ucv_get(func));
		uc_vm_stack_push(vm, reply);

		if (uc_ubus_vm_call(vm, true, 1))
			ucv_put(uc_vm_stack_pop(vm));
	}
}

static void
uc_ubus_call_fd_cb(struct ubus_request *req, int fd)
{
	uc_ubus_deferred_t *defer = container_of(req, uc_ubus_deferred_t, request);
	uc_value_t *func = defer->fd_callback;
	uc_vm_t *vm = defer->vm;

	if (defer->complete)
		return;

	if (ucv_is_callable(func)) {
		uc_vm_stack_push(vm, ucv_get(defer->res));
		uc_vm_stack_push(vm, ucv_get(func));
		uc_vm_stack_push(vm, ucv_int64_new(fd));

		if (uc_ubus_vm_call(vm, true, 1))
			ucv_put(uc_vm_stack_pop(vm));
	}
}

static void
uc_ubus_call_done_cb(struct ubus_request *req, int ret)
{
	uc_ubus_deferred_t *defer = container_of(req, uc_ubus_deferred_t, request);

	if (defer->complete)
		return;

	defer->complete = true;
	uloop_timeout_cancel(&defer->timeout);

	uc_ubus_call_user_cb(defer, ret, defer->response);
}

static void
uc_ubus_call_timeout_cb(struct uloop_timeout *timeout)
{
	uc_ubus_deferred_t *defer = container_of(timeout, uc_ubus_deferred_t, timeout);

	if (defer->complete)
		return;

	defer->complete = true;
	ubus_abort_request(defer->ctx, &defer->request);

	uc_ubus_call_user_cb(defer, UBUS_STATUS_TIMEOUT, NULL);
}

static int
get_fd(uc_vm_t *vm, uc_value_t *val, bool *handle)
{
	uc_value_t *fn;
	int64_t n;

	fn = ucv_property_get(val, "fileno");

	if (ucv_is_callable(fn)) {
		if (handle)
			*handle = true;

		uc_vm_stack_push(vm, ucv_get(val));
		uc_vm_stack_push(vm, ucv_get(fn));

		if (uc_vm_call(vm, true, 0) != EXCEPTION_NONE)
			return -1;

		val = uc_vm_stack_pop(vm);
		n = ucv_int64_get(val);
		ucv_put(val);
	}
	else {
		n = ucv_int64_get(val);
	}

	if (errno || n < 0 || n > (int64_t)INT_MAX)
		return -1;

	return (int)n;
}

static int
uc_ubus_call_common(uc_vm_t *vm, uc_ubus_connection_t *c, uc_ubus_call_res_t *res,
                    uint32_t id, uc_value_t *funname, uc_value_t *funargs,
                    uc_value_t *fd, uc_value_t *fdcb, uc_value_t *mret)
{
	uc_ubus_deferred_t defer = {};
	enum ubus_msg_status rv;
	int fd_val = -1;

	enum {
		RET_MODE_SINGLE,
		RET_MODE_MULTIPLE,
		RET_MODE_IGNORE,
	} ret_mode = RET_MODE_SINGLE;

	const char * const ret_modes[] = {
		[RET_MODE_SINGLE] = "single",
		[RET_MODE_MULTIPLE] = "multiple",
		[RET_MODE_IGNORE] = "ignore",
	};

	if (ucv_type(mret) == UC_STRING) {
		const char *str = ucv_string_get(mret);
		size_t i;

		for (i = 0; i < ARRAY_SIZE(ret_modes); i++)
			if (!strcmp(str, ret_modes[i]))
				break;

		if (i == ARRAY_SIZE(ret_modes))
			errval_return(UBUS_STATUS_INVALID_ARGUMENT,
			              "Invalid return mode argument");

		ret_mode = i;
	}
	else if (ucv_type(mret) == UC_BOOLEAN) {
		ret_mode = ucv_boolean_get(mret);
	}
	else if (ret_mode) {
		errval_return(UBUS_STATUS_INVALID_ARGUMENT,
		              "Invalid return mode argument");
	}

	blob_buf_init(&c->buf, 0);

	if (funargs)
		ucv_object_to_blob(funargs, &c->buf);

	if (fd) {
		fd_val = get_fd(vm, fd, NULL);

		if (fd_val < 0)
			errval_return(UBUS_STATUS_INVALID_ARGUMENT,
			              "Invalid file descriptor argument");
	}

	res->mret = (ret_mode == RET_MODE_MULTIPLE);

	rv = ubus_invoke_async_fd(&c->ctx, id, ucv_string_get(funname),
	                          c->buf.head, &defer.request, fd_val);

	defer.vm = vm;
	defer.ctx = &c->ctx;
	defer.request.data_cb = uc_ubus_call_cb;
	defer.request.priv = res;

	if (ucv_is_callable(fdcb)) {
		defer.request.fd_cb = uc_ubus_call_fd_cb;
		defer.fd_callback = fdcb;
	}

	if (rv == UBUS_STATUS_OK) {
		if (ret_mode == RET_MODE_IGNORE)
			ubus_abort_request(&c->ctx, &defer.request);
		else
			rv = ubus_complete_request(&c->ctx, &defer.request, c->timeout * 1000);
	}

	return rv;
}

/**
 * Invoke a ubus method synchronously.
 *
 * Calls the specified method on a ubus object and waits for the response.
 *
 * Returns the method response data, or an array of responses if multiple
 * replies were received.
 *
 * Returns `null` if the call failed.
 *
 * @function module:ubus.connection#call
 *
 * @param {string|number} object
 * The object name (string) or object ID (number) to call the method on.
 *
 * @param {string} method
 * The name of the method to invoke.
 *
 * @param {Object} [data]
 * Optional method arguments as an object with field names and values.
 *
 * @param {string|boolean} [return="single"]
 * Controls how multiple responses are handled: `"single"` returns only
 * the first response, `"multiple"` returns an array of all responses,
 * `"ignore"` discards the response.
 *
 * @param {number} [fd]
 * Optional file descriptor to send along with the call.
 *
 * @param {function} [fd_cb]
 * Optional callback function invoked when a file descriptor is received.
 *
 * @returns {?*}
 */
static uc_value_t *
uc_ubus_call(uc_vm_t *vm, size_t nargs)
{
	uc_value_t *obj, *funname, *funargs, *fd, *fdcb, *mret = NULL;
	uc_ubus_call_res_t res = { 0 };
	uc_ubus_connection_t *c;
	enum ubus_msg_status rv;
	uint32_t id;

	args_get_named(vm, nargs,
	               "object", 0, REQUIRED, &obj,
	               "method", UC_STRING, REQUIRED, &funname,
	               "data", UC_OBJECT, OPTIONAL, &funargs,
	               "return", 0, OPTIONAL, &mret,
	               "fd", 0, NAMED, &fd,
	               "fd_cb", UC_CLOSURE, NAMED, &fdcb);

	conn_get(vm, &c);

	if (ucv_type(obj) == UC_INTEGER) {
		id = ucv_int64_get(obj);
	}
	else if (ucv_type(obj) == UC_STRING) {
		rv = ubus_lookup_id(&c->ctx, ucv_string_get(obj), &id);

		if (rv != UBUS_STATUS_OK)
			err_return(rv, "Failed to resolve object name '%s'",
			           ucv_string_get(obj));
	}
	else {
		err_return(UBUS_STATUS_INVALID_ARGUMENT,
		           "Argument object is not string or integer");
	}

	rv = uc_ubus_call_common(vm, c, &res, id, funname, funargs, fd, fdcb, mret);

	if (rv != UBUS_STATUS_OK) {
		if (ucv_type(obj) == UC_STRING)
			err_return(rv, "Failed to invoke function '%s' on object '%s'",
			           ucv_string_get(funname), ucv_string_get(obj));
		else
			err_return(rv, "Failed to invoke function '%s' on system object %d",
			           ucv_string_get(funname), (int)ucv_int64_get(obj));
	}

	ok_return(res.res);
}

/**
 * Send a request on a channel connection.
 *
 * Similar to call() but uses object ID 0 for channel-based communication.
 *
 * Returns the method response data.
 *
 * Returns `null` if the request failed.
 *
 * @function module:ubus.channel#request
 *
 * @param {string} method
 * The name of the method to invoke.
 *
 * @param {Object} [data]
 * Optional method arguments as an object with field names and values.
 *
 * @param {string|boolean} [return="single"]
 * Controls how multiple responses are handled.
 *
 * @param {number} [fd]
 * Optional file descriptor to send along with the request.
 *
 * @param {function} [fd_cb]
 * Optional callback function invoked when a file descriptor is received.
 *
 * @returns {?*}
 *
 * @example
 * const chan = open_channel(…);
 * const result = chan.request("method_name", { arg: "value" });
 */
static uc_value_t *
uc_ubus_chan_request(uc_vm_t *vm, size_t nargs)
{
	uc_value_t *funname, *funargs, *fd, *fdcb, *mret = NULL;
	uc_ubus_call_res_t res = { 0 };
	uc_ubus_connection_t *c;
	enum ubus_msg_status rv;

	args_get_named(vm, nargs,
	               "method", UC_STRING, REQUIRED, &funname,
	               "data", UC_OBJECT, OPTIONAL, &funargs,
	               "return", 0, OPTIONAL, &mret,
	               "fd", 0, NAMED, &fd,
	               "fd_cb", UC_CLOSURE, NAMED, &fdcb);

	conn_get(vm, &c);

	rv = uc_ubus_call_common(vm, c, &res, 0, funname, funargs, fd, fdcb, mret);

	if (rv != UBUS_STATUS_OK)
		err_return(rv, "Failed to send request '%s' on channel",
		           ucv_string_get(funname));

	ok_return(res.res);
}

static int
uc_ubus_defer_common(uc_vm_t *vm, uc_ubus_connection_t *c, uc_ubus_call_res_t *res,
                     uint32_t id, uc_value_t *funname, uc_value_t *funargs,
                     uc_value_t *fd, uc_value_t *fdcb, uc_value_t *replycb,
                     uc_value_t *datacb)
{
	uc_ubus_deferred_t *defer = NULL;
	enum ubus_msg_status rv;
	int fd_val = -1;

	blob_buf_init(&c->buf, 0);

	if (funargs)
		ucv_object_to_blob(funargs, &c->buf);

	if (fd) {
		fd_val = get_fd(vm, fd, NULL);

		if (fd_val < 0)
			errval_return(UBUS_STATUS_INVALID_ARGUMENT,
			              "Invalid file descriptor argument");
	}

	res->res = ucv_resource_create_ex(vm, "ubus.deferred", (void **)&defer, __DEFER_RES_MAX, sizeof(*defer));

	if (!defer)
		errval_return(UBUS_STATUS_UNKNOWN_ERROR, "Out of memory");

	rv = ubus_invoke_async_fd(&c->ctx, id, ucv_string_get(funname),
	                          c->buf.head, &defer->request, fd_val);

	if (rv == UBUS_STATUS_OK) {
		defer->vm = vm;
		defer->ctx = &c->ctx;
		defer->res = ucv_get(res->res);
		ucv_resource_persistent_set(defer->res, true);
		ucv_resource_value_set(defer->res, DEFER_RES_CONN, ucv_get(c->res));
		ucv_resource_value_set(defer->res, DEFER_RES_CB, ucv_get(replycb));
		ucv_resource_value_set(defer->res, DEFER_RES_FD, ucv_get(fd));
		ucv_resource_value_set(defer->res, DEFER_RES_DATA_CB, ucv_get(datacb));

		if (ucv_is_callable(datacb))
			defer->request.data_cb = uc_ubus_call_data_user_cb;
		else
			defer->request.data_cb = uc_ubus_call_data_cb;

		if (ucv_is_callable(fdcb)) {
			defer->request.fd_cb = uc_ubus_call_fd_cb;
			defer->fd_callback = fdcb;
			ucv_resource_value_set(defer->res, DEFER_RES_FD_CB, ucv_get(fdcb));
		}

		defer->request.complete_cb = uc_ubus_call_done_cb;

		ubus_complete_request_async(&c->ctx, &defer->request);

		defer->timeout.cb = uc_ubus_call_timeout_cb;
		uloop_timeout_set(&defer->timeout, c->timeout * 1000);
	}
	else {
		uc_vm_stack_push(vm, ucv_get(replycb));
		uc_vm_stack_push(vm, ucv_int64_new(rv));

		if (uc_ubus_vm_call(vm, false, 1))
			ucv_put(uc_vm_stack_pop(vm));

		ucv_put(res->res);
	}

	return rv;
}

/**
 * Invoke a ubus method asynchronously.
 *
 * Initiates a non-blocking call to the specified method on a ubus object.
 * The provided callback will be invoked when the response is received or on
 * timeout.
 *
 * Returns a deferred request resource representing the pending operation.
 *
 * Returns `null` if the deferred call could not be initiated.
 *
 * @function module:ubus.connection#defer
 *
 * @param {string} object
 * The object name to call the method on.
 *
 * @param {string} method
 * The name of the method to invoke.
 *
 * @param {Object} [data]
 * Optional method arguments as an object with field names and values.
 *
 * @param {function} [cb]
 * Callback function invoked when the operation completes. Receives status
 * code and response data as arguments.
 *
 * @param {function} [data_cb]
 * Optional callback invoked for intermediate data notifications.
 *
 * @param {number} [fd]
 * Optional file descriptor to send along with the call.
 *
 * @param {function} [fd_cb]
 * Optional callback function invoked when a file descriptor is received.
 *
 * @returns {?module:ubus.deferred}
 *
 * @example
 * // Asynchronous call with callback - typical pattern for RPC handlers
 * const conn = connect();
 *
 * const req = conn.defer("some.object", "some_method", {}, (rc, data) => {
 *     if (rc == 0)
 *         printf("Result: %.J\n", data);
 * });
 *
 * @example
 * // Persistent connection pattern - avoid GC by keeping reference
 * const ubus = connect();
 *
 * function get_status(req) {
 *     // Use persistent ubus connection for async calls
 *     return ubus.defer("some.object", "some_method", {}, (rc, data) => {
 *         req.reply({ result: data });
 *     });
 * }
 */
static uc_value_t *
uc_ubus_defer(uc_vm_t *vm, size_t nargs)
{
	uc_value_t *objname, *funname, *funargs, *replycb, *datacb, *fd, *fdcb = NULL;
	uc_ubus_call_res_t res = { 0 };
	uc_ubus_connection_t *c;
	uint32_t id;
	int rv;

	conn_get(vm, &c);

	args_get_named(vm, nargs,
	               "object", UC_STRING, REQUIRED, &objname,
	               "method", UC_STRING, REQUIRED, &funname,
	               "data", UC_OBJECT, OPTIONAL, &funargs,
	               "cb", UC_CLOSURE, OPTIONAL, &replycb,
	               "data_cb", UC_CLOSURE, OPTIONAL, &datacb,
	               "fd", 0, NAMED, &fd,
	               "fd_cb", UC_CLOSURE, NAMED, &fdcb);

	rv = ubus_lookup_id(&c->ctx, ucv_string_get(objname), &id);

	if (rv != UBUS_STATUS_OK)
		err_return(rv, "Failed to resolve object name '%s'",
		           ucv_string_get(objname));

	rv = uc_ubus_defer_common(vm, c, &res, id, funname, funargs, fd, fdcb, replycb, datacb);

	if (rv != UBUS_STATUS_OK)
		err_return(rv, "Failed to invoke function '%s' on object '%s'",
		           ucv_string_get(funname), ucv_string_get(objname));

	ok_return(res.res);
}

/**
 * Send an asynchronous request on a channel connection.
 *
 * Similar to defer() but uses object ID 0 for channel-based communication.
 *
 * Returns a deferred request resource representing the pending operation.
 *
 * Returns `null` if the deferred call could not be initiated.
 *
 * @function module:ubus.channel#defer
 *
 * @param {string} method
 * The name of the method to invoke.
 *
 * @param {Object} [data]
 * Optional method arguments as an object with field names and values.
 *
 * @param {function} [cb]
 * Callback function invoked when the operation completes.
 *
 * @param {function} [data_cb]
 * Optional callback invoked for intermediate data notifications.
 *
 * @param {number} [fd]
 * Optional file descriptor to send along with the request.
 *
 * @param {function} [fd_cb]
 * Optional callback function invoked when a file descriptor is received.
 *
 * @returns {?module:ubus.deferred}
 *
 * @example
 * const chan = open_channel(…);
 * const req = chan.defer("method_name", { arg: "value" },
 *     (status, data) => {
 *         printf("Status: %d\n", status);
 *     });
 */
static uc_value_t *
uc_ubus_chan_defer(uc_vm_t *vm, size_t nargs)
{
	uc_value_t *funname, *funargs, *replycb, *datacb, *fd, *fdcb = NULL;
	uc_ubus_call_res_t res = { 0 };
	uc_ubus_connection_t *c;
	int rv;

	conn_get(vm, &c);

	args_get_named(vm, nargs,
	               "method", UC_STRING, REQUIRED, &funname,
	               "data", UC_OBJECT, OPTIONAL, &funargs,
	               "cb", UC_CLOSURE, OPTIONAL, &replycb,
	               "data_cb", UC_CLOSURE, OPTIONAL, &datacb,
	               "fd", 0, NAMED, &fd,
	               "fd_cb", UC_CLOSURE, NAMED, &fdcb);

	rv = uc_ubus_defer_common(vm, c, &res, 0, funname, funargs, fd, fdcb, replycb, datacb);

	if (rv != UBUS_STATUS_OK)
		err_return(rv, "Failed to invoke function '%s' on channel",
		           ucv_string_get(funname));

	ok_return(res.res);
}


/*
 * ubus object request context functions
 * --------------------------------------------------------------------------
 */

static void
uc_ubus_request_finish_common(uc_ubus_request_t *callctx, int code)
{
	int fd;

	fd = ubus_request_get_caller_fd(&callctx->req);

	if (fd >= 0)
		close(fd);

	callctx->replied = true;
	uloop_timeout_cancel(&callctx->timeout);
	ubus_complete_deferred_request(callctx->ctx, &callctx->req, code);
}

static void
uc_ubus_request_send_reply(uc_ubus_request_t *callctx, uc_value_t *reply)
{
	if (!reply)
		return;

	blob_buf_init(&buf, 0);
	ucv_object_to_blob(reply, &buf);
	ubus_send_reply(callctx->ctx, &callctx->req, buf.head);
}

static void
uc_ubus_request_finish(uc_ubus_request_t *callctx, int code)
{
	if (callctx->replied)
		return;

	uc_ubus_request_finish_common(callctx, code);
	uc_ubus_put_res(&callctx->res);
}

static void
uc_ubus_request_timeout(struct uloop_timeout *timeout)
{
	uc_ubus_request_t *callctx = container_of(timeout, uc_ubus_request_t, timeout);

	uc_ubus_request_finish(callctx, UBUS_STATUS_TIMEOUT);
}

/**
 * Send a reply to a deferred ubus method call.
 *
 * Sends the specified reply data to the caller of a deferred method
 * request. After sending the reply, the request context is finished unless
 * `more` is set to `true`.
 *
 * Returns `true` on success.
 *
 * Returns `null` if an error occurred or the reply was already sent.
 *
 * @function module:ubus.request#reply
 *
 * @param {Object} [reply]
 * The reply data to send as an object with field names and values.
 *
 * @param {number} [rcode=0]
 * Optional status code to return. Use negative values to indicate more
 * replies will follow.
 *
 * @returns {?boolean}
 *
 * @example
 * // In a published object method handler
 * publish("my.service", {
 *     "hello": (req, msg) => {
 *         req.reply({ message: "Hello, " + msg.name });
 *     }
 * });
 */
static uc_value_t *
uc_ubus_request_reply(uc_vm_t *vm, size_t nargs)
{
	uc_ubus_request_t *callctx = uc_fn_thisval("ubus.request");
	int64_t code = UBUS_STATUS_OK;
	uc_value_t *reply, *rcode;
	bool more = false;

	if (!callctx)
		err_return(UBUS_STATUS_INVALID_ARGUMENT, "Invalid call context");

	args_get(vm, nargs,
	         "reply", UC_OBJECT, true, &reply,
	         "rcode", UC_INTEGER, true, &rcode);

	if (callctx->replied)
		err_return(UBUS_STATUS_INVALID_ARGUMENT, "Reply has already been sent");

	if (rcode) {
		code = ucv_int64_get(rcode);

		if (errno == ERANGE || code < -1 || code > __UBUS_STATUS_LAST)
			code = UBUS_STATUS_UNKNOWN_ERROR;

		if (code < 0)
			more = true;
	}

	uc_ubus_request_send_reply(callctx, reply);

	if (!more)
		uc_ubus_request_finish(callctx, code);

	ok_return(ucv_boolean_new(true));
}

/**
 * Defer completion of a ubus method call.
 *
 * Marks the current request as deferred, allowing the handler to complete
 * the request asynchronously at a later time by calling reply().
 *
 * Returns `true` on success.
 *
 * Returns `null` if an error occurred.
 *
 * @function module:ubus.request#defer
 *
 * @returns {?boolean}
 *
 * @example
 * // In a published object method handler
 * publish("my.service", {
 *     "async_method": (req, msg) => {
 *         req.defer();
 *         // Do async work...
 *         req.reply({ result: "done" });
 *     }
 * });
 */
static uc_value_t *
uc_ubus_request_defer(uc_vm_t *vm, size_t nargs)
{
	uc_ubus_request_t *callctx = uc_fn_thisval("ubus.request");

	if (!callctx)
		return NULL;

	callctx->deferred = true;
	return ucv_boolean_new(true);
}

/**
 * Get the caller's file descriptor from a ubus method call.
 *
 * Returns the UNIX file descriptor number that was passed by the caller,
 * or -1 if no file descriptor was sent.
 *
 * @function module:ubus.request#get_fd
 *
 * @returns {number}
 * The file descriptor number, or -1 if none was provided.
 */
static uc_value_t *
uc_ubus_request_get_fd(uc_vm_t *vm, size_t nargs)
{
	uc_ubus_request_t *callctx = uc_fn_thisval("ubus.request");

	if (!callctx)
		return NULL;

	return ucv_int64_new(ubus_request_get_caller_fd(&callctx->req));
}

/**
 * Set a file descriptor to send with a ubus method reply.
 *
 * Associates a file descriptor with the current request to be sent back to
 * the caller along with the reply.
 *
 * Returns `true` on success.
 *
 * Returns `null` if an error occurred.
 *
 * @function module:ubus.request#set_fd
 *
 * @param {number} fd
 * The file descriptor number to send.
 *
 * @returns {?boolean}
 *
 * @example
 * // In a published object method handler
 * publish("my.service", {
 *     "get_fd": (req, msg) => {
 *         let fd = some_file_descriptor;
 *         req.set_fd(fd);
 *         req.reply({ info: "fd sent" });
 *     }
 * });
 */
static uc_value_t *
uc_ubus_request_set_fd(uc_vm_t *vm, size_t nargs)
{
	uc_ubus_request_t *callctx = uc_fn_thisval("ubus.request");
	int fd;

	if (!callctx)
		err_return(UBUS_STATUS_INVALID_ARGUMENT, "Invalid call context");

	fd = get_fd(vm, uc_fn_arg(0), NULL);

	if (fd < 0)
		err_return(UBUS_STATUS_INVALID_ARGUMENT, "Invalid file descriptor");

	ubus_request_set_fd(callctx->ctx, &callctx->req, fd);

	return ucv_boolean_new(true);
}

/**
 * Finish a ubus method call with an error status.
 *
 * Completes the current deferred request with the specified error status
 * code without sending any reply data.
 *
 * Returns `true` on success.
 *
 * Returns `null` if an error occurred or the reply was already sent.
 *
 * @function module:ubus.request#error
 *
 * @param {number} [rcode=UBUS_STATUS_UNKNOWN_ERROR]
 * The error status code to return.
 *
 * @returns {?boolean}
 *
 * @example
 * // In a published object method handler
 * publish("my.service", {
 *     "process": (req, msg) => {
 *         if (!msg.input) {
 *             req.error(UBUS_STATUS_INVALID_ARGUMENT);
 *             return;
 *         }
 *         req.reply({ result: "ok" });
 *     }
 * });
 */
static uc_value_t *
uc_ubus_request_error(uc_vm_t *vm, size_t nargs)
{
	uc_ubus_request_t *callctx = uc_fn_thisval("ubus.request");
	uc_value_t *rcode = uc_fn_arg(0);
	int64_t code;

	if (!callctx)
		err_return(UBUS_STATUS_INVALID_ARGUMENT, "Invalid call context");

	args_get(vm, nargs,
	         "rcode", UC_INTEGER, false, &rcode);

	if (callctx->replied)
		err_return(UBUS_STATUS_INVALID_ARGUMENT, "Reply has already been sent");

	code = ucv_int64_get(rcode);

	if (errno == ERANGE || code < 0 || code > __UBUS_STATUS_LAST)
		code = UBUS_STATUS_UNKNOWN_ERROR;

	uc_ubus_request_finish(callctx, code);

	ok_return(ucv_boolean_new(true));
}


/*
 * ubus object notify
 * --------------------------------------------------------------------------
 */

/**
 * Check if a notification request has completed.
 *
 * Returns `true` if the notification request has finished.
 *
 * Returns `false` if the request is still pending.
 *
 * @function module:ubus.notify#completed
 *
 * @returns {boolean}
 *
 * @example
 * const n = obj.notify("method", { data: "value" });
 * if (n.completed()) {
 *     printf("Notification sent\n");
 * }
 */
static uc_value_t *
uc_ubus_notify_completed(uc_vm_t *vm, size_t nargs)
{
	uc_ubus_notify_t *notifyctx = uc_fn_thisval("ubus.notify");

	ok_return(ucv_boolean_new(notifyctx->complete));
}

/**
 * Abort a pending notification request.
 *
 * Cancels an asynchronous notification request that has not yet completed.
 *
 * Returns `true` if the request was aborted.
 *
 * Returns `false` if the request was already completed.
 *
 * @function module:ubus.notify#abort
 *
 * @returns {boolean}
 *
 * @example
 * const n = obj.notify("method", { data: "value" });
 * if (!n.completed()) {
 *     n.abort();
 * }
 */
static uc_value_t *
uc_ubus_notify_abort(uc_vm_t *vm, size_t nargs)
{
	uc_ubus_notify_t *notifyctx = uc_fn_thisval("ubus.notify");

	if (notifyctx->complete)
		ok_return(ucv_boolean_new(false));

	ubus_abort_request(notifyctx->ctx, &notifyctx->req.req);
	notifyctx->complete = true;
	uc_ubus_put_res(&notifyctx->res);

	ok_return(ucv_boolean_new(true));
}

static void
uc_ubus_object_notify_data_cb(struct ubus_notify_request *req, int type, struct blob_attr *msg)
{
	uc_ubus_notify_t *notifyctx = (uc_ubus_notify_t *)req;
	uc_vm_t *vm = notifyctx->vm;
	uc_value_t *this, *func;

	this = notifyctx->res;
	func = ucv_resource_value_get(this, NOTIFY_RES_DATA_CB);

	if (ucv_is_callable(func)) {
		uc_vm_stack_push(vm, ucv_get(this));
		uc_vm_stack_push(vm, ucv_get(func));
		uc_vm_stack_push(vm, ucv_int64_new(type));
		uc_vm_stack_push(vm, blob_array_to_ucv(vm, blob_data(msg), blob_len(msg), true));

		if (uc_ubus_vm_call(vm, true, 2))
			ucv_put(uc_vm_stack_pop(vm));
	}
}

static void
uc_ubus_object_notify_status_cb(struct ubus_notify_request *req, int idx, int ret)
{
	uc_ubus_notify_t *notifyctx = (uc_ubus_notify_t *)req;
	uc_vm_t *vm = notifyctx->vm;
	uc_value_t *this, *func;

	this = notifyctx->res;
	func = ucv_resource_value_get(this, NOTIFY_RES_STATUS_CB);

	if (ucv_is_callable(func)) {
		uc_vm_stack_push(vm, ucv_get(this));
		uc_vm_stack_push(vm, ucv_get(func));
		uc_vm_stack_push(vm, ucv_int64_new(idx));
		uc_vm_stack_push(vm, ucv_int64_new(ret));

		if (uc_ubus_vm_call(vm, true, 2))
			ucv_put(uc_vm_stack_pop(vm));
	}
}

static void
uc_ubus_object_notify_complete_cb(struct ubus_notify_request *req, int idx, int ret)
{
	uc_ubus_notify_t *notifyctx = (uc_ubus_notify_t *)req;
	uc_vm_t *vm = notifyctx->vm;
	uc_value_t *this, *func;

	this = ucv_get(notifyctx->res);
	func = ucv_resource_value_get(this, NOTIFY_RES_CB);

	if (ucv_is_callable(func)) {
		uc_vm_stack_push(vm, ucv_get(this));
		uc_vm_stack_push(vm, ucv_get(func));
		uc_vm_stack_push(vm, ucv_int64_new(idx));
		uc_vm_stack_push(vm, ucv_int64_new(ret));

		if (uc_ubus_vm_call(vm, true, 2))
			ucv_put(uc_vm_stack_pop(vm));
	}

	notifyctx->complete = true;
	uc_ubus_put_res(&notifyctx->res);
	ucv_put(this);
}

/**
 * Send a notification from a ubus object.
 *
 * Sends an asynchronous notification of the specified type to all
 * subscribers of the object. Optional callbacks can be provided to handle
 * data, status, and completion events.
 *
 * Returns a notification request resource for asynchronous operations.
 *
 * Returns a status code number when a synchronous timeout is specified.
 *
 * Returns `null` if the notification could not be sent.
 *
 * @function module:ubus.object#notify
 *
 * @param {string} type
 * The notification type string.
 *
 * @param {Object} [data]
 * Optional notification data as an object with field names and values.
 *
 * @param {function} [data_cb]
 * Optional callback invoked for each data notification received.
 *
 * @param {function} [status_cb]
 * Optional callback invoked for status updates.
 *
 * @param {function} [cb]
 * Optional callback invoked when the notification operation completes.
 *
 * @param {number} [timeout]
 * Optional timeout in milliseconds. If specified, the operation waits
 * synchronously for completion.
 *
 * @returns {?module:ubus.notify|?number}
 *
 * @example
 * const obj = publish("my.service", {
 *     "trigger": (req, msg) => {
 *         obj.notify("update", { key: "value" }, (idx, ret) => {
 *             printf("Notification %d: status %d\n", idx, ret);
 *         });
 *         req.reply({ sent: true });
 *     }
 * });
 */
static uc_value_t *
uc_ubus_object_notify(uc_vm_t *vm, size_t nargs)
{
	uc_value_t *typename, *message, *data_cb, *status_cb, *complete_cb, *timeout;
	uc_ubus_object_t *uuobj = uc_fn_thisval("ubus.object");
	uc_ubus_notify_t *notifyctx = NULL;
	uc_value_t *res;
	int64_t t;
	int rv = UBUS_STATUS_UNKNOWN_ERROR;

	if (!uuobj)
		err_return(UBUS_STATUS_INVALID_ARGUMENT, "Invalid object context");

	args_get_named(vm, nargs,
	              "type", UC_STRING, REQUIRED, &typename,
	              "data", UC_OBJECT, OPTIONAL, &message,
	              "data_cb", UC_CLOSURE, OPTIONAL, &data_cb,
	              "status_cb", UC_CLOSURE, OPTIONAL, &status_cb,
	              "cb", UC_CLOSURE, OPTIONAL, &complete_cb,
	              "timeout", UC_INTEGER, OPTIONAL, &timeout);

	t = timeout ? ucv_int64_get(timeout) : -1;

	if (errno)
		err_return(UBUS_STATUS_INVALID_ARGUMENT,
		           "Invalid timeout value: %s", strerror(errno));

	res = ucv_resource_create_ex(vm, "ubus.notify", (void **)&notifyctx, __NOTIFY_RES_MAX, sizeof(*notifyctx));

	if (!notifyctx)
		err_return(rv, "Out of memory");

	notifyctx->vm = vm;
	notifyctx->ctx = uuobj->ctx;

	blob_buf_init(&buf, 0);

	if (message)
		ucv_object_to_blob(message, &buf);

	rv = ubus_notify_async(uuobj->ctx, &uuobj->obj,
	                       ucv_string_get(typename), buf.head,
	                       &notifyctx->req);

	if (rv != UBUS_STATUS_OK) {
		ucv_put(res);
		err_return(rv, "Failed to send notification");
	}

	notifyctx->res = ucv_get(res);
	notifyctx->req.data_cb = uc_ubus_object_notify_data_cb;
	notifyctx->req.status_cb = uc_ubus_object_notify_status_cb;
	notifyctx->req.complete_cb = uc_ubus_object_notify_complete_cb;

	ucv_resource_value_set(res, NOTIFY_RES_CONN, ucv_get(uuobj->res));
	ucv_resource_value_set(res, NOTIFY_RES_CB, ucv_get(complete_cb));
	ucv_resource_value_set(res, NOTIFY_RES_DATA_CB, ucv_get(data_cb));
	ucv_resource_value_set(res, NOTIFY_RES_STATUS_CB, ucv_get(status_cb));

	if (t >= 0) {
		rv = ubus_complete_request(uuobj->ctx, &notifyctx->req.req, t);

		ucv_put(res);

		ok_return(ucv_int64_new(rv));
	}

	ucv_resource_persistent_set(res, true);
	ubus_complete_request_async(uuobj->ctx, &notifyctx->req.req);

	ok_return(res);
}


/*
 * ubus object remove
 * --------------------------------------------------------------------------
 */

static int
uc_ubus_object_remove_common(uc_ubus_object_t *uuobj)
{
	int rv = ubus_remove_object(uuobj->ctx, &uuobj->obj);

	if (rv != UBUS_STATUS_OK)
		return rv;

	uc_ubus_put_res(&uuobj->res);

	return rv;
}

/**
 * Remove a ubus object from the bus.
 *
 * Unregisters the object from the ubus bus, making it no longer accessible
 * to other clients.
 *
 * Returns `true` on success.
 *
 * Returns `null` if the object could not be removed.
 *
 * @function module:ubus.object#remove
 *
 * @returns {?boolean}
 *
 * @example
 * const obj = publish("my.service", { … });
 * // … do work …
 * obj.remove();
 */
static uc_value_t *
uc_ubus_object_remove(uc_vm_t *vm, size_t nargs)
{
	uc_ubus_object_t *uuobj = uc_fn_thisval("ubus.object");
	int rv;

	if (!uuobj)
		err_return(UBUS_STATUS_INVALID_ARGUMENT, "Invalid object context");

	rv = uc_ubus_object_remove_common(uuobj);

	if (rv != UBUS_STATUS_OK)
		err_return(rv, "Failed to remove object");

	ok_return(ucv_boolean_new(true));
}


/*
 * ubus object subscription status
 */

/**
 * Check if a ubus object has subscribers.
 *
 * Returns `true` if there are active subscribers to the object.
 *
 * Returns `false` if no subscribers are currently connected.
 *
 * @function module:ubus.object#subscribed
 *
 * @returns {boolean}
 *
 * @example
 * const obj = publish("my.service", {
 *     "trigger": (req, msg) => {
 *         if (obj.subscribed()) {
 *             obj.notify("update", { data: "value" });
 *         }
 *         req.reply({ notified: obj.subscribed() });
 *     }
 * });
 */
static uc_value_t *
uc_ubus_object_subscribed(uc_vm_t *vm, size_t nargs)
{
	uc_ubus_object_t *uuobj = uc_fn_thisval("ubus.object");

	if (!uuobj)
		err_return(UBUS_STATUS_INVALID_ARGUMENT, "Invalid object context");

	ok_return(ucv_boolean_new(uuobj->obj.has_subscribers));
}


/*
 * ubus object method call handling
 * --------------------------------------------------------------------------
 */

static int
uc_ubus_object_call_args(struct ubus_object *obj, const char *ubus_method_name,
                         struct blob_attr *msg, uc_value_t **res)
{
	uc_ubus_object_t *uuobj = (uc_ubus_object_t *)obj;
	const struct ubus_method *method = NULL;
	const struct blobmsg_hdr *hdr;
	struct blob_attr *attr;
	size_t len;
	bool found;
	int i;

	for (i = 0; i < obj->n_methods; i++) {
		if (!strcmp(obj->methods[i].name, ubus_method_name)) {
			method = &obj->methods[i];
			break;
		}
	}

	if (!method)
		return UBUS_STATUS_METHOD_NOT_FOUND;

	len = blob_len(msg);

	__blob_for_each_attr(attr, blob_data(msg), len) {
		if (!blobmsg_check_attr_len(attr, false, len))
			return UBUS_STATUS_INVALID_ARGUMENT;

		if (!blob_is_extended(attr))
			return UBUS_STATUS_INVALID_ARGUMENT;

		hdr = blob_data(attr);
		found = false;

		for (i = 0; i < method->n_policy; i++) {
			if (blobmsg_namelen(hdr) != strlen(method->policy[i].name))
				continue;

			if (strcmp(method->policy[i].name, (char *)hdr->name))
				continue;

			/* named argument found but wrong type */
			if (blob_id(attr) != method->policy[i].type)
				goto inval;

			found = true;
			break;
		}

		/* named argument not found in policy */
		if (!found) {
			/* allow special ubus_rpc_session argument */
			if (!strcmp("ubus_rpc_session", (char *)hdr->name) && blob_id(attr) == BLOBMSG_TYPE_STRING)
				continue;

			goto inval;
		}
	}

	*res = blob_array_to_ucv(uuobj->vm, blob_data(msg), blob_len(msg), true);

	return UBUS_STATUS_OK;

inval:
	*res = NULL;

	return UBUS_STATUS_INVALID_ARGUMENT;
}

static uc_value_t *
uc_ubus_object_call_info(uc_vm_t *vm,
                         struct ubus_context *ctx, struct ubus_request_data *req,
                         struct ubus_object *obj, const char *ubus_method_name)
{
	uc_value_t *info, *o;

	info = ucv_object_new(vm);

	o = ucv_object_new(vm);

	ucv_object_add(o, "user", ucv_string_new(req->acl.user));
	ucv_object_add(o, "group", ucv_string_new(req->acl.group));

	if (req->acl.object)
		ucv_object_add(o, "object", ucv_string_new(req->acl.object));

	ucv_object_add(info, "acl", o);

	o = ucv_object_new(vm);

	ucv_object_add(o, "id", ucv_int64_new(obj->id));

	if (obj->name)
		ucv_object_add(o, "name", ucv_string_new(obj->name));

	if (obj->path)
		ucv_object_add(o, "path", ucv_string_new(obj->path));

	ucv_object_add(info, "object", o);

	if (ubus_method_name)
		ucv_object_add(info, "method", ucv_string_new(ubus_method_name));

	return info;
}

static int
uc_ubus_handle_reply_common(struct ubus_context *ctx,
                              struct ubus_request_data *req,
                              uc_vm_t *vm, uc_value_t *this, uc_value_t *func,
                              uc_value_t *reqproto)
{
	uc_ubus_connection_t *conn = container_of(ctx, uc_ubus_connection_t, ctx);
	uc_ubus_request_t *callctx = NULL;
	uc_value_t *reqobj, *res;
	int rv;

	/* allocate deferred method call context.
	 *
	 * reqproto is a per-call object, so it must be stored in the request
	 * resource's instance-specific prototype slot rather than being attached
	 * to the shared "ubus.request" type prototype. The latter would leak the
	 * per-call object (and everything it references) into the type prototype
	 * for the lifetime of the VM, and cause a use-after-free at teardown:
	 * uc_vm_free() releases the restype prototypes before the final GC, so
	 * the shared type prototype is freed while still pointing at the
	 * per-call object, which is then freed again as an unreachable value.
	 *
	 * ucv_resource_create_with_proto() takes ownership of reqproto and
	 * chains it to the type prototype, so property lookup on reqobj still
	 * falls through to the shared request methods.
	 */
	reqobj = ucv_resource_create_with_proto(vm, "ubus.request",
			(void **)&callctx, 1, sizeof(*callctx), reqproto);

	if (!callctx)
		return UBUS_STATUS_UNKNOWN_ERROR;

	callctx->ctx = ctx;
	callctx->vm = vm;
	ucv_resource_value_set(reqobj, 0, ucv_get(conn->res));

	ubus_defer_request(ctx, req, &callctx->req);

	/* fd is copied to deferred request. ensure it does not get closed early */
	ubus_request_get_caller_fd(req);

	/* push object context, handler and request object onto stack */
	uc_vm_stack_push(vm, ucv_get(this));
	uc_vm_stack_push(vm, ucv_get(func));
	uc_vm_stack_push(vm, ucv_get(reqobj));

	/* execute request handler function */
	switch (uc_vm_call(vm, true, 1)) {
	case EXCEPTION_NONE:
		res = uc_vm_stack_pop(vm);

		/* The handler function invoked a nested aync ubus request and returned it */
		if (ucv_resource_data(res, "ubus.deferred")) {
			/* Install guard timer in case the reply callback is never called */
			callctx->timeout.cb = uc_ubus_request_timeout;
			uloop_timeout_set(&callctx->timeout, 10000 /* FIXME */);
			callctx->res = ucv_get(reqobj);
			ucv_resource_persistent_set(callctx->res, true);
		}

		/* Otherwise, when the function returned an object, treat it as
		* reply data and conclude deferred request immediately */
		else if (ucv_type(res) == UC_OBJECT) {
			blob_buf_init(&buf, 0);
			ucv_object_to_blob(res, &buf);
			ubus_send_reply(ctx, &callctx->req, buf.head);

			uc_ubus_request_finish_common(callctx, UBUS_STATUS_OK);
		}

		/* If neither a deferred ubus request, nor a plain object were
		 * returned and if reqobj.reply() hasn't been called, immediately
		 * finish deferred request with UBUS_STATUS_NO_DATA. */
		else if (!callctx->replied && !callctx->deferred) {
			rv = UBUS_STATUS_NO_DATA;

			if (ucv_type(res) == UC_INTEGER) {
				rv = (int)ucv_int64_get(res);

				if (rv < 0 || rv > __UBUS_STATUS_LAST)
					rv = UBUS_STATUS_UNKNOWN_ERROR;
			}

			uc_ubus_request_finish_common(callctx, rv);
		}

		ucv_put(res);
		break;

	/* if the handler function invoked exit(), forward exit status as ubus
	 * return code, map out of range values to UBUS_STATUS_UNKNOWN_ERROR. */
	case EXCEPTION_EXIT:
		rv = vm->arg.s32;

		if (rv < UBUS_STATUS_OK || rv >= __UBUS_STATUS_LAST)
			rv = UBUS_STATUS_UNKNOWN_ERROR;

		uc_ubus_request_finish_common(callctx, rv);
		break;

	/* treat other exceptions as fatal and halt uloop */
	default:
		uc_ubus_request_finish_common(callctx, UBUS_STATUS_UNKNOWN_ERROR);
		uc_ubus_vm_handle_exception(vm);
		break;
	}

	/* release request object */
	ucv_put(reqobj);

	return UBUS_STATUS_OK;
}

static int
uc_ubus_object_call_cb(struct ubus_context *ctx, struct ubus_object *obj,
                       struct ubus_request_data *req, const char *ubus_method_name,
                       struct blob_attr *msg)
{
	uc_value_t *func, *args = NULL, *reqproto, *methods;
	uc_ubus_object_t *uuobj = (uc_ubus_object_t *)obj;
	int rv;

	methods = ucv_resource_value_get(uuobj->res, OBJ_RES_METHODS);
	func = ucv_object_get(ucv_object_get(methods, ubus_method_name, NULL), "call", NULL);

	if (!ucv_is_callable(func))
		return UBUS_STATUS_METHOD_NOT_FOUND;

	rv = uc_ubus_object_call_args(obj, ubus_method_name, msg, &args);

	if (rv != UBUS_STATUS_OK)
		return rv;

	reqproto = ucv_object_new(uuobj->vm);

	ucv_object_add(reqproto, "args", args);
	ucv_object_add(reqproto, "info",
		uc_ubus_object_call_info(uuobj->vm, ctx, req, obj, ubus_method_name));

	return uc_ubus_handle_reply_common(ctx, req, uuobj->vm, uuobj->res, func, reqproto);
}


/*
 * ubus object registration
 * --------------------------------------------------------------------------
 */

static void
uc_ubus_object_subscribe_cb(struct ubus_context *ctx, struct ubus_object *obj)
{
	uc_ubus_object_t *uuobj = (uc_ubus_object_t *)obj;
	uc_value_t *func;

	func = ucv_resource_value_get(uuobj->res, OBJ_RES_SUB_CB);

	uc_vm_stack_push(uuobj->vm, ucv_get(uuobj->res));
	uc_vm_stack_push(uuobj->vm, ucv_get(func));

	if (uc_ubus_vm_call(uuobj->vm, true, 0))
		ucv_put(uc_vm_stack_pop(uuobj->vm));
}

static bool
uc_ubus_object_methods_validate(uc_value_t *methods)
{
	uc_value_t *func, *args;

	ucv_object_foreach(methods, ubus_method_name, ubus_method_definition) {
		(void)ubus_method_name;

		func = ucv_object_get(ubus_method_definition, "call", NULL);
		args = ucv_object_get(ubus_method_definition, "args", NULL);

		if (!ucv_is_callable(func))
			err_return(UBUS_STATUS_INVALID_ARGUMENT,
			           "Method '%s' field 'call' is not a function value",
			           ubus_method_name);

		if (args) {
			if (ucv_type(args) != UC_OBJECT)
				err_return(UBUS_STATUS_INVALID_ARGUMENT,
				           "Method '%s' field 'args' is not an object value",
				           ubus_method_name);

			ucv_object_foreach(args, ubus_argument_name, ubus_argument_typehint) {
				(void)ubus_argument_name;

				switch (ucv_type(ubus_argument_typehint)) {
				case UC_BOOLEAN:
				case UC_INTEGER:
				case UC_DOUBLE:
				case UC_STRING:
				case UC_ARRAY:
				case UC_OBJECT:
					continue;

				default:
					err_return(UBUS_STATUS_INVALID_ARGUMENT,
					           "Method '%s' field 'args' argument '%s' hint has unsupported type %s",
					           ubus_method_name, ubus_argument_name,
					           ucv_typename(ubus_argument_typehint));
				}
			}
		}
	}

	ok_return(true);
}

static bool
uc_ubus_object_method_register(struct ubus_method *method, const char *ubus_method_name,
                               uc_value_t *ubus_method_arguments)
{
	struct blobmsg_policy *policy;
	enum blobmsg_type type;

	method->name = strdup(ubus_method_name);
	method->policy = calloc(ucv_object_length(ubus_method_arguments), sizeof(*method->policy));
	method->handler = uc_ubus_object_call_cb;

	if (!method->name || !method->policy)
		return false;

	ucv_object_foreach(ubus_method_arguments, ubus_argument_name, ubus_argument_typehint) {
		switch (ucv_type(ubus_argument_typehint)) {
		case UC_BOOLEAN:
			type = BLOBMSG_TYPE_INT8;
			break;

		case UC_INTEGER:
			switch (ucv_int64_get(ubus_argument_typehint)) {
			case 8:
				type = BLOBMSG_TYPE_INT8;
				break;

			case 16:
				type = BLOBMSG_TYPE_INT16;
				break;

			case 64:
				type = BLOBMSG_TYPE_INT64;
				break;

			default:
				type = BLOBMSG_TYPE_INT32;
				break;
			}

			break;

		case UC_DOUBLE:
			type = BLOBMSG_TYPE_DOUBLE;
			break;

		case UC_ARRAY:
			type = BLOBMSG_TYPE_ARRAY;
			break;

		case UC_OBJECT:
			type = BLOBMSG_TYPE_TABLE;
			break;

		default:
			type = BLOBMSG_TYPE_STRING;
			break;
		}

		policy = (struct blobmsg_policy *)&method->policy[method->n_policy++];
		policy->type = type;
		policy->name = strdup(ubus_argument_name);

		if (!policy->name)
			return false;
	}

	return true;
}

static uc_ubus_object_t *
uc_ubus_object_register(uc_vm_t *vm, uc_ubus_connection_t *c, const char *ubus_object_name,
                        uc_value_t *ubus_object_methods)
{
	struct ubus_context *ctx = &c->ctx;
	const struct blobmsg_policy *policy;
	uc_ubus_object_t *uuobj = NULL;
	int rv = UBUS_STATUS_UNKNOWN_ERROR;
	char *tnptr, *onptr;
	struct ubus_method *method;
	struct ubus_object *obj;
	size_t len, typelen, namelen, methodlen;
	uc_value_t *args, *res;

	namelen = strlen(ubus_object_name);
	typelen = strlen("ucode-ubus-") + namelen;
	methodlen = ucv_object_length(ubus_object_methods) * sizeof(struct ubus_method);
	len = sizeof(*uuobj) + methodlen + namelen + 1 + typelen + 1;

	res = ucv_resource_create_ex(vm, "ubus.object", (void **)&uuobj, __OBJ_RES_MAX, len);

	if (!uuobj)
		err_return(rv, "Out of memory");

	method = uuobj->methods;

	obj = &uuobj->obj;
	obj->methods = method;

	if (ubus_object_methods) {
		ucv_object_foreach(ubus_object_methods, ubus_method_name, ubus_method_definition) {
			args = ucv_object_get(ubus_method_definition, "args", NULL);

			if (!uc_ubus_object_method_register(&method[obj->n_methods++], ubus_method_name, args))
				goto out;
		}
	}

	onptr = (char *)&uuobj->methods[obj->n_methods];
	tnptr = onptr + namelen + 1;

	snprintf(tnptr, typelen, "ucode-ubus-%s", ubus_object_name);
	obj->name = memcpy(onptr, ubus_object_name, namelen);

	obj->type = (struct ubus_object_type *)&uuobj->type;
	obj->type->name = tnptr;
	obj->type->methods = obj->methods;
	obj->type->n_methods = obj->n_methods;

	rv = ubus_add_object(ctx, obj);

	if (rv != UBUS_STATUS_OK)
		goto out;

	uuobj->vm = vm;
	uuobj->ctx = ctx;
	uuobj->res = ucv_get(res);
	ucv_resource_persistent_set(res, true);
	ucv_resource_value_set(res, OBJ_RES_CONN, ucv_get(c->res));
	ucv_resource_value_set(res, OBJ_RES_METHODS, ucv_get(ubus_object_methods));

	return uuobj;

out:
	for (; obj->n_methods > 0; method++, obj->n_methods--) {
		for (policy = method->policy; method->n_policy > 0; policy++, method->n_policy--)
			free((char *)policy->name);

		free((char *)method->name);
		free((char *)method->policy);
	}

	ucv_put(res);

	err_return(rv, "Unable to add ubus object");
}

/**
 * Publish a ubus object on the bus.
 *
 * Registers a new object with the specified name and methods on the ubus
 * bus. The object can define method handlers and an optional subscribe
 * callback.
 *
 * Returns an object resource representing the published object.
 *
 * Returns `null` if the object could not be registered.
 *
 * @function module:ubus.connection#publish
 *
 * @param {string} object_name
 * The name to register the object under.
 *
 * @param {Object} [methods]
 * Optional object defining methods with `call` functions and optional
 * `args` type specifications.
 *
 * @param {function} [subscribe_callback]
 * Optional callback invoked when a subscriber connects to the object.
 *
 * @returns {?module:ubus.object}
 *
 * @example
 * const conn = connect();
 * const obj = conn.publish("my.service", {
 *     "hello": {
 *         call: (req, msg) => {
 *             req.reply({ message: "Hello, " + msg.name });
 *         },
 *         args: { name: "string" }
 *     }
 * });
 */
static uc_value_t *
uc_ubus_publish(uc_vm_t *vm, size_t nargs)
{
	uc_value_t *objname, *methods, *subscribecb;
	uc_ubus_connection_t *c;
	uc_ubus_object_t *uuobj;

	conn_get(vm, &c);

	args_get(vm, nargs,
	         "object name", UC_STRING, false, &objname,
	         "object methods", UC_OBJECT, true, &methods,
	         "subscribe callback", UC_CLOSURE, true, &subscribecb);

	if (!methods && !subscribecb)
		err_return(UBUS_STATUS_INVALID_ARGUMENT, "Either methods or subscribe callback required");

	if (methods && !uc_ubus_object_methods_validate(methods))
		return NULL;

	uuobj = uc_ubus_object_register(vm, c, ucv_string_get(objname), methods);

	if (!uuobj)
		return NULL;

	if (subscribecb) {
		uuobj->obj.subscribe_cb = uc_ubus_object_subscribe_cb;
		ucv_resource_value_set(uuobj->res, OBJ_RES_SUB_CB, ucv_get(subscribecb));
	}

	ok_return(uuobj->res);
}


/*
 * ubus events
 * --------------------------------------------------------------------------
 */

static int
uc_ubus_listener_remove_common(uc_ubus_listener_t *uul)
{
	int rv = ubus_unregister_event_handler(uul->ctx, &uul->ev);

	if (rv == UBUS_STATUS_OK)
		uc_ubus_put_res(&uul->res);

	return rv;
}

/**
 * Remove an event listener from the bus.
 *
 * Unregisters the event handler, stopping it from receiving further events.
 *
 * Returns `true` on success.
 *
 * Returns `null` if the listener could not be removed.
 *
 * @function module:ubus.listener#remove
 *
 * @returns {?boolean}
 *
 * @example
 * const listener = conn.listener("my.event.*", (type, data) => {
 *     printf("Event: %s, Data: %.J\n", type, data);
 * });
 * // … later …
 * listener.remove();
 */
static uc_value_t *
uc_ubus_listener_remove(uc_vm_t *vm, size_t nargs)
{
	uc_ubus_listener_t *uul = uc_fn_thisval("ubus.listener");
	int rv;

	rv = uc_ubus_listener_remove_common(uul);

	if (rv != UBUS_STATUS_OK)
		err_return(rv, "Failed to remove listener object");

	ok_return(ucv_boolean_new(true));
}

static void
uc_ubus_listener_cb(struct ubus_context *ctx, struct ubus_event_handler *ev,
                    const char *type, struct blob_attr *msg)
{
	uc_ubus_listener_t *uul = (uc_ubus_listener_t *)ev;
	uc_value_t *this, *func;
	uc_vm_t *vm = uul->vm;

	this = uul->res;
	func = ucv_resource_value_get(this, 0);

	uc_vm_stack_push(vm, ucv_get(this));
	uc_vm_stack_push(vm, ucv_get(func));
	uc_vm_stack_push(vm, ucv_string_new(type));
	uc_vm_stack_push(vm, blob_array_to_ucv(vm, blob_data(msg), blob_len(msg), true));

	if (uc_ubus_vm_call(vm, true, 2))
		ucv_put(uc_vm_stack_pop(vm));
}



/**
 * Register an event listener.
 *
 * Registers a callback to be invoked when events matching the specified
 * pattern are received. The listener receives the event type and data.
 *
 * Returns a listener resource that can be removed via
 * {@link module:ubus.listener#remove|remove()}.
 *
 * Returns `null` if the listener could not be registered.
 *
 * @function module:ubus.connection#listener
 *
 * @param {string} pattern
 * The event type pattern to match, supporting wildcards (e.g.,
 * `` `system.*` ``, `` `my.event.?` ``).
 *
 * @param {function} cb
 * Callback invoked when a matching event is received. Receives the event
 * type string and event data object as arguments.
 *
 * @returns {?module:ubus.listener}
 *
 * @example
 * const conn = connect();
 * const listener = conn.listener("system.*", (type, data) => {
 *     printf("Event %s: %.J\n", type, data);
 * });
 */
static uc_value_t *
uc_ubus_listener(uc_vm_t *vm, size_t nargs)
{
	uc_value_t *cb, *pattern;
	uc_ubus_connection_t *c;
	uc_ubus_listener_t *uul = NULL;
	uc_value_t *res;
	int rv;

	conn_get(vm, &c);

	args_get(vm, nargs,
	         "event type pattern", UC_STRING, false, &pattern,
	         "event callback", UC_CLOSURE, false, &cb);

	res = ucv_resource_create_ex(vm, "ubus.listener", (void **)&uul, 1, sizeof(*uul));

	if (!uul)
		err_return(UBUS_STATUS_UNKNOWN_ERROR, "Out of memory");

	uul->vm = vm;
	uul->ctx = &c->ctx;
	uul->res = res;
	uul->ev.cb = uc_ubus_listener_cb;

	rv = ubus_register_event_handler(&c->ctx, &uul->ev,
	                                 ucv_string_get(pattern));

	if (rv != UBUS_STATUS_OK) {
		ucv_put(res);
		err_return(rv, "Failed to register listener object");
	}

	ucv_resource_persistent_set(res, true);
	ucv_resource_value_set(res, 0, ucv_get(cb));

	ok_return(ucv_get(res));
}

/**
 * Send a ubus event.
 *
 * Broadcasts an event of the specified type with optional data to all
 * registered event listeners.
 *
 * Returns `true` on success.
 *
 * Returns `null` if the event could not be sent.
 *
 * @function module:ubus.connection#event
 *
 * @param {string} event_type
 * The type string identifying the event.
 *
 * @param {Object} [event_data]
 * Optional event data as an object with field names and values.
 *
 * @returns {?boolean}
 *
 * @example
 * const conn = connect();
 * conn.event("system.boot", { host: "router1", uptime: 3600 });
 */
static uc_value_t *
uc_ubus_event(uc_vm_t *vm, size_t nargs)
{
	uc_value_t *eventtype, *eventdata;
	uc_ubus_connection_t *c;
	int rv;

	conn_get(vm, &c);

	args_get(vm, nargs,
	         "event id", UC_STRING, false, &eventtype,
	         "event data", UC_OBJECT, true, &eventdata);

	blob_buf_init(&buf, 0);

	if (eventdata)
		ucv_object_to_blob(eventdata, &buf);

	rv = ubus_send_event(&c->ctx, ucv_string_get(eventtype), buf.head);

	if (rv != UBUS_STATUS_OK)
		err_return(rv, "Unable to send event");

	ok_return(ucv_boolean_new(true));
}


/*
 * ubus subscriptions
 * --------------------------------------------------------------------------
 */

static int
uc_ubus_subscriber_notify_cb(struct ubus_context *ctx, struct ubus_object *obj,
	                         struct ubus_request_data *req, const char *method,
	                         struct blob_attr *msg)
{
	struct ubus_subscriber *sub = container_of(obj, struct ubus_subscriber, obj);
	uc_ubus_subscriber_t *uusub = container_of(sub, uc_ubus_subscriber_t, sub);
	uc_value_t *this, *func, *reqproto;

	this = uusub->res;
	func = ucv_resource_value_get(this, SUB_RES_NOTIFY_CB);

	if (!ucv_is_callable(func))
		return UBUS_STATUS_METHOD_NOT_FOUND;

	reqproto = ucv_object_new(uusub->vm);

	ucv_object_add(reqproto, "type", ucv_string_new(method));

	ucv_object_add(reqproto, "data",
		blob_array_to_ucv(uusub->vm, blob_data(msg), blob_len(msg), true));

	ucv_object_add(reqproto, "info",
		uc_ubus_object_call_info(uusub->vm, ctx, req, obj, NULL));

	return uc_ubus_handle_reply_common(ctx, req, uusub->vm, this, func, reqproto);
}

static void
uc_ubus_subscriber_remove_cb(struct ubus_context *ctx,
                             struct ubus_subscriber *sub, uint32_t id)
{
	uc_ubus_subscriber_t *uusub = container_of(sub, uc_ubus_subscriber_t, sub);
	uc_value_t *this, *func;
	uc_vm_t *vm = uusub->vm;

	this = uusub->res;
	func = ucv_resource_value_get(this, SUB_RES_REMOVE_CB);

	if (!ucv_is_callable(func))
		return;

	uc_vm_stack_push(vm, ucv_get(this));
	uc_vm_stack_push(vm, ucv_get(func));
	uc_vm_stack_push(vm, ucv_uint64_new(id));

	if (uc_ubus_vm_call(vm, true, 1))
		ucv_put(uc_vm_stack_pop(vm));
}

static uc_value_t *
uc_ubus_subscriber_subunsub_common(uc_vm_t *vm, size_t nargs, bool subscribe)
{
	uc_ubus_subscriber_t *uusub = uc_fn_thisval("ubus.subscriber");
	uc_value_t *objname;
	uint32_t id;
	int rv;

	if (!uusub)
		err_return(UBUS_STATUS_INVALID_ARGUMENT, "Invalid subscriber context");

	args_get(vm, nargs,
	         "object name", UC_STRING, false, &objname);

	rv = ubus_lookup_id(uusub->ctx, ucv_string_get(objname), &id);

	if (rv != UBUS_STATUS_OK)
		err_return(rv, "Failed to resolve object name '%s'",
		           ucv_string_get(objname));

	if (subscribe)
		rv = ubus_subscribe(uusub->ctx, &uusub->sub, id);
	else
		rv = ubus_unsubscribe(uusub->ctx, &uusub->sub, id);

	if (rv != UBUS_STATUS_OK)
		err_return(rv, "Failed to %s object '%s'",
		           subscribe ? "subscribe" : "unsubscribe",
		           ucv_string_get(objname));

	ok_return(ucv_boolean_new(true));
}

/**
 * Subscribe to a ubus object.
 *
 * Registers interest in notifications from the specified object.
 *
 * Returns `true` on success.
 *
 * Returns `null` if the subscription failed.
 *
 * @function module:ubus.subscriber#subscribe
 *
 * @param {string} object_name
 * The name of the object to subscribe to.
 *
 * @returns {?boolean}
 *
 * @example
 * const conn = connect();
 * const sub = conn.subscriber("my.object", (event) => {
 *     printf("Notification: type=%s, data=%.J\n", event.type, event.data);
 * });
 * // … later, to re-subscribe …
 * sub.subscribe("my.object");
 */
static uc_value_t *
uc_ubus_subscriber_subscribe(uc_vm_t *vm, size_t nargs)
{
	return uc_ubus_subscriber_subunsub_common(vm, nargs, true);
}

/**
 * Unsubscribe from a ubus object.
 *
 * Stops receiving notifications from the specified object.
 *
 * Returns `true` on success.
 *
 * Returns `null` if the unsubscription failed.
 *
 * @function module:ubus.subscriber#unsubscribe
 *
 * @param {string} object_name
 * The name of the object to unsubscribe from.
 *
 * @returns {?boolean}
 *
 * @example
 * const sub = conn.subscriber("my.object", (event) => { … });
 * // … later, to unsubscribe temporarily …
 * sub.unsubscribe("my.object");
 */
static uc_value_t *
uc_ubus_subscriber_unsubscribe(uc_vm_t *vm, size_t nargs)
{
	return uc_ubus_subscriber_subunsub_common(vm, nargs, false);
}

static int
uc_ubus_subscriber_remove_common(uc_ubus_subscriber_t *uusub)
{
	int rv = ubus_unregister_subscriber(uusub->ctx, &uusub->sub);

	if (rv == UBUS_STATUS_OK)
		uc_ubus_put_res(&uusub->res);

	return rv;
}

#ifdef HAVE_UBUS_NEW_OBJ_CB
static bool
uc_ubus_subscriber_new_object_cb(struct ubus_context *ctx, struct ubus_subscriber *sub, const char *path)
{
	uc_ubus_subscriber_t *uusub = container_of(sub, uc_ubus_subscriber_t, sub);
	uc_value_t *patterns = ucv_resource_value_get(uusub->res, SUB_RES_PATTERNS);
	size_t len = ucv_array_length(patterns);

	for (size_t i = 0; i < len; i++) {
		uc_value_t *val = ucv_array_get(patterns, i);
		const char *pattern;

		if (ucv_type(val) != UC_STRING)
			continue;

		pattern = ucv_string_get(val);

		if (fnmatch(pattern, path, 0) == 0)
			return true;
	}

	return false;
}
#endif

/**
 * Remove a subscriber from the bus.
 *
 * Unregisters the subscriber, stopping it from receiving further
 * notifications.
 *
 * Returns `true` on success.
 *
 * Returns `null` if the subscriber could not be removed.
 *
 * @function module:ubus.subscriber#remove
 *
 * @returns {?boolean}
 *
 * @example
 * const sub = conn.subscriber("my.object", (event) => { … });
 * // … when done …
 * sub.remove();
 */
static uc_value_t *
uc_ubus_subscriber_remove(uc_vm_t *vm, size_t nargs)
{
	uc_ubus_subscriber_t *uusub = uc_fn_thisval("ubus.subscriber");
	int rv;

	if (!uusub)
		err_return(UBUS_STATUS_INVALID_ARGUMENT, "Invalid subscriber context");

	rv = uc_ubus_subscriber_remove_common(uusub);

	if (rv != UBUS_STATUS_OK)
		err_return(rv, "Failed to remove subscriber object");

	ok_return(ucv_boolean_new(true));
}

/**
 * Register a ubus subscriber.
 *
 * Creates a subscriber that can receive notifications from ubus objects.
 * The subscriber can define notify and remove callbacks, and optional
 * subscription patterns for automatic object discovery.
 *
 * Returns a subscriber resource representing the registered subscriber.
 *
 * Returns `null` if the subscriber could not be registered.
 *
 * @function module:ubus.connection#subscriber
 *
 * @param {function} notify_callback
 * Callback invoked when a notification is received from a subscribed
 * object.
 *
 * @param {function} remove_callback
 * Callback invoked when a subscribed object is removed from the bus.
 *
 * @param {string[]} [subscription_patterns]
 * Optional array of glob patterns for automatic object subscription.
 *
 * @returns {?module:ubus.subscriber}
 *
 * @example
 * const conn = connect();
 * const sub = conn.subscriber(
 *     (event) => {
 *         printf("Received: type=%s, data=%.J\n",
 *                event.type, event.data);
 *     },
 *     (objid) => {
 *         printf("Object removed: %d\n", objid);
 *     },
 *     ["network.interface.*"]  // Auto-subscribe to matching objects
 * );
 * // Subscribe to a specific object
 * sub.subscribe("some.object");
 */
static uc_value_t *
uc_ubus_subscriber(uc_vm_t *vm, size_t nargs)
{
	uc_value_t *notify_cb, *remove_cb, *subscriptions;
	uc_ubus_subscriber_t *uusub = NULL;
	uc_ubus_connection_t *c;
	uc_value_t *res;
	int rv;

	conn_get(vm, &c);

	args_get(vm, nargs,
	         "notify callback", UC_CLOSURE, true, &notify_cb,
	         "remove callback", UC_CLOSURE, true, &remove_cb,
	         "subscription patterns", UC_ARRAY, true, &subscriptions);

	if (!notify_cb && !remove_cb)
		err_return(UBUS_STATUS_INVALID_ARGUMENT, "Either notify or remove callback required");

	res = ucv_resource_create_ex(vm, "ubus.subscriber", (void **)&uusub, __SUB_RES_MAX, sizeof(*uusub));

	if (!uusub)
		err_return(UBUS_STATUS_UNKNOWN_ERROR, "Out of memory");

	uusub->vm = vm;
	uusub->ctx = &c->ctx;
	uusub->res = ucv_get(res);

	ucv_resource_value_set(res, SUB_RES_NOTIFY_CB, ucv_get(notify_cb));
	ucv_resource_value_set(res, SUB_RES_REMOVE_CB, ucv_get(remove_cb));
	ucv_resource_value_set(res, SUB_RES_PATTERNS, ucv_get(subscriptions));

#ifdef HAVE_UBUS_NEW_OBJ_CB
	if (subscriptions)
		uusub->sub.new_obj_cb = uc_ubus_subscriber_new_object_cb;
#endif

	rv = ubus_register_subscriber(&c->ctx, &uusub->sub);

	if (rv != UBUS_STATUS_OK) {
		ucv_put(uusub->res);
		ucv_put(res);
		err_return(rv, "Failed to register subscriber object");
	}

	if (notify_cb)
		uusub->sub.cb = uc_ubus_subscriber_notify_cb;

	if (remove_cb)
		uusub->sub.remove_cb = uc_ubus_subscriber_remove_cb;

	ucv_resource_persistent_set(res, true);

	ok_return(res);
}


/*
 * connection methods
 * --------------------------------------------------------------------------
 */

/**
 * Send a reply to an asynchronous method call.
 *
 * Sends the specified data as a reply to the incoming method call. This
 * function should be called within a published object's method handler.
 *
 * Returns `true` on success.
 *
 * Returns `null` if the reply could not be sent.
 *
 * @function module:ubus.request#reply
 *
 * @param {*} data
 * The data to send as reply. Can be any JSON-serializable value.
 *
 * @returns {boolean}
 *
 * @example
 * // Synchronous reply
 * const obj = publish("my.service", {
 *     hello: (req, msg) => {
 *         req.reply({ message: "Hello " + msg.name });
 *     }
 * });
 *
 * @example
 * // Asynchronous reply after defer completes
 * const obj = publish("my.service", {
 *     some_method: (req) => {
 *         ubus_conn.defer("other.object", "other_method", {},
 *             (rc, data) => {
 *                 req.reply({ result: data });
 *             });
 *     }
 * });
 */
static uc_value_t *
uc_ubus_remove(uc_vm_t *vm, size_t nargs)
{
	uc_ubus_subscriber_t **uusub;
	uc_ubus_connection_t *c;
	uc_ubus_object_t *uuobj;
	uc_ubus_listener_t **uul;
	int rv;

	conn_get(vm, &c);

	uusub = (uc_ubus_subscriber_t **)ucv_resource_dataptr(uc_fn_arg(0), "ubus.subscriber");
	uuobj = (uc_ubus_object_t *)ucv_resource_data(uc_fn_arg(0), "ubus.object");
	uul = (uc_ubus_listener_t **)ucv_resource_dataptr(uc_fn_arg(0), "ubus.listener");

	if (uusub && *uusub) {
		if ((*uusub)->ctx != &c->ctx)
			err_return(UBUS_STATUS_INVALID_ARGUMENT,
			           "Subscriber belongs to different connection");

		rv = uc_ubus_subscriber_remove_common(*uusub);

		if (rv != UBUS_STATUS_OK)
			err_return(rv, "Unable to remove subscriber");
	}
	else if (uuobj) {
		if (uuobj->ctx != &c->ctx)
			err_return(UBUS_STATUS_INVALID_ARGUMENT,
			           "Object belongs to different connection");

		rv = uc_ubus_object_remove_common(uuobj);

		if (rv != UBUS_STATUS_OK)
			err_return(rv, "Unable to remove object");
	}
	else if (uul && *uul) {
		if ((*uul)->ctx != &c->ctx)
			err_return(UBUS_STATUS_INVALID_ARGUMENT,
			           "Listener belongs to different connection");

		rv = uc_ubus_listener_remove_common(*uul);

		if (rv != UBUS_STATUS_OK)
			err_return(rv, "Unable to remove listener");
	}
	else {
		err_return(UBUS_STATUS_INVALID_ARGUMENT, "Unhandled resource type");
	}

	ok_return(ucv_boolean_new(true));
}


/**
 * Disconnect from the ubus bus.
 *
 * Closes the connection to the ubus bus and releases associated resources.
 * All pending requests are aborted.
 *
 * Returns `true` on success.
 *
 * @function module:ubus.connection#disconnect
 *
 * @returns {boolean}
 *
 * @example
 * const conn = connect();
 * // … do work …
 * conn.disconnect();
 */
static uc_value_t *
uc_ubus_disconnect(uc_vm_t *vm, size_t nargs)
{
	uc_ubus_connection_t *c;

	conn_get(vm, &c);

#ifdef HAVE_UBUS_FLUSH_REQUESTS
	ubus_flush_requests(&c->ctx);
#endif
	if (c->fd_handle) {
		uloop_fd_delete(&c->ctx.sock);
		c->ctx.sock.fd = -1;
	}
	ubus_shutdown(&c->ctx);
	c->ctx.sock.fd = -1;
	uc_ubus_put_res(&c->res);

	ok_return(ucv_boolean_new(true));
}

/**
 * Check if a deferred request has completed.
 *
 * Returns `true` if the deferred request has finished.
 *
 * Returns `false` if the request is still pending.
 *
 * @function module:ubus.deferred#completed
 *
 * @returns {boolean}
 */
static uc_value_t *
uc_ubus_defer_completed(uc_vm_t *vm, size_t nargs)
{
	uc_ubus_deferred_t *d = uc_fn_thisval("ubus.deferred");

	if (!d)
		err_return(UBUS_STATUS_INVALID_ARGUMENT, "Invalid deferred context");

	ok_return(ucv_boolean_new(d->complete));
}

/**
 * Wait synchronously for a deferred request to complete.
 *
 * Blocks until the deferred request completes or times out.
 *
 * Returns `true` if the request completed.
 *
 * Returns `false` if the request was already completed.
 *
 * @function module:ubus.deferred#await
 *
 * @returns {boolean}
 */
static uc_value_t *
uc_ubus_defer_await(uc_vm_t *vm, size_t nargs)
{
	uc_ubus_deferred_t *d = uc_fn_thisval("ubus.deferred");
	int64_t remaining;

	if (!d)
		err_return(UBUS_STATUS_INVALID_ARGUMENT, "Invalid deferred context");

	if (d->complete)
		ok_return(ucv_boolean_new(false));

#ifdef HAVE_ULOOP_TIMEOUT_REMAINING64
	remaining = uloop_timeout_remaining64(&d->timeout);
#else
	remaining = uloop_timeout_remaining(&d->timeout);
#endif

	ubus_complete_request(d->ctx, &d->request, remaining);

	ok_return(ucv_boolean_new(true));
}

/**
 * Abort a pending deferred request.
 *
 * Cancels an asynchronous request that has not yet completed.
 *
 * Returns `true` if the request was aborted.
 *
 * Returns `false` if the request was already completed.
 *
 * @function module:ubus.deferred#abort
 *
 * @returns {boolean}
 */
static uc_value_t *
uc_ubus_defer_abort(uc_vm_t *vm, size_t nargs)
{
	uc_ubus_deferred_t *d = uc_fn_thisval("ubus.deferred");

	if (!d)
		err_return(UBUS_STATUS_INVALID_ARGUMENT, "Invalid deferred context");

	if (d->complete)
		ok_return(ucv_boolean_new(false));

	ubus_abort_request(d->ctx, &d->request);
	uloop_timeout_cancel(&d->timeout);

	uc_ubus_put_res(&d->res);
	d->complete = true;

	ok_return(ucv_boolean_new(true));
}

/*
 * channel related methods
 * --------------------------------------------------------------------------
 */

#ifdef HAVE_UBUS_CHANNEL_SUPPORT
static int
uc_ubus_channel_req_cb(struct ubus_context *ctx, struct ubus_object *obj,
                       struct ubus_request_data *req, const char *method,
                       struct blob_attr *msg)
{
	uc_ubus_connection_t *c = container_of(ctx, uc_ubus_connection_t, ctx);
	uc_value_t *func, *args, *reqproto;

	func = ucv_resource_value_get(c->res, CONN_RES_CB);

	if (!ucv_is_callable(func))
		return UBUS_STATUS_METHOD_NOT_FOUND;

	args = blob_array_to_ucv(c->vm, blob_data(msg), blob_len(msg), true);
	reqproto = ucv_object_new(c->vm);
	ucv_object_add(reqproto, "args", args);

	if (method)
		ucv_object_add(reqproto, "type", ucv_string_new(method));

	return uc_ubus_handle_reply_common(ctx, req, c->vm, c->res, func, reqproto);
}

static void
uc_ubus_channel_disconnect_cb(struct ubus_context *ctx)
{
	uc_ubus_connection_t *c = container_of(ctx, uc_ubus_connection_t, ctx);
	uc_value_t *res, *func;

	/* pin ref across user callback to guard against re-entrant disconnect() */
	res = ucv_get(c->res);

	func = ucv_resource_value_get(res, CONN_RES_DISCONNECT_CB);

	if (ucv_is_callable(func)) {
		uc_vm_stack_push(c->vm, ucv_get(res));
		uc_vm_stack_push(c->vm, ucv_get(func));

		if (uc_ubus_vm_call(c->vm, true, 0))
			ucv_put(uc_vm_stack_pop(c->vm));
	}

	blob_buf_free(&c->buf);

	if (c->ctx.sock.fd >= 0) {
		if (c->fd_handle) {
			uloop_fd_delete(&c->ctx.sock);
			c->ctx.sock.fd = -1;
		}
		ubus_shutdown(&c->ctx);
		c->ctx.sock.fd = -1;
	}

	uc_ubus_put_res(&c->res);
	ucv_put(res);
}

static uc_value_t *
uc_ubus_channel_add(uc_ubus_connection_t *c, uc_value_t *cb,
                    uc_value_t *disconnect_cb, uc_value_t *fd)
{
	ucv_resource_persistent_set(c->res, true);
	ucv_resource_value_set(c->res, CONN_RES_FD, ucv_get(fd));
	ucv_resource_value_set(c->res, CONN_RES_CB, ucv_get(cb));
	ucv_resource_value_set(c->res, CONN_RES_DISCONNECT_CB, ucv_get(disconnect_cb));
	c->ctx.connection_lost = uc_ubus_channel_disconnect_cb;
	ubus_add_uloop(&c->ctx);

	ok_return(ucv_get(c->res));
}

#endif

/**
 * Create a new ubus channel from a method call context.
 *
 * Creates a bidirectional channel communication path in response to an
 * incoming method call. The callback will be invoked for incoming messages
 * on the channel.
 *
 * Returns a channel connection resource.
 *
 * Returns `null` if the channel could not be created.
 *
 * @function module:ubus.request#new_channel
 *
 * @param {function} cb
 * Callback invoked for incoming messages on the channel.
 *
 * @param {function} [disconnect_cb]
 * Optional callback invoked when the channel is disconnected.
 *
 * @param {number} [timeout=30]
 * The timeout in seconds for subsequent operations.
 *
 * @returns {?module:ubus.channel}
 */
static uc_value_t *
uc_ubus_request_new_channel(uc_vm_t *vm, size_t nargs)
{
#ifdef HAVE_UBUS_CHANNEL_SUPPORT
	uc_ubus_request_t *callctx = uc_fn_thisval("ubus.request");
	uc_value_t *cb, *disconnect_cb, *timeout;
	uc_ubus_connection_t *c;
	int fd;

	if (!callctx)
		err_return(UBUS_STATUS_INVALID_ARGUMENT, "Invalid call context");

	args_get(vm, nargs,
	         "cb", UC_CLOSURE, true, &cb,
	         "disconnect_cb", UC_CLOSURE, true, &disconnect_cb,
	         "timeout", UC_INTEGER, true, &timeout);

	c = uc_ubus_conn_alloc(vm, timeout, "ubus.channel");

	if (!c)
		return NULL;

	if (ubus_channel_create(&c->ctx, &fd, cb ? uc_ubus_channel_req_cb : NULL)) {
		ucv_put(c->res);
		err_return(UBUS_STATUS_UNKNOWN_ERROR, "Unable to create ubus channel");
	}

	ubus_request_set_fd(callctx->ctx, &callctx->req, fd);

	return uc_ubus_channel_add(c, cb, disconnect_cb, NULL);
#else
	err_return(UBUS_STATUS_NOT_SUPPORTED, "No ubus channel support");
#endif
}


/**
 * Connect to a ubus channel from a file descriptor.
 *
 * Creates a channel connection from an existing file descriptor, typically
 * received from a method call. The callback will be invoked for incoming
 * messages on the channel.
 *
 * Returns a channel connection resource.
 *
 * Returns `null` if the channel could not be created.
 *
 * @function module:ubus#open_channel
 *
 * @param {number|module:fs.file|module:socket.socket} fd
 * The file descriptor or resource of the channel to connect to. When a plain
 * integer fd is passed, the ubus channel takes ownership and closes it on
 * disconnect. When a resource object with a `fileno()` method is passed, the
 * resource retains ownership of the fd; the channel merely detaches from it
 * on disconnect without closing.
 *
 * @param {function} cb
 * Callback invoked for incoming messages on the channel.
 *
 * @param {function} [disconnect_cb]
 * Optional callback invoked when the channel is disconnected.
 *
 * @param {number} [timeout=30]
 * The timeout in seconds for subsequent operations.
 *
 * @returns {?module:ubus.channel}
 */
static uc_value_t *
uc_ubus_channel_connect(uc_vm_t *vm, size_t nargs)
{
#ifdef HAVE_UBUS_CHANNEL_SUPPORT
	uc_value_t *fd, *cb, *disconnect_cb, *timeout;
	bool handle = false;
	uc_ubus_connection_t *c;
	int fd_val;

	args_get(vm, nargs,
	         "fd", UC_NULL, false, &fd,
	         "cb", UC_CLOSURE, true, &cb,
	         "disconnect_cb", UC_CLOSURE, true, &disconnect_cb,
	         "timeout", UC_INTEGER, true, &timeout);

	fd_val = get_fd(vm, fd, &handle);

	if (fd_val < 0)
		err_return(UBUS_STATUS_INVALID_ARGUMENT, "Invalid file descriptor argument");

	c = uc_ubus_conn_alloc(vm, timeout, "ubus.channel");

	if (!c)
		return NULL;

	c->fd_handle = handle;

	if (ubus_channel_connect(&c->ctx, fd_val, cb ? uc_ubus_channel_req_cb : NULL)) {
		ucv_put(c->res);
		err_return(UBUS_STATUS_UNKNOWN_ERROR, "Unable to create ubus channel");
	}

	return uc_ubus_channel_add(c, cb, disconnect_cb, fd);
#else
	err_return(UBUS_STATUS_NOT_SUPPORTED, "No ubus channel support");
#endif
}


/**
 * Get or set the ubus exception handler.
 *
 * When called without arguments, returns the currently registered exception
 * handler function. When called with a function argument, registers it as
 * the exception handler for ubus operations.
 *
 * Returns the current exception handler when called without arguments.
 *
 * Returns `true` when a new handler was set.
 *
 * @function module:ubus#guard
 *
 * @param {function} [handler]
 * The exception handler function to register.
 *
 * @returns {function|boolean}
 */
static uc_value_t *
uc_ubus_guard(uc_vm_t *vm, size_t nargs)
{
	uc_value_t *arg = uc_fn_arg(0);

	if (!nargs)
		return ucv_get(uc_vm_registry_get(vm, "ubus.ex_handler"));

	if (arg && !ucv_is_callable(arg))
		return NULL;

	uc_vm_registry_set(vm, "ubus.ex_handler", ucv_get(arg));

	return ucv_boolean_new(true);
}


static const uc_function_list_t global_fns[] = {
	{ "error",			uc_ubus_error },
	{ "connect",		uc_ubus_connect },
	{ "open_channel",	uc_ubus_channel_connect },
	{ "guard",			uc_ubus_guard },
};

static const uc_function_list_t conn_fns[] = {
	{ "list",			uc_ubus_list },
	{ "call",			uc_ubus_call },
	{ "defer",			uc_ubus_defer },
	{ "publish",		uc_ubus_publish },
	{ "remove",			uc_ubus_remove },
	{ "listener",		uc_ubus_listener },
	{ "subscriber",		uc_ubus_subscriber },
	{ "event",			uc_ubus_event },
	{ "error",			uc_ubus_error },
	{ "disconnect",		uc_ubus_disconnect },
};

static const uc_function_list_t chan_fns[] = {
	{ "request",		uc_ubus_chan_request },
	{ "defer",			uc_ubus_chan_defer },
	{ "error",			uc_ubus_error },
	{ "disconnect",		uc_ubus_disconnect },
};

static const uc_function_list_t defer_fns[] = {
	{ "await",			uc_ubus_defer_await },
	{ "completed",		uc_ubus_defer_completed },
	{ "abort",			uc_ubus_defer_abort },
};

static const uc_function_list_t object_fns[] = {
	{ "subscribed",		uc_ubus_object_subscribed },
	{ "notify",			uc_ubus_object_notify },
	{ "remove",			uc_ubus_object_remove },
};

static const uc_function_list_t request_fns[] = {
	{ "reply",			uc_ubus_request_reply },
	{ "error",			uc_ubus_request_error },
	{ "defer",			uc_ubus_request_defer },
	{ "get_fd",			uc_ubus_request_get_fd },
	{ "set_fd",			uc_ubus_request_set_fd },
	{ "new_channel",	uc_ubus_request_new_channel },
};

static const uc_function_list_t notify_fns[] = {
	{ "completed",		uc_ubus_notify_completed },
	{ "abort",			uc_ubus_notify_abort },
};

static const uc_function_list_t listener_fns[] = {
	{ "remove",			uc_ubus_listener_remove },
};

static const uc_function_list_t subscriber_fns[] = {
	{ "subscribe",		uc_ubus_subscriber_subscribe },
	{ "unsubscribe",	uc_ubus_subscriber_unsubscribe },
	{ "remove",			uc_ubus_subscriber_remove },
};

static void free_connection(void *ud) {
	uc_ubus_connection_t *conn = ud;

	blob_buf_free(&conn->buf);

	if (conn->ctx.sock.fd >= 0) {
		if (conn->fd_handle) {
			uloop_fd_delete(&conn->ctx.sock);
			conn->ctx.sock.fd = -1;
		}
		ubus_shutdown(&conn->ctx);
	}
}

static void free_deferred(void *ud) {
	uc_ubus_deferred_t *defer = ud;

	uloop_timeout_cancel(&defer->timeout);
}

static void free_object(void *ud) {
	uc_ubus_object_t *uuobj = ud;
	struct ubus_object *obj = &uuobj->obj;
	int i, j;

	for (i = 0; i < obj->n_methods; i++) {
		for (j = 0; j < obj->methods[i].n_policy; j++)
			free((char *)obj->methods[i].policy[j].name);

		free((char *)obj->methods[i].name);
		free((char *)obj->methods[i].policy);
	}
}

static void free_request(void *ud) {
	uc_ubus_request_t *callctx = ud;

	uc_ubus_request_finish(callctx, UBUS_STATUS_TIMEOUT);
}

void uc_module_init(uc_vm_t *vm, uc_value_t *scope)
{
	uc_function_list_register(scope, global_fns);
	uc_function_list_register(scope, conn_fns);

	/**
	 * @typedef
	 * @name Ubus status codes
	 * @property {number} STATUS_OK - Operation successful
	 * @property {number} STATUS_INVALID_COMMAND - Invalid command
	 * @property {number} STATUS_INVALID_ARGUMENT - Invalid argument
	 * @property {number} STATUS_METHOD_NOT_FOUND - Method not found
	 * @property {number} STATUS_NOT_FOUND - Object not found
	 * @property {number} STATUS_NO_DATA - No data available
	 * @property {number} STATUS_PERMISSION_DENIED - Permission denied
	 * @property {number} STATUS_TIMEOUT - Operation timed out
	 * @property {number} STATUS_NOT_SUPPORTED - Operation not supported
	 * @property {number} STATUS_UNKNOWN_ERROR - Unknown error
	 * @property {number} STATUS_CONNECTION_FAILED - Connection failed
	 * @property {number} STATUS_NO_MEMORY - Out of memory (new)
	 * @property {number} STATUS_PARSE_ERROR - Parse error (new)
	 * @property {number} STATUS_SYSTEM_ERROR - System error (new)
	 * @property {number} STATUS_CONTINUE - Virtual code for continued replies
	 */

#define ADD_CONST(x) ucv_object_add(scope, #x, ucv_int64_new(UBUS_##x))
	ADD_CONST(STATUS_OK);
	ADD_CONST(STATUS_INVALID_COMMAND);
	ADD_CONST(STATUS_INVALID_ARGUMENT);
	ADD_CONST(STATUS_METHOD_NOT_FOUND);
	ADD_CONST(STATUS_NOT_FOUND);
	ADD_CONST(STATUS_NO_DATA);
	ADD_CONST(STATUS_PERMISSION_DENIED);
	ADD_CONST(STATUS_TIMEOUT);
	ADD_CONST(STATUS_NOT_SUPPORTED);
	ADD_CONST(STATUS_UNKNOWN_ERROR);
	ADD_CONST(STATUS_CONNECTION_FAILED);

#ifdef HAVE_NEW_UBUS_STATUS_CODES
	ADD_CONST(STATUS_NO_MEMORY);
	ADD_CONST(STATUS_PARSE_ERROR);
	ADD_CONST(STATUS_SYSTEM_ERROR);
#endif

	/* virtual status code for reply */
#define UBUS_STATUS_CONTINUE -1
	ADD_CONST(STATUS_CONTINUE);

	/**
	 * @typedef
	 * @name Ubus system object IDs
	 * @property {number} SYSTEM_OBJECT_ACL - System object ACL identifier,
 * used to query ACL data via
 * {@link module:ubus#call|call()} with an integer object ID
	 */
	ADD_CONST(SYSTEM_OBJECT_ACL);

	uc_type_declare(vm, "ubus.connection", conn_fns, free_connection);
	uc_type_declare(vm, "ubus.channel", chan_fns, free_connection);
	uc_type_declare(vm, "ubus.deferred", defer_fns, free_deferred);
	uc_type_declare(vm, "ubus.object", object_fns, free_object);
	uc_type_declare(vm, "ubus.notify", notify_fns, NULL);
	uc_type_declare(vm, "ubus.request", request_fns, free_request);
	uc_type_declare(vm, "ubus.listener", listener_fns, NULL);
	uc_type_declare(vm, "ubus.subscriber", subscriber_fns, NULL);
}
