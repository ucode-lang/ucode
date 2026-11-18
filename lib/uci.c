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
 * # OpenWrt UCI configuration
 *
 * The `uci` module provides access to the native OpenWrt
 * {@link https://github.com/openwrt/uci libuci} API for reading and
 * manipulating UCI configuration files.
 *
 * Functions can be individually imported and directly accessed using the
 * {@link https://developer.mozilla.org/en-US/docs/Web/JavaScript/Reference/Statements/import#named_import named import}
 * syntax:
 *
 *   ```
 *   import { cursor } from 'uci';
 *
 *   let ctx = cursor();
 *   let hostname = ctx.get_first('system', 'system', 'hostname');
 *   ```
 *
 * Alternatively, the module namespace can be imported
 * using a wildcard import statement:
 *
 *   ```
 *   import * as uci from 'uci';
 *
 *   let ctx = uci.cursor();
 *   let hostname = ctx.get_first('system', 'system', 'hostname');
 *   ```
 *
 * Additionally, the uci module namespace may also be imported by invoking
 * the `ucode` interpreter with the `-luci` switch.
 *
 * @module uci
 */

#include <uci.h>

#include "ucode/module.h"

#define ok_return(expr) do { \
	uc_vm_registry_delete(vm, "uci.error"); \
	return (expr); \
} while(0)

#define err_return(err) do { \
	uc_vm_registry_set(vm, "uci.error", ucv_int64_new(err)); \
	return NULL; \
} while(0)

enum pkg_cmd {
	CMD_SAVE,
	CMD_COMMIT,
	CMD_REVERT
};

/**
 * Query error information.
 *
 * Returns a string containing a description of the last occurred error or
 * `null` if there is no error information.
 *
 * @function module:uci#error
 *
 * @returns {?string}
 *
 * @example
 * // Trigger error
 * const ctx = cursor();
 * ctx.set("not_existing_config", "test", "1");
 *
 * // Print error (should yield "Entry not found")
 * print(ctx.error(), "\n");
 */
static uc_value_t *
uc_uci_error(uc_vm_t *vm, size_t nargs)
{
	int last_error = ucv_int64_get(uc_vm_registry_get(vm, "uci.error"));
	char buf[sizeof("Unknown error: -9223372036854775808")];
	uc_value_t *errmsg;

	const char *errstr[] = {
		[UCI_ERR_MEM] =       "Out of memory",
		[UCI_ERR_INVAL] =     "Invalid argument",
		[UCI_ERR_NOTFOUND] =  "Entry not found",
		[UCI_ERR_IO] =        "I/O error",
		[UCI_ERR_PARSE] =     "Parse error",
		[UCI_ERR_DUPLICATE] = "Duplicate entry",
		[UCI_ERR_UNKNOWN] =   "Unknown error",
	};

	if (last_error == 0)
		return NULL;

	if (last_error >= 0 && (unsigned)last_error < ARRAY_SIZE(errstr)) {
		errmsg = ucv_string_new(errstr[last_error]);
	}
	else {
		snprintf(buf, sizeof(buf), "Unknown error: %d", last_error);
		errmsg = ucv_string_new(buf);
	}

	uc_vm_registry_delete(vm, "uci.error");

	return errmsg;
}


/**
 * @typedef {Object} module:uci.cursor.ParserFlags
 * @property {boolean} strict
 * Strict parsing mode (enabled by default). Aborts parsing when encountering
 * a parser error.
 *
 * @property {boolean} print_errors
 * Print parser errors to stderr.
 */

/**
 * Instantiate uci cursor.
 *
 * A uci cursor is a context for interacting with uci configuration files. It's
 * purpose is to cache and hold changes made to loaded configuration states
 * until those changes are written out to disk or discared.
 *
 * Unsaved and uncommitted changes in a cursor instance are private and not
 * visible to other cursor instances instantiated by the same program or other
 * processes on the system.
 *
 * Returns the instantiated cursor on success.
 *
 * Returns `null` on error, e.g. if an invalid path argument was provided.
 *
 * @function module:uci#cursor
 *
 * @param {string} [config_dir=/etc/config]
 * The directory to search for configuration files. It defaults to the well
 * known uci configuration directory `/etc/config` but may be set to a different
 * path for special purpose applications.
 *
 * @param {string} [delta_dir=/tmp/.uci]
 * The directory to save delta records in. It defaults to the well known
 * `/tmp/.uci` path which is used as default by the uci command line tool.
 *
 * By changing this path to a different location, it is possible to isolate
 * uncommitted application changes from the uci cli or other processes on the
 * system.
 *
 * @param {string} [config2_dir=/var/run/uci]
 * The directory to keep override config files in. Files are in the same format
 * as in config_dir, but can individually override ones from that directory.
 * It defaults to the uci configuration directory `/var/run/uci` but may be
 * set to a different path for special purpose applications, or even disabled
 * by setting this parameter to an empty string.
 *
 * @param {module:uci.cursor.ParserFlags}
 * Parser flags to change.
 *
 * @returns {?module:uci.cursor}
 */
static uc_value_t *
uc_uci_cursor(uc_vm_t *vm, size_t nargs)
{
	uc_value_t *cdir = uc_fn_arg(0);
	uc_value_t *sdir = uc_fn_arg(1);
	uc_value_t *c2dir = uc_fn_arg(2);
	uc_value_t *flags = uc_fn_arg(3);
	struct uci_context *c;
	int rv;

	if ((cdir && ucv_type(cdir) != UC_STRING) ||
	    (sdir && ucv_type(sdir) != UC_STRING) ||
	    (c2dir && ucv_type(c2dir) != UC_STRING) ||
	    (flags && ucv_type(flags) != UC_OBJECT))
		err_return(UCI_ERR_INVAL);

	c = uci_alloc_context();

	if (!c)
		err_return(UCI_ERR_MEM);

	if (cdir) {
		rv = uci_set_confdir(c, ucv_string_get(cdir));

		if (rv)
			goto error;
	}

	if (sdir) {
		rv = uci_set_savedir(c, ucv_string_get(sdir));

		if (rv)
			goto error;
	}

#ifdef HAVE_UCI_CONF2DIR
	if (c2dir) {
		rv = uci_set_conf2dir(c, ucv_string_get(c2dir));

		if (rv)
			goto error;
	}
#endif

	if (flags) {
		unsigned int i, set = 0, clear = 0;
		static const struct {
			const char *name;
			unsigned int mask;
		} flag_spec[] = {
			{ "strict", UCI_FLAG_STRICT },
			{ "print_errors", UCI_FLAG_PERROR },
		};

		ucv_object_foreach(flags, key, value) {
			for (i = 0; i < ARRAY_SIZE(flag_spec); i++)
				if (!strcmp(flag_spec[i].name, key))
					break;
			if (i == ARRAY_SIZE(flag_spec)) {
				rv = UCI_ERR_INVAL;
				goto error;
			}

			if (ucv_is_truish(value))
				set |= flag_spec[i].mask;
			else
				clear |= flag_spec[i].mask;
		}

		c->flags = (c->flags & ~clear) | set;
	}

	ok_return(ucv_resource_create(vm, "uci.cursor", c));

error:
	uci_free_context(c);
	err_return(rv);
}


/**
 * Represents a context for interacting with uci configuration files.
 *
 * Operations on uci configurations are performed through a uci cursor object
 * which operates on in-memory representations of loaded configuration files.
 *
 * Any changes made to configuration values are local to the cursor object and
 * held in memory only until they're written out to the filesystem using the
 * {@link module:uci.cursor#save|save()} and
 * {@link module:uci.cursor#commit|commit()} methods.
 *
 * Changes performed in one cursor instance are not reflected in another, unless
 * the first instance writes those changes to the filesystem and the other
 * instance explicitly (re)loads the affected configuration files.
 *
 * @class module:uci.cursor
 * @hideconstructor
 *
 * @borrows module:uci#error as module.uci.cursor#error
 *
 * @see {@link module:uci#cursor|cursor()}
 *
 * @example
 *
 * const ctx = cursor(…);
 *
 * // Enumerate configuration files
 * ctx.configs();
 *
 * // Load configuration files
 * ctx.load(…);
 * ctx.unload(…);
 *
 * // Query values
 * ctx.get(…);
 * ctx.get_all(…);
 * ctx.get_first(…);
 * ctx.foreach(…);
 *
 * // Modify values
 * ctx.add(…);
 * ctx.set(…);
 * ctx.rename(…);
 * ctx.reorder(…);
 * ctx.delete(…);
 *
 * // Stage, revert, save changes
 * ctx.changes(…);
 * ctx.save(…);
 * ctx.revert(…);
 * ctx.commit(…);
 */

/**
 * A uci change record is a plain array containing the change operation name as
 * first element, the affected section ID as second argument and an optional
 * third and fourth argument whose meanings depend on the operation.
 *
 * @typedef {string[]} ChangeRecord
 * @memberof module:uci.cursor
 *
 * @property {string} 0
 * The operation name - may be one of `add`, `set`, `remove`, `order`,
 * `list-add`, `list-del` or `rename`.
 *
 * @property {string} 1
 * The section ID targeted by the operation.
 *
 * @property {string} 2
 * The meaning of the third element depends on the operation.
 * - For `add` it is type of the section that has been added
 * - For `set` it either is the option name if a fourth element exists, or the
 *   type of a named section which has been added when the change entry only
 *   contains three elements.
 * - For `remove` it contains the name of the option that has been removed.
 * - For `order` it specifies the new sort index of the section.
 * - For `list-add` it contains the name of the list option a new value has been
 *   added to.
 * - For `list-del` it contains the name of the list option a value has been
 *   removed from.
 * - For `rename` it contains the name of the option that has been renamed if a
 *   fourth element exists, else it contains the new name a section has been
 *   renamed to if the change entry only contains three elements.
 *
 * @property {string} 4
 * The meaning of the fourth element depends on the operation.
 * - For `set` it is the value an option has been set to.
 * - For `list-add` it is the new value that has been added to a list option.
 * - For `rename` it is the new name of an option that has been renamed.
 */

/**
 * A section object represents the options and their corresponding values
 * enclosed within a configuration section, as well as some additional meta data
 * such as sort indexes and internal ID.
 *
 * Any internal metadata fields are prefixed with a dot which isn't an allowed
 * character for normal option names.
 *
 * @typedef {Object<string, boolean|number|string|string[]>} SectionObject
 * @memberof module:uci.cursor
 *
 * @property {boolean} .anonymous
 * The `.anonymous` property specifies whether the configuration is
 * anonymous (`true`) or named (`false`).
 *
 * @property {number} .index
 * The `.index` property specifies the sort order of the section.
 *
 * @property {string} .name
 * The `.name` property holds the name of the section object. It may be either
 * an anonymous ID in the form `cfgXXXXXX` with `X` being a hexadecimal digit or
 * a string holding the name of the section.
 *
 * @property {string} .type
 * The `.type` property contains the type of the corresponding uci
 * section.
 *
 * @property {string|string[]} *
 * A section object may contain an arbitrary number of further properties
 * representing the uci option enclosed in the section.
 *
 * All option property names will be in the form `[A-Za-z0-9_]+` and either
 * contain a string value or an array of strings, in case the underlying option
 * is an UCI list.
 */

/**
 * The sections callback is invoked for each section found within the given
 * configuration and receives the section object and its associated name as
 * arguments.
 *
 * @callback module:uci.cursor.SectionCallback
 *
 * @param {module:uci.cursor.SectionObject} section
 * The section object.
 */

/**
 * Explicitly reload configuration file.
 *
 * Usually, any attempt to query or modify a value within a given configuration
 * will implicitly load the underlying file into memory. By invoking `load()`
 * explicitly, a potentially loaded stale configuration is discarded and
 * reloaded from the file system, ensuring that the latest state is reflected in
 * the cursor.
 *
 * Returns `true` if the configuration was successfully loaded.
 *
 * Returns `null` on error, e.g. if the requested configuration does not exist.
 *
 * @function module:uci.cursor#load
 *
 * @param {string} config
 * The name of the configuration file to load, e.g. `"system"` to load
 * `/etc/config/system` into the cursor.
 *
 * @returns {?boolean}
 */
static uc_value_t *
uc_uci_load(uc_vm_t *vm, size_t nargs)
{
	struct uci_context **c = uc_fn_this("uci.cursor");
	uc_value_t *conf = uc_fn_arg(0);
	struct uci_element *e;
	char *s;

	if (!c || !*c)
		err_return(UCI_ERR_INVAL);

	if (ucv_type(conf) != UC_STRING)
		err_return(UCI_ERR_INVAL);

	s = ucv_string_get(conf);

	uci_foreach_element(&(*c)->root, e) {
		if (!strcmp(e->name, s)) {
			uci_unload(*c, uci_to_package(e));
			break;
		}
	}

	if (uci_load(*c, s, NULL))
		err_return((*c)->err);

	ok_return(ucv_boolean_new(true));
}

/**
 * Explicitly unload configuration file.
 *
 * The `unload()` function forcibly discards a loaded configuration state from
 * the cursor so that the next attempt to read or modify that configuration
 * will load it anew from the file system.
 *
 * Returns `true` if the configuration was successfully unloaded.
 *
 * Returns `false` if the configuration was not loaded to begin with.
 *
 * Returns `null` on error, e.g. if the requested configuration does not exist.
 *
 * @function module:uci.cursor#unload
 *
 * @param {string} config
 * The name of the configuration file to unload.
 *
 * @returns {?boolean}
 */
static uc_value_t *
uc_uci_unload(uc_vm_t *vm, size_t nargs)
{
	struct uci_context **c = uc_fn_this("uci.cursor");
	uc_value_t *conf = uc_fn_arg(0);
	struct uci_element *e;

	if (!c || !*c)
		err_return(UCI_ERR_INVAL);

	if (ucv_type(conf) != UC_STRING)
		err_return(UCI_ERR_INVAL);

	uci_foreach_element(&(*c)->root, e) {
		if (!strcmp(e->name, ucv_string_get(conf))) {
			uci_unload(*c, uci_to_package(e));

			ok_return(ucv_boolean_new(true));
		}
	}

	ok_return(ucv_boolean_new(false));
}

static int
lookup_extended(struct uci_context *ctx, struct uci_ptr *ptr, bool extended)
{
	int rv;
	struct uci_ptr lookup;

	/* use a copy of the passed ptr since failing lookups will
	 * clobber the state */
	lookup = *ptr;
	lookup.flags |= UCI_LOOKUP_EXTENDED;

	rv = uci_lookup_ptr(ctx, &lookup, NULL, extended);

	/* copy to passed ptr on success */
	if (!rv)
		*ptr = lookup;

	return rv;
}

static int
lookup_ptr(struct uci_context *ctx, struct uci_ptr *ptr, bool extended)
{
	if (ptr && !ptr->s && ptr->section && *ptr->section == '@')
		return lookup_extended(ctx, ptr, extended);

	return uci_lookup_ptr(ctx, ptr, NULL, extended);
}

static uc_value_t *
option_to_uval(uc_vm_t *vm, struct uci_option *o)
{
	struct uci_element *e;
	uc_value_t *arr;

	switch (o->type) {
	case UCI_TYPE_STRING:
		return ucv_string_new(o->v.string);

	case UCI_TYPE_LIST:
		arr = ucv_array_new(vm);

		if (arr)
			uci_foreach_element(&o->v.list, e)
				ucv_array_push(arr, ucv_string_new(e->name));

		return arr;

	default:
		return NULL;
	}
}

static uc_value_t *
section_to_uval(uc_vm_t *vm, struct uci_section *s, int index)
{
	uc_value_t *so = ucv_object_new(vm);
	struct uci_element *e;
	struct uci_option *o;

	if (!so)
		return NULL;

	ucv_object_add(so, ".anonymous", ucv_boolean_new(s->anonymous));
	ucv_object_add(so, ".type", ucv_string_new(s->type));
	ucv_object_add(so, ".name", ucv_string_new(s->e.name));

	if (index >= 0)
		ucv_object_add(so, ".index", ucv_int64_new(index));

	uci_foreach_element(&s->options, e) {
		o = uci_to_option(e);
		ucv_object_add(so, o->e.name, option_to_uval(vm, o));
	}

	return so;
}

static uc_value_t *
package_to_uval(uc_vm_t *vm, struct uci_package *p)
{
	uc_value_t *po = ucv_object_new(vm);
	uc_value_t *so;
	struct uci_element *e;
	int i = 0;

	if (!po)
		return NULL;

	uci_foreach_element(&p->sections, e) {
		so = section_to_uval(vm, uci_to_section(e), i++);
		ucv_object_add(po, e->name, so);
	}

	return po;
}

static uc_value_t *
uc_uci_get_any(uc_vm_t *vm, size_t nargs, bool all)
{
	struct uci_context **c = uc_fn_this("uci.cursor");
	uc_value_t *conf = uc_fn_arg(0);
	uc_value_t *sect = uc_fn_arg(1);
	uc_value_t *opt = uc_fn_arg(2);
	struct uci_ptr ptr = { 0 };
	int rv;

	if (!c || !*c)
		err_return(UCI_ERR_INVAL);

	if ((ucv_type(conf) != UC_STRING) ||
	    (sect && ucv_type(sect) != UC_STRING) ||
	    (opt && ucv_type(opt) != UC_STRING))
		err_return(UCI_ERR_INVAL);

	if ((!sect && !all) || (opt && all))
		err_return(UCI_ERR_INVAL);

	ptr.package = ucv_string_get(conf);
	ptr.section = sect ? ucv_string_get(sect) : NULL;
	ptr.option = opt ? ucv_string_get(opt) : NULL;

	rv = lookup_ptr(*c, &ptr, true);

	if (rv != UCI_OK)
		err_return(rv);

	if (!(ptr.flags & UCI_LOOKUP_COMPLETE))
		err_return(UCI_ERR_NOTFOUND);

	if (all) {
		if (ptr.section) {
			if (!ptr.s)
				err_return(UCI_ERR_NOTFOUND);

			ok_return(section_to_uval(vm, ptr.s, -1));
		}

		if (!ptr.p)
			err_return(UCI_ERR_NOTFOUND);

		ok_return(package_to_uval(vm, ptr.p));
	}

	if (ptr.option) {
		if (!ptr.o)
			err_return(UCI_ERR_NOTFOUND);

		ok_return(option_to_uval(vm, ptr.o));
	}

	if (!ptr.s)
		err_return(UCI_ERR_NOTFOUND);

	ok_return(ucv_string_new(ptr.s->type));
}

/**
 * Query a single option value or section type.
 *
 * When invoked with three arguments, the function returns the value of the
 * given option, within the specified section of the given configuration.
 *
 * When invoked with just a config and section argument, the function returns
 * the type of the specified section.
 *
 * In either case, the given configuration is implicitly loaded into the cursor
 * if not already present.
 *
 * Returns the configuration value or section type on success.
 *
 * Returns `null` on error, e.g. if the requested configuration does not exist
 * or if an invalid argument was passed.
 *
 * @function module:uci.cursor#get
 *
 * @param {string} config
 * The name of the configuration file to query, e.g. `"system"` to query values
 * in `/etc/config/system`.
 *
 * @param {string} section
 * The name of the section to query within the configuration.
 *
 * @param {string} [option]
 * The name of the option to query within the section. If omitted, the type of
 * the section is returned instead.
 *
 * @returns {?(string|string[])}
 *
 * @example
 * const ctx = cursor(…);
 *
 * // Query an option, extended section notation is supported
 * ctx.get('system', '@system[0]', 'hostname');
 *
 * // Query a section type (should yield 'interface')
 * ctx.get('network', 'lan');
 */
static uc_value_t *
uc_uci_get(uc_vm_t *vm, size_t nargs)
{
	return uc_uci_get_any(vm, nargs, false);
}

static uc_value_t *
str_to_bool(uc_vm_t *vm, uc_value_t *value)
{
	/* Values consistent with
	 * https://openwrt.org/docs/guide-developer/config-scripting#reading_booleans
	 * https://openwrt.github.io/luci/jsapi/LuCI.uci.html#get_bool
	 */
	static char *true_values[] = { "1", "enabled", "on", "true", "yes", };

	size_t i;
	char *str;
	bool is_true = false;

	if (ucv_type(value) != UC_STRING)
		err_return(UCI_ERR_INVAL);

	str = ucv_string_get(value);
	for (i = 0; i < ARRAY_SIZE(true_values); i++) {
		if (!strcasecmp(str, true_values[i])) {
			is_true = true;
			break;
		}
	}

	ok_return(ucv_boolean_new(is_true));
}

/**
 * Provide a special case for {@link module:uci.cursor#get|get()} that parses
 * and interprets truishness of boolean settings represented as strings.
 *
 * When the configuration option contains any of `"1"`, `"enabled"`, `"on"`,
 * `"true"` or `"yes"`, then a value of `true` is returned.  Any other value
 * in the config option will be interpreted as `false`.  Character case is
 * folded, so for example, `YES`, `Yes` and `yes` are all valid and interpreted
 * as `true`.
 *
 * Returns `null` on error, e.g., if the requested configuration does not
 * exist or if an invalid argument was passed.
 *
 * Note that there is no equivalent `set` function, so be careful to adhere to
 * the conventions of this method and use only values from the above list to
 * imply `true`; best practice is to simply call `set` using a boolean `true`.
 *
 * @function module:uci.cursor#get_bool
 *
 * @param {string} config
 * The name of the configuration file to query.
 *
 * @param {string} section
 * The name of the section to query within the configuration.
 *
 * @param {string} option
 * The name of the option to query within the section.
 *
 * @returns {?boolean}
 *
 * @example
 * const ctx = cursor(…);
 *
 * // Query if the ntp server is enabled
 * ctx.get_bool('system', 'ntp', 'enable_server');
 *
 * // Query if the dhcp server is disabled on wan
 * ctx.get_bool('dhcp', 'wan', 'ignore');
 */
static uc_value_t *
uc_uci_get_bool(uc_vm_t *vm, size_t nargs)
{
	uc_value_t *value = uc_uci_get_any(vm, nargs, false);

	if (!value)
		return NULL;

	return str_to_bool(vm, value);
}

/**
 * Determine the truishness of a string value, returning a bool result.
 *
 * An input value of `"1"`, `"enabled"`, `"on"`, `"true"` or `"yes"`,
 * returns `true`; any other value will be interpreted as `false`.
 * Character case is folded, so for example, `YES`, `Yes` and `yes`
 * are all valid and interpreted as `true`.
 *
 * @function module:uci#parse_bool
 *
 * @param {string} value
 * The input string to be converted into a boolean value.
 *
 * @returns {?boolean}
 *
 * @example
 * const ctx = cursor(…);
 *
 * ctx.parse_bool('yes');  // returns true
 * ctx.parse_bool('no');   // returns false
 * ctx.parse_bool('On');   // returns true
 * ctx.parse_bool('Off');  // returns false
 */
static uc_value_t *
uc_uci_parse_bool(uc_vm_t *vm, size_t nargs)
{
	return str_to_bool(vm, uc_fn_arg(0));
}

/**
 * Query a complete section or configuration.
 *
 * When invoked with two arguments, the function returns all values of the
 * specified section within the given configuration as dictionary.
 *
 * When invoked with just a config argument, the function returns a nested
 * dictionary of all sections present within the given configuration.
 *
 * In either case, the given configuration is implicitly loaded into the cursor
 * if not already present.
 *
 * Returns the section or configuration dictionary on success.
 *
 * Returns `null` on error, e.g. if the requested configuration does not exist
 * or if an invalid argument was passed.
 *
 * @function module:uci.cursor#get_all
 *
 * @param {string} config
 * The name of the configuration file to query, e.g. `"system"` to query values
 * in `/etc/config/system`.
 *
 * @param {string} [section]
 * The name of the section to query within the configuration. If omitted a
 * nested dictionary containing all section values is returned.
 *
 * @returns {?(Object<string, module:uci.cursor.SectionObject>|module:uci.cursor.SectionObject)}
 *
 * @example
 * const ctx = cursor(…);
 *
 * // Query all lan interface details
 * ctx.get_all('network', 'lan');
 *
 * // Dump the entire dhcp configuration
 * ctx.get_all('dhcp');
 */
static uc_value_t *
uc_uci_get_all(uc_vm_t *vm, size_t nargs)
{
	return uc_uci_get_any(vm, nargs, true);
}

/**
 * Query option value or name of first section of given type.
 *
 * When invoked with three arguments, the function returns the value of the
 * given option within the first found section of the specified type in the
 * given configuration.
 *
 * When invoked with just a config and section type argument, the function
 * returns the name of the first found section of the given type.
 *
 * In either case, the given configuration is implicitly loaded into the cursor
 * if not already present.
 *
 * Returns the configuration value or section name on success.
 *
 * Returns `null` on error, e.g. if the requested configuration does not exist
 * or if an invalid argument was passed.
 *
 * @function module:uci.cursor#get_first
 *
 * @param {string} config
 * The name of the configuration file to query, e.g. `"system"` to query values
 * in `/etc/config/system`.
 *
 * @param {string} type
 * The section type to find the first section for within the configuration.
 *
 * @param {string} [option]
 * The name of the option to query within the section. If omitted, the name of
 * the section is returned instead.
 *
 * @returns {?(string|string[])}
 *
 * @example
 * const ctx = cursor(…);
 *
 * // Query hostname in first anonymous "system" section of /etc/config/system
 * ctx.get_first('system', 'system', 'hostname');
 *
 * // Figure out name of first network interface section (usually "loopback")
 * ctx.get_first('network', 'interface');
 */
static uc_value_t *
uc_uci_get_first(uc_vm_t *vm, size_t nargs)
{
	struct uci_context **c = uc_fn_this("uci.cursor");
	uc_value_t *conf = uc_fn_arg(0);
	uc_value_t *type = uc_fn_arg(1);
	uc_value_t *opt = uc_fn_arg(2);
	struct uci_package *p = NULL;
	struct uci_section *sc;
	struct uci_element *e;
	struct uci_ptr ptr = { 0 };
	int rv;

	if (ucv_type(conf) != UC_STRING ||
	    ucv_type(type) != UC_STRING ||
	    (opt && ucv_type(opt) != UC_STRING))
		err_return(UCI_ERR_INVAL);

	uci_foreach_element(&(*c)->root, e) {
		if (strcmp(e->name, ucv_string_get(conf)))
			continue;

		p = uci_to_package(e);
		break;
	}

	if (!p && uci_load(*c, ucv_string_get(conf), &p))
		err_return((*c)->err);

	uci_foreach_element(&p->sections, e) {
		sc = uci_to_section(e);

		if (strcmp(sc->type, ucv_string_get(type)))
			continue;

		if (!opt)
			ok_return(ucv_string_new(sc->e.name));

		ptr.package = ucv_string_get(conf);
		ptr.section = sc->e.name;
		ptr.option = ucv_string_get(opt);
		ptr.p = p;
		ptr.s = sc;

		rv = lookup_ptr(*c, &ptr, false);

		if (rv != UCI_OK)
			err_return(rv);

		if (!(ptr.flags & UCI_LOOKUP_COMPLETE))
			err_return(UCI_ERR_NOTFOUND);

		ok_return(option_to_uval(vm, ptr.o));
	}

	err_return(UCI_ERR_NOTFOUND);
}

/**
 * Add anonymous section to given configuration.
 *
 * Adds a new anonymous (unnamed) section of the specified type to the given
 * configuration. In order to add a named section, the three argument form of
 * `set()` should be used instead.
 *
 * In contrast to other query functions, `add()` will not implicitly load the
 * configuration into the cursor. The configuration either needs to be loaded
 * explicitly through `load()` beforehand, or implicitly by querying it through
 * one of the `get()`, `get_all()`, `get_first()` or `foreach()` functions.
 *
 * Returns the autogenerated, ephemeral name of the added unnamed section
 * on success.
 *
 * Returns `null` on error, e.g. if the targeted configuration was not loaded or
 * if an invalid section type value was passed.
 *
 * @function module:uci.cursor#add
 *
 * @param {string} config
 * The name of the configuration file to add the section to, e.g. `"system"` to
 * modify `/etc/config/system`.
 *
 * @param {string} type
 * The type value to use for the added section.
 *
 * @returns {?string}
 *
 * @example
 * const ctx = cursor(…);
 *
 * // Load firewall configuration
 * ctx.load('firewall');
 *
 * // Add unnamed `config rule` section
 * const sid = ctx.add('firewall', 'rule');
 *
 * // Set values on the newly added section
 * ctx.set('firewall', sid, 'name', 'A test');
 * ctx.set('firewall', sid, 'target', 'ACCEPT');
 * …
 */
static uc_value_t *
uc_uci_add(uc_vm_t *vm, size_t nargs)
{
	struct uci_context **c = uc_fn_this("uci.cursor");
	uc_value_t *conf = uc_fn_arg(0);
	uc_value_t *type = uc_fn_arg(1);
	struct uci_element *e = NULL;
	struct uci_package *p = NULL;
	struct uci_section *sc = NULL;
	int rv;

	if (ucv_type(conf) != UC_STRING ||
	    ucv_type(type) != UC_STRING)
	    err_return(UCI_ERR_INVAL);

	uci_foreach_element(&(*c)->root, e) {
		if (!strcmp(e->name, ucv_string_get(conf))) {
			p = uci_to_package(e);
			break;
		}
	}

	if (!p)
		err_return(UCI_ERR_NOTFOUND);

	rv = uci_add_section(*c, p, ucv_string_get(type), &sc);

	if (rv != UCI_OK)
		err_return(rv);
	else if (!sc)
		err_return(UCI_ERR_NOTFOUND);

	return ucv_string_new(sc->e.name);
}

static bool
uval_to_uci(uc_vm_t *vm, uc_value_t *val, const char **p, bool *is_list)
{
	uc_value_t *item;

	*p = NULL;

	if (is_list)
		*is_list = false;

	switch (ucv_type(val)) {
	case UC_ARRAY:
		if (ucv_array_length(val) == 0)
			return false;

		item = ucv_array_get(val, 0);

		/* don't recurse */
		if (ucv_type(item) == UC_ARRAY)
			return false;

		if (is_list)
			*is_list = true;

		return uval_to_uci(vm, item, p, NULL);

	case UC_BOOLEAN:
		*p = xstrdup(ucv_boolean_get(val) ? "1" : "0");

		return true;

	case UC_DOUBLE:
	case UC_INTEGER:
	case UC_STRING:
		*p = ucv_to_string(vm, val);
		/* fall through */

	case UC_NULL:
		return true;

	default:
		return false;
	}
}

/**
 * Set option value or add named section in given configuration.
 *
 * When invoked with four arguments, the function sets the value of the given
 * option within the specified section of the given configuration to the
 * provided value. A value of `""` (empty string) can be used to delete an
 * existing option.
 *
 * When invoked with three arguments, the function adds a new named section to
 * the given configuration, using the specified type.
 *
 * In either case, the given configuration is implicitly loaded into the cursor
 * if not already present.
 *
 * Returns the `true` if the named section was added or the specified option was
 * set.
 *
 * Returns `null` on error, e.g. if the targeted configuration was not found or
 * if an invalid value was passed.
 *
 * @function module:uci.cursor#set
 *
 * @param {string} config
 * The name of the configuration file to set values in, e.g. `"system"` to
 * modify `/etc/config/system`.
 *
 * @param {string} section
 * The section name to create or set a value in.
 *
 * @param {string} option_or_type
 * The option name to set within the section or, when the subsequent value
 * argument is omitted, the type of the section to create within the
 * configuration.
 *
 * @param {(Array<string|boolean|number>|string|boolean|number)} [value]
 * The option value to set.
 *
 * @returns {?boolean}
 *
 * @example
 * const ctx = cursor(…);
 *
 * // Add named `config interface guest` section
 * ctx.set('network', 'guest', 'interface');
 *
 * // Set values on the newly added section
 * ctx.set('network', 'guest', 'proto', 'static');
 * ctx.set('network', 'guest', 'ipaddr', '10.0.0.1/24');
 * ctx.set('network', 'guest', 'dns', ['8.8.4.4', '8.8.8.8']);
 * …
 *
 * // Delete 'disabled' option in first wifi-iface section
 * ctx.set('wireless', '@wifi-iface[0]', 'disabled', '');
 */
static uc_value_t *
uc_uci_set(uc_vm_t *vm, size_t nargs)
{
	struct uci_context **c = uc_fn_this("uci.cursor");
	uc_value_t *conf = uc_fn_arg(0);
	uc_value_t *sect = uc_fn_arg(1);
	uc_value_t *opt = NULL, *val = NULL;
	struct uci_ptr ptr = { 0 };
	bool is_list = false;
	size_t i;
	int rv;

	if (ucv_type(conf) != UC_STRING ||
	    ucv_type(sect) != UC_STRING)
	    err_return(UCI_ERR_INVAL);

	switch (nargs) {
	/* conf, sect, opt, val */
	case 4:
		opt = uc_fn_arg(2);
		val = uc_fn_arg(3);

		if (ucv_type(opt) != UC_STRING)
			err_return(UCI_ERR_INVAL);

		break;

	/* conf, sect, type */
	case 3:
		val = uc_fn_arg(2);

		if (ucv_type(val) != UC_STRING)
			err_return(UCI_ERR_INVAL);

		break;

	default:
		err_return(UCI_ERR_INVAL);
	}

	ptr.package = ucv_string_get(conf);
	ptr.section = ucv_string_get(sect);
	ptr.option = opt ? ucv_string_get(opt) : NULL;

	rv = lookup_ptr(*c, &ptr, true);

	if (rv != UCI_OK)
		err_return(rv);

	if (!ptr.s && ptr.option)
		err_return(UCI_ERR_NOTFOUND);

	if (!uval_to_uci(vm, val, &ptr.value, &is_list))
		err_return(UCI_ERR_INVAL);

	if (is_list) {
		/* if we got a one-element array, delete existing option (if any)
		 * and iterate array at offset 0 */
		if (ucv_array_length(val) == 1) {
			i = 0;

			free((char *)ptr.value);
			ptr.value = NULL;

			if (ptr.o) {
				rv = uci_delete(*c, &ptr);

				if (rv != UCI_OK)
					err_return(rv);
			}
		}
		/* if we get a multi element array, overwrite existing option (if any)
		 * with first value and iterate remaining array at offset 1 */
		else {
			i = 1;

			rv = uci_set(*c, &ptr);
			free((char *)ptr.value);

			if (rv != UCI_OK)
				err_return(rv);
		}

		for (; i < ucv_array_length(val); i++) {
			if (!uval_to_uci(vm, ucv_array_get(val, i), &ptr.value, NULL))
				continue;

			rv = uci_add_list(*c, &ptr);
			free((char *)ptr.value);

			if (rv != UCI_OK)
				err_return(rv);
		}
	}
	else {
		rv = uci_set(*c, &ptr);
		free((char *)ptr.value);

		if (rv != UCI_OK)
			err_return(rv);
	}

	ok_return(ucv_boolean_new(true));
}

/**
 * Delete an option or section from given configuration.
 *
 * When invoked with three arguments, the function deletes the given option
 * within the specified section of the given configuration.
 *
 * When invoked with two arguments, the function deletes the entire specified
 * section within the given configuration.
 *
 * In either case, the given configuration is implicitly loaded into the cursor
 * if not already present.
 *
 * Returns the `true` if specified option or section has been deleted.
 *
 * Returns `null` on error, e.g. if the targeted configuration was not found or
 * if an invalid value was passed.
 *
 * @function module:uci.cursor#delete
 *
 * @param {string} config
 * The name of the configuration file to delete values in, e.g. `"system"` to
 * modify `/etc/config/system`.
 *
 * @param {string} section
 * The section name to remove the specified option in or, when the subsequent
 * argument is omitted, the section to remove entirely.
 *
 * @param {string} [option]
 * The option name to remove within the section.
 *
 * @returns {?boolean}
 *
 * @example
 * const ctx = cursor(…);
 *
 * // Delete 'disabled' option in first wifi-iface section
 * ctx.delete('wireless', '@wifi-iface[0]', 'disabled');
 *
 * // Delete 'wan' interface
 * ctx.delete('network', 'lan');
 *
 * // Delete last firewall rule
 * ctx.delete('firewall', '@rule[-1]');
 */
static uc_value_t *
uc_uci_delete(uc_vm_t *vm, size_t nargs)
{
	struct uci_context **c = uc_fn_this("uci.cursor");
	uc_value_t *conf = uc_fn_arg(0);
	uc_value_t *sect = uc_fn_arg(1);
	uc_value_t *opt = uc_fn_arg(2);
	struct uci_ptr ptr = { 0 };
	int rv;

	if (ucv_type(conf) != UC_STRING ||
	    ucv_type(sect) != UC_STRING ||
	    (opt && ucv_type(opt) != UC_STRING))
	    err_return(UCI_ERR_INVAL);

	ptr.package = ucv_string_get(conf);
	ptr.section = ucv_string_get(sect);
	ptr.option = opt ? ucv_string_get(opt) : NULL;

	rv = lookup_ptr(*c, &ptr, true);

	if (rv != UCI_OK)
		err_return(rv);

	if (opt ? !ptr.o : !ptr.s)
		err_return(UCI_ERR_NOTFOUND);

	rv = uci_delete(*c, &ptr);

	if (rv != UCI_OK)
		err_return(rv);

	ok_return(ucv_boolean_new(true));
}

static uc_value_t *
uc_uci_list_modify(uc_vm_t *vm, size_t nargs,
                   int (*op)(struct uci_context *, struct uci_ptr *))
{
	struct uci_context **c = uc_fn_this("uci.cursor");
	uc_value_t *conf = uc_fn_arg(0);
	uc_value_t *sect = uc_fn_arg(1);
	uc_value_t *opt = uc_fn_arg(2);
	uc_value_t *val = uc_fn_arg(3);
	struct uci_ptr ptr = { 0 };
	bool is_list;
	int rv;

	if (ucv_type(conf) != UC_STRING ||
	    ucv_type(sect) != UC_STRING ||
	    ucv_type(opt) != UC_STRING)
		err_return(UCI_ERR_INVAL);

	ptr.package = ucv_string_get(conf);
	ptr.section = ucv_string_get(sect);
	ptr.option = ucv_string_get(opt);

	rv = lookup_ptr(*c, &ptr, true);

	if (rv != UCI_OK)
		err_return(rv);

	if (!ptr.s)
		err_return(UCI_ERR_NOTFOUND);

	if (uval_to_uci(vm, val, &ptr.value, &is_list) && !is_list)
		rv = op(*c, &ptr);
	else
		rv = UCI_ERR_INVAL;

	free((char *)ptr.value);

	if (rv != UCI_OK)
		err_return(rv);

	ok_return(ucv_boolean_new(true));
}

/**
 * Add an item to a list option in given configuration.
 *
 * Adds a single value to an existing list option within the specified section
 * of the given configuration. The configuration is implicitly loaded into the
 * cursor if not already present.
 *
 * The new value is appended to the end of the list, maintaining the existing order.
 * No attempt is made to check for or remove duplicate values.
 *
 * Returns `true` if the item was successfully added to the list.
 *
 * Returns `null` on error, e.g. if the targeted option was not found or
 * if an invalid value was passed.
 *
 * @function module:uci.cursor#list_append
 *
 * @param {string} config
 * The name of the configuration file to modify, e.g. `"firewall"` to
 * modify `/etc/config/firewall`.
 *
 * @param {string} section
 * The section name containing the list option to modify.
 *
 * @param {string} option
 * The list option name to add a value to.
 *
 * @param {string|boolean|number} value
 * The value to add to the list option.
 *
 * @returns {?boolean}
 *
 * @example
 * const ctx = cursor(…);
 *
 * // Add '192.168.1.1' to the 'dns' list in the 'lan' interface
 * ctx.add_list('network', 'lan', 'dns', '192.168.1.1');
 *
 * // Add a port to the first redirect section
 * ctx.add_list('firewall', '@redirect[0]', 'src_dport', '8080');
 */
static uc_value_t *
uc_uci_list_append(uc_vm_t *vm, size_t nargs)
{
	return uc_uci_list_modify(vm, nargs, uci_add_list);
}

/**
 * Remove an item from a list option in given configuration.
 *
 * Removes a single value from an existing list option within the specified section
 * of the given configuration. The configuration is implicitly loaded into the
 * cursor if not already present.
 *
 * If the specified value appears multiple times in the list, all matching occurrences
 * will be removed.
 *
 * Returns `true` if the item was successfully removed from the list.
 *
 * Returns `null` on error, e.g. if the targeted option was not foundor if an
 * invalid value was passed.
 *
 * @function module:uci.cursor#list_remove
 *
 * @param {string} config
 * The name of the configuration file to modify, e.g. `"firewall"` to
 * modify `/etc/config/firewall`.
 *
 * @param {string} section
 * The section name containing the list option to modify.
 *
 * @param {string} option
 * The list option name to remove a value from.
 *
 * @param {string|boolean|number} value
 * The value to remove from the list option.
 *
 * @returns {?boolean}
 *
 * @example
 * const ctx = cursor(…);
 *
 * // Remove '8.8.8.8' from the 'dns' list in the 'lan' interface
 * ctx.delete_list('network', 'lan', 'dns', '8.8.8.8');
 *
 * // Remove a port from the first redirect section
 * ctx.delete_list('firewall', '@redirect[0]', 'src_dport', '8080');
 */
static uc_value_t *
uc_uci_list_remove(uc_vm_t *vm, size_t nargs)
{
	return uc_uci_list_modify(vm, nargs, uci_del_list);
}

/**
 * Rename an option or section in given configuration.
 *
 * When invoked with four arguments, the function renames the given option
 * within the specified section of the given configuration to the provided
 * value.
 *
 * When invoked with three arguments, the function renames the entire specified
 * section to the provided value.
 *
 * In either case, the given configuration is implicitly loaded into the cursor
 * if not already present.
 *
 * Returns the `true` if specified option or section has been renamed.
 *
 * Returns `null` on error, e.g. if the targeted configuration was not found or
 * if an invalid value was passed.
 *
 * @function module:uci.cursor#rename
 *
 * @param {string} config
 * The name of the configuration file to rename values in, e.g. `"system"` to
 * modify `/etc/config/system`.
 *
 * @param {string} section
 * The section name to rename or to rename an option in.
 *
 * @param {string} option_or_name
 * The option name to rename within the section or, when the subsequent name
 * argument is omitted, the new name of the renamed section within the
 * configuration.
 *
 * @param {string} [name]
 * The new name of the option to rename.
 *
 * @returns {?boolean}
 *
 * @example
 * const ctx = cursor(…);
 *
 * // Assign explicit name to last anonymous firewall rule section
 * ctx.rename('firewall', '@rule[-1]', 'my_block_rule');
 *
 * // Rename 'server' to 'orig_server_list' in ntp section of system config
 * ctx.rename('system', 'ntp', 'server', 'orig_server_list');
 *
 * // Rename 'wan' interface to 'external'
 * ctx.rename('network', 'wan', 'external');
 */
static uc_value_t *
uc_uci_rename(uc_vm_t *vm, size_t nargs)
{
	struct uci_context **c = uc_fn_this("uci.cursor");
	uc_value_t *conf = uc_fn_arg(0);
	uc_value_t *sect = uc_fn_arg(1);
	uc_value_t *opt = NULL, *val = NULL;
	struct uci_ptr ptr = { 0 };
	int rv;

	if (ucv_type(conf) != UC_STRING ||
	    ucv_type(sect) != UC_STRING)
	    err_return(UCI_ERR_INVAL);

	switch (nargs) {
	/* conf, sect, opt, val */
	case 4:
		opt = uc_fn_arg(2);
		val = uc_fn_arg(3);

		if (ucv_type(opt) != UC_STRING ||
		    ucv_type(val) != UC_STRING)
			err_return(UCI_ERR_INVAL);

		break;

	/* conf, sect, type */
	case 3:
		val = uc_fn_arg(2);

		if (ucv_type(val) != UC_STRING)
			err_return(UCI_ERR_INVAL);

		break;

	default:
		err_return(UCI_ERR_INVAL);
	}

	ptr.package = ucv_string_get(conf);
	ptr.section = ucv_string_get(sect);
	ptr.option = opt ? ucv_string_get(opt) : NULL;
	ptr.value = ucv_string_get(val);

	rv = lookup_ptr(*c, &ptr, true);

	if (rv != UCI_OK)
		err_return(rv);

	if (!ptr.s && ptr.option)
		err_return(UCI_ERR_NOTFOUND);

	rv = uci_rename(*c, &ptr);

	if (rv != UCI_OK)
		err_return(rv);

	ok_return(ucv_boolean_new(true));
}

/**
 * Reorder sections in given configuration.
 *
 * The `reorder()` function moves a single section by repositioning it to the
 * given index within the configurations section list.
 *
 * The given configuration is implicitly loaded into the cursor if not already
 * present.
 *
 * Returns the `true` if specified section has been moved.
 *
 * Returns `null` on error, e.g. if the targeted configuration was not found or
 * if an invalid value was passed.
 *
 * @function module:uci.cursor#reorder
 *
 * @param {string} config
 * The name of the configuration file to move the section in, e.g. `"system"` to
 * modify `/etc/config/system`.
 *
 * @param {string} section
 * The section name to move.
 *
 * @param {number} index
 * The target index to move the section to, starting from `0`.
 *
 * @returns {?boolean}
 *
 * @example
 * const ctx = cursor(…);
 *
 * // Query whole firewall config and reorder resulting dict by type and name
 * const type_order = ['defaults', 'zone', 'forwarding', 'redirect', 'rule'];
 * const values = ctx.get_all('firewall');
 *
 * sort(values, (k1, k2, s1, s2) => {
 *     // Get weight from type_order array
 *     let w1 = index(type_order, s1['.type']);
 *     let w2 = index(type_order, s2['.type']);
 *
 *     // For unknown type orders, use type value itself as weight
 *     if (w1 == -1) w1 = s1['.type'];
 *     if (w2 == -1) w2 = s2['.type'];
 *
 *     // Get name from name option, fallback to section name
 *     let n1 = s1.name ?? k1;
 *     let n2 = s2.name ?? k2;
 *
 *     // Order by weight
 *     if (w1 < w2) return -1;
 *     if (w1 > w2) return 1;
 *
 *     // For same weight order by name
 *     if (n1 < n2) return -1;
 *     if (n1 > n2) return 1;
 *
 *     return 0;
 * });
 *
 * // Sequentially reorder sorted sections in firewall configuration
 * let position = 0;
 *
 * for (let sid in values)
 *   ctx.reorder('firewall', sid, position++);
 */
static uc_value_t *
uc_uci_reorder(uc_vm_t *vm, size_t nargs)
{
	struct uci_context **c = uc_fn_this("uci.cursor");
	uc_value_t *conf = uc_fn_arg(0);
	uc_value_t *sect = uc_fn_arg(1);
	uc_value_t *val = uc_fn_arg(2);
	struct uci_ptr ptr = { 0 };
	int64_t n;
	int rv;

	if (ucv_type(conf) != UC_STRING ||
	    ucv_type(sect) != UC_STRING ||
	    ucv_type(val) != UC_INTEGER)
	    err_return(UCI_ERR_INVAL);

	n = ucv_int64_get(val);

	if (n < 0)
		err_return(UCI_ERR_INVAL);

	ptr.package = ucv_string_get(conf);
	ptr.section = ucv_string_get(sect);

	rv = lookup_ptr(*c, &ptr, true);

	if (rv != UCI_OK)
		err_return(rv);

	if (!ptr.s)
		err_return(UCI_ERR_NOTFOUND);

	rv = uci_reorder_section(*c, ptr.s, n);

	if (rv != UCI_OK)
		err_return(rv);

	ok_return(ucv_boolean_new(true));
}

static int
uc_uci_pkg_command_single(struct uci_context *ctx, enum pkg_cmd cmd,
                          struct uci_package *pkg)
{
	struct uci_ptr ptr = { 0 };

	switch (cmd) {
	case CMD_COMMIT:
		return uci_commit(ctx, &pkg, false);

	case CMD_SAVE:
		return uci_save(ctx, pkg);

	case CMD_REVERT:
		ptr.p = pkg;

		return uci_revert(ctx, &ptr);

	default:
		return UCI_ERR_INVAL;
	}
}

static uc_value_t *
uc_uci_pkg_command(uc_vm_t *vm, size_t nargs, enum pkg_cmd cmd)
{
	struct uci_context **c = uc_fn_this("uci.cursor");
	uc_value_t *conf = uc_fn_arg(0);
	struct uci_package *p;
	char **configs = NULL;
	int rv, res = UCI_OK;
	size_t i;

	if (conf) {
		if (ucv_type(conf) != UC_STRING)
			err_return(UCI_ERR_INVAL);

		if (!(p = uci_lookup_package(*c, ucv_string_get(conf))))
			err_return(UCI_ERR_NOTFOUND);

		res = uc_uci_pkg_command_single(*c, cmd, p);
	}
	else {
		if (uci_list_configs(*c, &configs))
			err_return((*c)->err);

		if (!configs || !configs[0]) {
			free(configs);
			err_return(UCI_ERR_NOTFOUND);
		}

		for (i = 0; configs[i]; i++) {
			if (!(p = uci_lookup_package(*c, configs[i])))
				continue;

			rv = uc_uci_pkg_command_single(*c, cmd, p);

			if (rv != UCI_OK)
				res = rv;
		}

		free(configs);
	}

	if (res != UCI_OK)
		err_return(res);

	ok_return(ucv_boolean_new(true));
}

/**
 * Save accumulated cursor changes to delta directory.
 *
 * The `save()` function writes consolidated changes made to in-memory copies of
 * loaded configuration files to the uci delta directory which effectively makes
 * them available to other processes using the same delta directory path as well
 * as the `uci changes` cli command when using the default delta directory.
 *
 * Note that uci deltas are overlayed over the actual configuration file values
 * so they're reflected by `get()`, `foreach()` etc. even if the underlying
 * configuration files are not actually changed (yet). The delta records may be
 * either permanently merged into the configuration by invoking `commit()` or
 * reverted through `revert()` in order to restore the current state of the
 * underlying configuration file.
 *
 * When the optional "config" parameter is omitted, delta records for all
 * currently loaded configuration files are written.
 *
 * In case that neither sharing changes with other processes nor any revert
 * functionality is required, changes may be committed directly using `commit()`
 * instead, bypassing any delta record creation.
 *
 * Returns the `true` if operation completed successfully.
 *
 * Returns `null` on error, e.g. if the requested configuration was not loaded
 * or when a file system error occurred.
 *
 * @function module:uci.cursor#save
 *
 * @param {string} [config]
 * The name of the configuration file to save delta records for, e.g. `"system"`
 * to store changes for `/etc/config/system`.
 *
 * @returns {?boolean}
 *
 * @example
 * const ctx = cursor(…);
 *
 * ctx.set('wireless', '@wifi-iface[0]', 'disabled', '1');
 * ctx.save('wireless');
 *
 * @see {@link module:uci.cursor#commit|commit()}
 * @see {@link module:uci.cursor#revert|revert()}
 */
static uc_value_t *
uc_uci_save(uc_vm_t *vm, size_t nargs)
{
	return uc_uci_pkg_command(vm, nargs, CMD_SAVE);
}

/**
 * Update configuration files with accumulated cursor changes.
 *
 * The `commit()` function merges changes made to in-memory copies of loaded
 * configuration files as well as existing delta records in the cursors
 * configured delta directory and writes them back into the underlying
 * configuration files, persistently committing changes to the file system.
 *
 * When the optional "config" parameter is omitted, all currently loaded
 * configuration files with either present delta records or yet unsaved
 * cursor changes are updated.
 *
 * Returns the `true` if operation completed successfully.
 *
 * Returns `null` on error, e.g. if the requested configuration was not loaded
 * or when a file system error occurred.
 *
 * @function module:uci.cursor#commit
 *
 * @param {string} [config]
 * The name of the configuration file to commit, e.g. `"system"` to update the
 * `/etc/config/system` file.
 *
 * @returns {?boolean}
 *
 * @example
 * const ctx = cursor(…);
 *
 * ctx.set('system', '@system[0]', 'hostname', 'example.org');
 * ctx.commit('system');
 */
static uc_value_t *
uc_uci_commit(uc_vm_t *vm, size_t nargs)
{
	return uc_uci_pkg_command(vm, nargs, CMD_COMMIT);
}

/**
 * Revert accumulated cursor changes and associated delta records.
 *
 * The `revert()` function discards any changes made to in-memory copies of
 * loaded configuration files and discards any related existing delta records in
 * the  cursors configured delta directory.
 *
 * When the optional "config" parameter is omitted, all currently loaded
 * configuration files with either present delta records or yet unsaved
 * cursor changes are reverted.
 *
 * Returns the `true` if operation completed successfully.
 *
 * Returns `null` on error, e.g. if the requested configuration was not loaded
 * or when a file system error occurred.
 *
 * @function module:uci.cursor#revert
 *
 * @param {string} [config]
 * The name of the configuration file to revert, e.g. `"system"` to discard any
 * changes for the `/etc/config/system` file.
 *
 * @returns {?boolean}
 *
 * @example
 * const ctx = cursor(…);
 *
 * ctx.set('system', '@system[0]', 'hostname', 'example.org');
 * ctx.revert('system');
 *
 * @see {@link module:uci.cursor#save|save()}
 */
static uc_value_t *
uc_uci_revert(uc_vm_t *vm, size_t nargs)
{
	return uc_uci_pkg_command(vm, nargs, CMD_REVERT);
}

static uc_value_t *
change_to_uval(uc_vm_t *vm, struct uci_delta *d)
{
	const char *types[] = {
		[UCI_CMD_REORDER]  = "order",
		[UCI_CMD_REMOVE]   = "remove",
		[UCI_CMD_RENAME]   = "rename",
		[UCI_CMD_ADD]      = "add",
		[UCI_CMD_LIST_ADD] = "list-add",
		[UCI_CMD_LIST_DEL] = "list-del",
		[UCI_CMD_CHANGE]   = "set",
	};

	uc_value_t *a;

	if (!d->section)
		return NULL;

	a = ucv_array_new(vm);

	if (!a)
		return NULL;

	ucv_array_push(a, ucv_string_new(types[d->cmd]));
	ucv_array_push(a, ucv_string_new(d->section));

	if (d->e.name)
		ucv_array_push(a, ucv_string_new(d->e.name));

	if (d->value) {
		if (d->cmd == UCI_CMD_REORDER)
			ucv_array_push(a, ucv_int64_new(strtoul(d->value, NULL, 10)));
		else
			ucv_array_push(a, ucv_string_new(d->value));
	}

	return a;
}

static uc_value_t *
changes_to_uval(uc_vm_t *vm, struct uci_context *ctx, const char *package,
                bool unload)
{
	uc_value_t *a = NULL, *c;
	struct uci_package *p = NULL;
	struct uci_element *e;

	uci_foreach_element(&ctx->root, e) {
		if (strcmp(e->name, package))
			continue;

		p = uci_to_package(e);
	}

	if (!p)
		uci_load(ctx, package, &p);
	else
		unload = false;

	if (!p)
		return NULL;

	if (!uci_list_empty(&p->delta) || !uci_list_empty(&p->saved_delta)) {
		a = ucv_array_new(vm);

		if (!a)
			err_return(UCI_ERR_MEM);

		uci_foreach_element(&p->saved_delta, e) {
			c = change_to_uval(vm, uci_to_delta(e));

			if (c)
				ucv_array_push(a, c);
		}

		uci_foreach_element(&p->delta, e) {
			c = change_to_uval(vm, uci_to_delta(e));

			if (c)
				ucv_array_push(a, c);
		}
	}

	if (unload)
		uci_unload(ctx, p);

	return a;
}

/**
 * Enumerate pending changes.
 *
 * The `changes()` function returns a list of change records for currently
 * loaded configuration files, originating both from the cursors associated
 * delta directory and yet unsaved cursor changes.
 *
 * When the optional "config" parameter is specified, the requested
 * configuration is implicitly loaded if it is not already loaded into the
 * cursor.
 *
 * Returns a dictionary of change record arrays, keyed by configuration name.
 *
 * Returns `null` on error, e.g. if the requested configuration could not be
 * loaded.
 *
 * @function module:uci.cursor#changes
 *
 * @param {string} [config]
 * The name of the configuration file to enumerate changes for, e.g. `"system"`
 * to query pending changes for the `/etc/config/system` file.
 *
 * @returns {?Object<string, module:uci.cursor.ChangeRecord[]>}
 *
 * @example
 * const ctx = cursor(…);
 *
 * // Enumerate changes for all currently loaded configurations
 * const deltas = ctx.changes();
 *
 * // Explicitly load and enumerate changes for the "system" configuration
 * const deltas = ctx.changes('system');
 */
static uc_value_t *
uc_uci_changes(uc_vm_t *vm, size_t nargs)
{
	struct uci_context **c = uc_fn_this("uci.cursor");
	uc_value_t *conf = uc_fn_arg(0);
	uc_value_t *res, *chg;
	char **configs;
	int rv, i;

	if (conf && ucv_type(conf) != UC_STRING)
		err_return(UCI_ERR_INVAL);

	rv = uci_list_configs(*c, &configs);

	if (rv != UCI_OK)
		err_return(rv);

	res = ucv_object_new(vm);

	for (i = 0; configs[i]; i++) {
		if (conf && strcmp(configs[i], ucv_string_get(conf)))
			continue;

		chg = changes_to_uval(vm, *c, configs[i], !conf);

		if (chg)
			ucv_object_add(res, configs[i], chg);
	}

	free(configs);

	ok_return(res);
}

/**
 * Iterate configuration sections.
 *
 * The `foreach()` function iterates all sections of the given configuration,
 * optionally filtered by type, and invokes the given callback function for
 * each encountered section.
 *
 * When the optional "type" parameter is specified, the callback is only invoked
 * for sections of the given type, otherwise it is invoked for all sections.
 *
 * The requested configuration is implicitly loaded into the cursor.
 *
 * Returns `true` if the callback was executed successfully at least once.
 *
 * Returns `false` if the callback was never invoked, e.g. when the
 * configuration is empty or contains no sections of the given type.
 *
 * Returns `null` on error, e.g. when an invalid callback was passed or the
 * requested configuration not found.
 *
 * @function module:uci.cursor#foreach
 *
 * @param {string} config
 * The configuration to iterate sections for, e.g. `"system"` to read the
 * `/etc/config/system` file.
 *
 * @param {?string} type
 * Invoke the callback only for sections of the specified type.
 *
 * @param {module:uci.cursor.SectionCallback} callback
 * The callback to invoke for each section, will receive a section dictionary
 * as sole argument.
 *
 * @returns {?boolean}
 *
 * @example
 * const ctx = cursor(…);
 *
 * // Iterate all network interfaces
 * ctx.foreach('network', 'interface',
 * 	   section => print(`Have interface ${section[".name"]}\n`));
 */
static uc_value_t *
uc_uci_foreach(uc_vm_t *vm, size_t nargs)
{
	struct uci_context **c = uc_fn_this("uci.cursor");
	uc_value_t *conf = uc_fn_arg(0);
	uc_value_t *type = uc_fn_arg(1);
	uc_value_t *func = uc_fn_arg(2);
	uc_value_t *rv = NULL;
	struct uci_package *p = NULL;
	struct uci_element *e, *tmp;
	struct uci_section *sc;
	uc_exception_type_t ex;
	bool stop = false;
	bool ret = false;
	int i = 0;

	if (ucv_type(conf) != UC_STRING ||
	    (type && ucv_type(type) != UC_STRING))
	    err_return(UCI_ERR_INVAL);

	uci_foreach_element(&(*c)->root, e) {
		if (strcmp(e->name, ucv_string_get(conf)))
			continue;

		p = uci_to_package(e);
		break;
	}

	if (!p && uci_load(*c, ucv_string_get(conf), &p))
		err_return((*c)->err);

	uci_foreach_element_safe(&p->sections, tmp, e) {
		sc = uci_to_section(e);
		i++;

		if (type && strcmp(sc->type, ucv_string_get(type)))
			continue;

		uc_vm_stack_push(vm, ucv_get(func));
		uc_vm_stack_push(vm, section_to_uval(vm, sc, i - 1));

		ex = uc_vm_call(vm, false, 1);

		/* stop on exception in callback */
		if (ex)
			break;

		ret = true;
		rv = uc_vm_stack_pop(vm);
		stop = (ucv_type(rv) == UC_BOOLEAN && !ucv_boolean_get(rv));

		ucv_put(rv);

		if (stop)
			break;
	}

	ok_return(ucv_boolean_new(ret));
}

/**
 * Enumerate existing configurations.
 *
 * The `configs()` function yields an array of configuration files present in
 * the cursors associated configuration directory, `/etc/config/` by default.
 *
 * Returns an array of configuration names on success.
 *
 * Returns `null` on error, e.g. due to filesystem errors.
 *
 * @function module:uci.cursor#configs
 *
 * @returns {?string[]}
 *
 * @example
 * const ctx = cursor(…);
 *
 * // Enumerate all present configuration file names
 * const configurations = ctx.configs();
 */
static uc_value_t *
uc_uci_configs(uc_vm_t *vm, size_t nargs)
{
	struct uci_context **c = uc_fn_this("uci.cursor");
	uc_value_t *a;
	char **configs;
	int i, rv;

	rv = uci_list_configs(*c, &configs);

	if (rv != UCI_OK)
		err_return(rv);

	a = ucv_array_new(vm);

	for (i = 0; configs[i]; i++)
		ucv_array_push(a, ucv_string_new(configs[i]));

	free(configs);

	ok_return(a);
}


static const uc_function_list_t cursor_fns[] = {
	{ "load",			uc_uci_load },
	{ "unload",			uc_uci_unload },
	{ "get",			uc_uci_get },
	{ "get_all",		uc_uci_get_all },
	{ "get_first",		uc_uci_get_first },
	{ "get_bool",			uc_uci_get_bool },
	{ "add",			uc_uci_add },
	{ "set",			uc_uci_set },
	{ "rename",			uc_uci_rename },
	{ "save",			uc_uci_save },
	{ "delete",			uc_uci_delete },
	{ "list_append",	uc_uci_list_append },
	{ "list_remove", 	uc_uci_list_remove },
	{ "commit",			uc_uci_commit },
	{ "revert",			uc_uci_revert },
	{ "reorder",		uc_uci_reorder },
	{ "changes",		uc_uci_changes },
	{ "foreach",		uc_uci_foreach },
	{ "configs",		uc_uci_configs },
	{ "error",			uc_uci_error },
	{ "parse_bool",		uc_uci_parse_bool },
};

static const uc_function_list_t global_fns[] = {
	{ "error",		uc_uci_error },
	{ "parse_bool",		uc_uci_parse_bool },
	{ "cursor",		uc_uci_cursor },
};


static void close_uci(void *ud) {
	uci_free_context((struct uci_context *)ud);
}

void uc_module_init(uc_vm_t *vm, uc_value_t *scope)
{
	uc_function_list_register(scope, global_fns);

	uc_type_declare(vm, "uci.cursor", cursor_fns, close_uci);
}
