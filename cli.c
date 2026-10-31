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

/* Command line options shared by the ucode CLI and native programs. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "ucode/lib.h"
#include "ucode/vm.h"
#include "ucode/internal/cli.h"
#include "ucode/internal/types.h"
#include "ucode/json.h"

/* Adds the properties of the object `obj` to `globals` and releases it */
static void
uc_cli_define_object(uc_value_t *globals, uc_value_t *obj)
{
	ucv_object_foreach_entry(obj, e)
		ucv_object_insert_string(globals, e->key, ucv_get(e->value));

	ucv_put(obj);
}

bool
uc_cli_define_file(char *opt, uc_value_t *globals, FILE **stdin_unused)
{
	uc_json_error_t err = UC_JSON_CONTINUE;
	char buf[128], *name = NULL, *p;
	uc_json_parser_t *parser;
	uc_value_t *val = NULL;
	size_t rlen;
	FILE *fp;

	p = strchr(opt, '=');

	if (p) {
		name = opt;
		*p++ = 0;
	}
	else {
		p = opt;
	}

	if (!strcmp(p, "-")) {
		if (!*stdin_unused) {
			fprintf(stderr, "The stdin can only be read once\n");

			return false;
		}

		fp = *stdin_unused;
		*stdin_unused = NULL;
	}
	else
		fp = fopen(p, "r");

	if (!fp) {
		fprintf(stderr, "Unable to open definition file \"%s\": %s\n",
		        p, strerror(errno));

		return true;
	}

	parser = uc_json_parser_new(NULL);

	while (true) {
		rlen = fread(buf, 1, sizeof(buf), fp);

		if (rlen == 0)
			break;

		val = uc_json_parse(parser, buf, rlen);
		err = uc_json_parser_error(parser);

		if (err != UC_JSON_CONTINUE)
			break;
	}

	uc_json_parser_free(parser);
	fclose(fp);

	if (err != UC_JSON_SUCCESS || ucv_type(val) != UC_OBJECT) {
		ucv_put(val);

		fprintf(stderr, "Invalid definition file \"%s\": %s\n",
		        p, (err != UC_JSON_SUCCESS)
		             ? "JSON parse failure" : "Not a valid JSON object");

		return false;
	}

	if (name && *name)
		ucv_object_add(globals, name, val);
	else
		uc_cli_define_object(globals, val);

	return true;
}

bool
uc_cli_define_string(char *opt, uc_value_t *globals)
{
	uc_json_parser_t *parser;
	char *name = NULL, *p;
	uc_value_t *val;
	uc_json_error_t err;
	size_t len;

	p = strchr(opt, '=');

	if (p) {
		name = opt;
		*p++ = 0;
	}
	else {
		p = opt;
	}

	len = strlen(p);
	parser = uc_json_parser_new(NULL);

	/* NB: the len + 1 here is intentional, the terminating \0 byte ends the
	 * input, so that values without delimiter such as `null` complete */
	val = uc_json_parse(parser, p, len + 1);

	err = uc_json_parser_error(parser);

	/* Treat trailing bytes after a parsed value as error */
	if (err == UC_JSON_SUCCESS && uc_json_parser_end(parser) < len)
		err = UC_JSON_ERROR_UNEXPECTED;

	uc_json_parser_free(parser);

	if (err != UC_JSON_SUCCESS) {
		ucv_put(val);

		if (!name || !*name) {
			fprintf(stderr, "Invalid -D option value \"%s\": %s\n",
			        p, uc_json_error_desc(err));

			return false;
		}

		ucv_object_add(globals, name, ucv_string_new(p));

		return true;
	}

	if (name && *name) {
		ucv_object_add(globals, name, val);

		return true;
	}

	if (ucv_type(val) == UC_OBJECT) {
		uc_cli_define_object(globals, val);

		return true;
	}

	ucv_put(val);
	fprintf(stderr, "Invalid -D option value \"%s\": Not a valid JSON object\n", p);

	return false;
}

void
uc_cli_search_path(char *pattern, uc_parse_config_t *config)
{
	size_t len;
	char *p;

	if (strchr(pattern, '*')) {
		uc_search_path_add(&config->module_search_path, pattern);
		return;
	}

	len = strlen(pattern);

	if (!len)
		return;

	while (pattern[len-1] == '/')
		pattern[--len] = 0;

	xasprintf(&p, "%s/*.so", pattern);
	uc_search_path_add(&config->module_search_path, p);
	free(p);

	xasprintf(&p, "%s/*.uc", pattern);
	uc_search_path_add(&config->module_search_path, p);
	free(p);
}

bool
uc_cli_library_load(char *opt, uc_vm_t *vm)
{
	char *name = NULL, *p;
	uc_value_t *lib, *ctx;

	p = strchr(opt, '=');

	if (p) {
		name = opt;
		*p++ = 0;
	}
	else {
		p = opt;
	}

	lib = ucv_string_new(p);
	ctx = uc_vm_invoke(vm, "require", 1, lib);
	ucv_put(lib);

	if (!ctx)
		return vm->exception.type == EXCEPTION_NONE;

	ucv_object_add(uc_vm_scope_get(vm), name ? name : p, ctx);

	return true;
}
