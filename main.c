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

#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>


#include "ucode/compiler.h"
#include "ucode/lib.h"
#include "ucode/vm.h"
#include "ucode/types.h"
#include "ucode/source.h"
#include "ucode/program.h"

#include "ucode/internal/cli.h"
#include "ucode/internal/native.h"

#ifdef __GLIBC__
# define POSIXLY_CORRECT_FLAG "+"
#else
# define POSIXLY_CORRECT_FLAG ""
#endif

static FILE *stdin_unused;

static void
print_usage(const char *app)
{
	printf(
	"Usage:\n"
	"  %1$s -h\n"
	"  %1$s -e \"expression\"\n"
	"  %1$s input.uc [input2.uc ...]\n"
	"  %1$s -c [-s] [-o output.uc] input.uc [input2.uc ...]\n\n"

	"-h\n"
	"  Help display this help.\n\n"

	"-e \"expression\"\n"
	"  Execute the given expression as ucode program.\n\n"

	"-p \"expression\"\n"
	"  Like `-e` but print the result of expression.\n\n"

	"-t\n"
	"  Enable VM execution tracing.\n\n"

	"-g interval\n"
	"  Perform periodic garbage collection every `interval` object\n"
	"  allocations.\n\n"

	"-S\n"
	"  Enable strict mode.\n\n"

	"-R\n"
	"  Process source file(s) as raw script code (default).\n\n"

	"-T[flag,flag,...]\n"
	"  Process the source file(s) as templates, not as raw script code.\n"
	"  Supported flags: no-lstrip (don't strip leading whitespace before\n"
	"  block tags), no-rtrim (don't strip trailing newline after block tags).\n\n"

	"-D [name=]value\n"
	"  Define global variable. If `name` is omitted, a JSON dictionary is\n"
	"  expected with each property becoming a global variable set to the\n"
	"  corresponding value. If `name` is specified, it is defined as global\n"
	"  variable set to `value` parsed as JSON (or the literal `value` string\n"
	"  if JSON parsing fails).\n\n"

	"-F [name=]path\n"
	"  Like `-D` but reading the value from the file in `path`. The given\n"
	"  file must contain a single, well-formed JSON dictionary.\n\n"

	"-U name\n"
	"  Undefine the given global variable name.\n\n"

	"-l [name=]library\n"
	"  Preload the given `library`, optionally aliased to `name`.\n\n"

	"-L pattern\n"
	"  Prepend given `pattern` to default library search paths. If the pattern\n"
	"  contains no `*`, it is added twice, once with `/*.so` and once with\n"
	"  `/*.uc` appended to it.\n\n"

	"-c[flag,flag,...]\n"
	"  Compile the given source file(s) to bytecode instead of executing them.\n"
	"  Supported flags: no-interp (omit interpreter line), interp=... (over-\n"
	"  ride interpreter line with ...), dynlink=... (force import from ... to\n"
	"  be treated as shared extensions loaded at runtime), module (build load-\n"
	"  able library), llvm (write LLVM IR of a native program, if built with\n"
	"  LLVM support; the IR must be compiled for the CPU and float ABI that\n"
	"  NATIVE_TARGET_FLAGS of the runtime name, as clang with these flags\n"
	"  does), target=... (LLVM target triple of the native program),\n"
	"  embed=[path=]file (also embed `file` or the scripts below it, which the\n"
	"  program loads as `path` by computed names), embed-template=[path=]file\n"
	"  (the same for templates), debug (embed debug information and source\n"
	"  text for complete stack traces), map=... (write the program with debug\n"
	"  information to ..., for ucode-trace to resolve its stack traces).\n\n"

	"-o path\n"
	"  Output file path when compiling. If omitted, the compiled byte code\n"
	"  is written to `./uc.out`. Only meaningful in conjunction with `-c`.\n\n"

	"-s\n"
	"  Omit (strip) debug information when compiling files.\n"
	"  Only meaningful in conjunction with `-c`.\n\n"

	"-x[expr]\n"
	"  Start program in interactive debugger. If given, stop at the location\n"
	"  described by `expr` (a function name, `path:line[:offset]`, or a ucode\n"
	"  expression evaluating to a function - the same grammar the `break`\n"
	"  debugger CLI command accepts) instead of the first instruction.\n\n"
	"-X[expr]\n"
	"  Enable debugger infrastructure (SIGUSR1 break, uloop) without\n"
	"  launching the interactive debugger automatically. If given, `expr` is\n"
	"  resolved the same way as for `-x` and a breakpoint is installed at\n"
	"  that location; once hit, execution pauses and waits for a remote\n"
	"  debugger to attach, the same way the SIGUSR1 break does.\n\n",
		app);
}

static uc_value_t *
debug_lookup_fn(uc_vm_t *vm, const char *name)
{
	uc_value_t *dbgmod = ucv_object_get(uc_vm_scope_get(vm), "debug", NULL);
	uc_value_t *fn = ucv_object_get(dbgmod, name, NULL);

	if (ucv_type(fn) != UC_CFUNCTION) {
		fprintf(stderr, "Unable to locate debug.%s() function\n", name);

		return NULL;
	}

	return fn;
}

typedef enum {
	DEBUG_MODE_NONE,   /* neither -x nor -X given */
	DEBUG_MODE_LOCAL,  /* -x: launch local interactive debugger */
	DEBUG_MODE_REMOTE, /* -X: enable break infrastructure for a remote debugger */
} debug_mode_t;

typedef struct {
	bool strip;
	char *interpreter;
	bool autoprint;
	debug_mode_t debug;
	char *breakpoint;
	bool llvm;
	uc_llvm_options_t *native;
} compile_opts_t;

static int
write_llvm(uc_vm_t *vm, uc_program_t *program, FILE *out, const compile_opts_t *opts)
{
#ifdef HAVE_LLVM_SUPPORT
	uc_parse_config_t *config = vm->config;
	char *err = NULL;
	bool ok;

	opts->native->config = config;

	ok = uc_program_write_llvm(program, out, opts->native, &err);

	if (!ok) {
		fprintf(stderr, "%s", err ? err : "Unable to write LLVM IR\n");
		free(err);
	}

	return ok ? 0 : -1;
#else
	fprintf(stderr, "This ucode was built without LLVM support\n");

	return -1;
#endif
}

static int
compile(uc_vm_t *vm, uc_source_t *src, FILE *precompile, const compile_opts_t *opts)
{
	uc_value_t *res = NULL;
	bool strip = opts->strip, autoprint = opts->autoprint;
	debug_mode_t debug = opts->debug;
	char *interp = opts->interpreter, *breakpoint = opts->breakpoint;
	uc_program_t *program;
	int rc = 0;
	char *err;

	program = uc_compile(vm->config, src, &err);

	if (!program) {
		fprintf(stderr, "%s", err);
		free(err);
		rc = -1;
		goto out;
	}

	if (precompile && opts->llvm) {
		rc = write_llvm(vm, program, precompile, opts);
		fclose(precompile);
		goto out;
	}

	if (precompile) {
		if (interp)
			fprintf(precompile, "#!%s\n", interp);

		uc_program_write(program, precompile, !strip);
		fclose(precompile);
		goto out;
	}

	if (vm->gc_interval)
		uc_vm_gc_start(vm, vm->gc_interval);

	if (debug != DEBUG_MODE_NONE) {
		uc_value_t *entryfn;

		if (!uc_cli_library_load("debug", vm)) {
			fprintf(stderr, "Unable to load debug module\n");
			rc = -2;
			goto out;
		}

		entryfn = uc_program_main(vm, program);

		/* -x: launch local debugger, breaking at `breakpoint` if given,
		 * else at the first instruction.
		 * -X: arm attach mode (wait for a remote debugger on the PID-derived
		 * socket). With `breakpoint` given, break at that location; without
		 * one, break at the first instruction of the entry function - the
		 * remote mirror of `-x` with no argument. Either way this is the
		 * remote equivalent of the local `-x` flow above. */
		if (debug == DEBUG_MODE_LOCAL) {
			uc_value_t *dbgfn = debug_lookup_fn(vm, "debugger");

			if (!dbgfn) {
				ucv_put(entryfn);
				rc = -2;
				goto out;
			}

			uc_vm_stack_push(vm, ucv_get(dbgfn));
			uc_vm_stack_push(vm, breakpoint ? NULL : ucv_get(entryfn));

			if (uc_vm_call(vm, false, 1) == EXCEPTION_NONE)
				ucv_put(uc_vm_stack_pop(vm));
		}
		else {
			/* -X: arm attach mode. Pass entryfn so that, when no explicit
			 * `breakpoint` is given, debug.attach() installs its step
			 * breakpoint at the entry function's first instruction (breaking
			 * at the start of main, mirroring `-x` with no argument). When a
			 * `breakpoint` is given, the explicit breakpoint below is what
			 * pauses first; the entry step breakpoint is then simply
			 * superseded. debug.attach() ignores a non-closure arg, so
			 * passing NULL in that case is harmless. */
			uc_value_t *attachfn = debug_lookup_fn(vm, "attach");

			if (!attachfn) {
				ucv_put(entryfn);
				rc = -2;
				goto out;
			}

			uc_vm_stack_push(vm, ucv_get(attachfn));
			uc_vm_stack_push(vm, breakpoint ? NULL : ucv_get(entryfn));

			if (uc_vm_call(vm, false, 1) == EXCEPTION_NONE)
				ucv_put(uc_vm_stack_pop(vm));
		}

		if (breakpoint) {
			uc_value_t *bkfn = debug_lookup_fn(vm, "breakpoint");
			uc_value_t *id;

			if (!bkfn) {
				ucv_put(entryfn);
				rc = -2;
				goto out;
			}

			uc_vm_stack_push(vm, ucv_get(bkfn));
			uc_vm_stack_push(vm, ucv_string_new(breakpoint));
			uc_vm_stack_push(vm, ucv_get(entryfn));

			if (uc_vm_call(vm, false, 2) == EXCEPTION_NONE) {
				id = uc_vm_stack_pop(vm);

				if (!ucv_is_truish(id)) {
					fprintf(stderr,
						"Unable to resolve breakpoint location '%s'\n",
						breakpoint);
					ucv_put(id);
					ucv_put(entryfn);
					rc = -2;
					goto out;
				}

				ucv_put(id);
			}
		}

		ucv_put(entryfn);
	}

	uc_vm_status_t status = uc_vm_execute(vm, program, &res);

	rc = status;

	switch (rc) {
	case STATUS_OK:
		if (autoprint) {
			if (ucv_type(res) == UC_STRING) {
				fwrite(ucv_string_get(res), ucv_string_length(res), 1, stdout);
			}
			else {
				uc_stringbuf_t *pb = uc_stringbuf_new();

				ucv_to_stringbuf_formatted(vm, pb, res, 0, '\t', 1);
				fwrite(pb->buf, pb->bpos, 1, stdout);
				uc_stringbuf_free(pb);
			}
		}

		rc = 0;
		break;

	case STATUS_EXIT:
		rc = (int)ucv_int64_get(res);
		break;

	case STATUS_BREAK:
		/* Break requested - in remote debug mode, continue running */
		if (debug == DEBUG_MODE_REMOTE)
			rc = 0;
		else
			rc = -2;
		break;

	case ERROR_COMPILE:
		rc = -1;
		break;

	case ERROR_RUNTIME:
		rc = -2;
		break;
	}

	/* Let an attached remote debugger client know the target is going away
	 * and why, instead of it only finding out once the connection drops.
	 * Pass the raw VM status rather than the CLI's own exit-code
	 * translation above (which flattens both error kinds to the same -2
	 * and loses the actual exception), plus the exit code / a full
	 * exception object (same {type, message, stacktrace} shape script
	 * code sees via try/catch) it corresponds to.
	 *
	 * These have to be snapshotted into locals *before* the uc_vm_call()
	 * below, not read from vm->exception/vm->arg by the callee once
	 * inside it: uc_vm_call() unconditionally calls
	 * uc_vm_clear_exception() as its very first action (a normal safety
	 * reset for ordinary calls, which also frees vm->exception.message/
	 * ->stacktrace), which would wipe vm->exception out from under us
	 * before debug.notifyExit() ever got to look at it.
	 *
	 * vm->output (stdout) is fully block-buffered once it's a socket
	 * rather than a tty, while the notification itself goes out via a raw
	 * write() - flush first, or the event can overtake not-yet-flushed
	 * script output that was already written earlier. */
	if (debug != DEBUG_MODE_NONE) {
		uc_value_t *notifyfn = debug_lookup_fn(vm, "notifyExit");
		int32_t exit_code = vm->arg.s32;
		uc_value_t *exception_obj = (status == ERROR_COMPILE || status == ERROR_RUNTIME)
			? uc_vm_exception_object(vm) : NULL;

		fflush(vm->output);

		if (notifyfn) {
			uc_vm_stack_push(vm, ucv_get(notifyfn));
			uc_vm_stack_push(vm, ucv_int64_new(status));
			uc_vm_stack_push(vm, ucv_int64_new(exit_code));
			uc_vm_stack_push(vm, exception_obj);

			if (uc_vm_call(vm, false, 3) == EXCEPTION_NONE)
				ucv_put(uc_vm_stack_pop(vm));
		}
		else {
			ucv_put(exception_obj);
		}
	}

out:
	uc_program_put(program);
	ucv_put(res);

	return rc;
}

static uc_source_t *
read_stdin(void)
{
	FILE *fp = stdin_unused;

	if (!fp) {
		fprintf(stderr, "The stdin can only be read once\n");
		errno = EINVAL;

		return NULL;
	}

	stdin_unused = NULL;

	return uc_source_new_slurp("[stdin]", fp);
}

static void
parse_template_modeflags(char *opt, uc_parse_config_t *config)
{
	char *p;

	if (!opt)
		return;

	for (p = strtok(opt, ", "); p; p = strtok(NULL, ", ")) {
		if (!strcmp(p, "no-lstrip"))
			config->lstrip_blocks = false;
		else if (!strcmp(p, "no-rtrim"))
			config->trim_blocks = false;
		else
			fprintf(stderr, "Unrecognized -T flag \"%s\", ignoring\n", p);
	}
}

static void
parse_compile_flags(char *opt, char **interp, uc_search_path_t *dynlink_list,
                    bool *module, bool *llvm, uc_llvm_options_t *native)
{
	char *p, *k, *v;

	if (!opt)
		return;

	for (p = strtok(opt, ","); p; p = strtok(NULL, ",")) {
		k = p;
		v = strchr(p, '=');

		if (v)
			*v++ = 0;

		if (!strcmp(k, "no-interp")) {
			if (v)
				fprintf(stderr, "Compile flag \"%s\" takes no value, ignoring\n", k);

			*interp = NULL;
		}
		else if (!strcmp(k, "interp")) {
			if (!v)
				fprintf(stderr, "Compile flag \"%s\" requires a value, ignoring\n", k);
			else
				*interp = v;
		}
		else if (!strcmp(k, "dynlink")) {
			if (!v)
				fprintf(stderr, "Compile flag \"%s\" requires a value, ignoring\n", k);
			else
				uc_vector_push(dynlink_list, v);
		}
		else if (!strcmp(k, "module")) {
			if (v)
				fprintf(stderr, "Compile flag \"%s\" takes no value, ignoring\n", k);

			*module = true;
		}
		else if (!strcmp(k, "llvm")) {
			if (v)
				fprintf(stderr, "Compile flag \"%s\" takes no value, ignoring\n", k);

			*llvm = true;
		}
		else if (!strcmp(k, "target")) {
			if (!v)
				fprintf(stderr, "Compile flag \"%s\" requires a value, ignoring\n", k);
			else
				native->triple = v;
		}
		else if (!strcmp(k, "debug")) {
			if (v)
				fprintf(stderr, "Compile flag \"%s\" takes no value, ignoring\n", k);

			native->debug = true;
		}
		else if (!strcmp(k, "map")) {
			if (!v)
				fprintf(stderr, "Compile flag \"%s\" requires a value, ignoring\n", k);
			else
				native->map = v;
		}
		else if (!strcmp(k, "embed") || !strcmp(k, "embed-template")) {
			if (!v) {
				fprintf(stderr, "Compile flag \"%s\" requires a value, ignoring\n", k);
				continue;
			}

			/* [logical=]path */
			char *path = strchr(v, '=');

			if (path)
				*path++ = 0;

			uc_vector_push(native->embeds, {
				.path = path ? path : v,
				.logical = path ? v : NULL,
				.template = !strcmp(k, "embed-template")
			});
		}
		else {
			fprintf(stderr, "Unrecognized -c flag \"%s\", ignoring\n", k);
		}
	}
}

static const char *
appname(const char *argv0)
{
	const char *p;

	if (!argv0)
		return "ucode";

	p = strrchr(argv0, '/');

	if (p)
		return p + 1;

	return argv0;
}

int
main(int argc, char **argv)
{
	const char *optspec = POSIXLY_CORRECT_FLAG "he:p:tg:ST::RD:F:U:l:L:c::o:sx::X::";
	bool strip = false, print_result = false, llvm = false;
	uc_llvm_embeds_t embeds = { 0 };
	uc_llvm_options_t native = { .embeds = &embeds };
	debug_mode_t debug = DEBUG_MODE_NONE;
	char *breakpoint = NULL;
	char *interp = "/usr/bin/env ucode";
	uc_source_t *source = NULL;
	FILE *precompile = NULL;
	char *outfile = NULL;
	uc_vm_t vm = { 0 };
	const char *argv0;
	int opt, rv = 0;
	const char *app;
	uc_value_t *o;
	int fd;

	uc_parse_config_t config = {
		.strict_declarations = false,
		.lstrip_blocks = true,
		.trim_blocks = true,
		.raw_mode = true,
		.setup_signal_handlers = true
	};

	app = appname(argv[0]);

	if (argc == 1) {
		print_usage(app);
		goto out;
	}

	if (!strcmp(app, "utpl"))
		config.raw_mode = false;
	else if (!strcmp(app, "ucc"))
		outfile = "./uc.out";

	stdin_unused = stdin;

	/* parse options iteration 1: parse config related options */
	while ((opt = getopt(argc, argv, optspec)) != -1)
	{
		switch (opt) {
		case 'L':
			uc_cli_search_path(optarg, &config);
			break;

		case 'S':
			config.strict_declarations = true;
			break;

		case 'R':
			config.raw_mode = true;
			break;

		case 'T':
			config.raw_mode = false;
			parse_template_modeflags(optarg, &config);
			break;
		}
	}

	uc_search_path_init(&config.module_search_path);

	argv0 = argv[optind];
	native.script_name = argv0;
	optind = 1;

	uc_vm_init(&vm, &config);

	/* load std functions into global scope */
	uc_stdlib_load(uc_vm_scope_get(&vm));

	/* register ARGV array but populate it later (to allow for -U ARGV) */
	o = ucv_array_new(&vm);

	ucv_object_add(uc_vm_scope_get(&vm), "ARGV", ucv_get(o));
	if (argv0)
		ucv_object_add(uc_vm_scope_get(&vm), "SCRIPT_NAME", ucv_string_new(argv0));

	/* parse options iteration 2: process remaining options */
	while ((opt = getopt(argc, argv, optspec)) != -1)
	{
		switch (opt) {
		case 'h':
			print_usage(argv[0]);
			goto out;

		case 'e':
			source = uc_source_new_buffer("[-e argument]", xstrdup(optarg), strlen(optarg));
			break;

		case 'p':
			source = uc_source_new_buffer("[-p argument]", xstrdup(optarg), strlen(optarg));
			print_result = true;
			break;

		case 't':
			uc_vm_trace_set(&vm, 1);
			break;

		case 'g':
			vm.gc_interval = atoi(optarg);
			break;

		case 'D':
			if (!uc_cli_define_string(optarg, uc_vm_scope_get(&vm))) {
				rv = 1;
				goto out;
			}

			break;

		case 'F':
			if (!uc_cli_define_file(optarg, uc_vm_scope_get(&vm), &stdin_unused)) {
				rv = 1;
				goto out;
			}

			break;

		case 'U':
			ucv_object_delete(uc_vm_scope_get(&vm), optarg);
			break;

		case 'l':
			if (!uc_cli_library_load(optarg, &vm)) {
				rv = 1;
				goto out;
			}

			break;

		case 'c':
			outfile = "./uc.out";
			parse_compile_flags(optarg, &interp, &config.force_dynlink_list,
			                    &config.compile_module, &llvm, &native);
			break;

		case 's':
			strip = true;
			break;

		case 'o':
			outfile = optarg;
			break;

		case 'x':
			debug = DEBUG_MODE_LOCAL;
			breakpoint = optarg;
			break;

		case 'X':
			debug = DEBUG_MODE_REMOTE;
			breakpoint = optarg;
			break;
		}
	}

	if (!source && argv[optind] != NULL) {
		if (!strcmp(argv[optind], "-"))
			source = read_stdin();
		else
			source = uc_source_new_file(argv[optind]);

		if (!source) {
			fprintf(stderr, "Failed to open \"%s\": %s\n", argv[optind], strerror(errno));
			rv = 1;
			goto out;
		}

		optind++;
	}

	if (!source) {
		fprintf(stderr, "Require either -e/-p expression or source file\n");
		rv = 1;
		goto out;
	}

	if (outfile) {
		if (!strcmp(outfile, "-")) {
			precompile = stdout;
		}
		else {
			fd = open(outfile, O_WRONLY|O_CREAT|O_TRUNC, 0777);

			if (fd == -1) {
				fprintf(stderr, "Unable to open output file %s: %s\n",
				        outfile, strerror(errno));

				rv = 1;
				goto out;
			}

			precompile = fdopen(fd, "wb");
		}
	}

	/* populate ARGV array */
	for (; optind < argc; optind++)
		ucv_array_push(o, ucv_string_new(argv[optind]));

	ucv_put(o);

	rv = compile(&vm, source, precompile, &((compile_opts_t){
		.strip = strip,
		.interpreter = interp,
		.autoprint = print_result,
		.debug = debug,
		.breakpoint = breakpoint,
		.llvm = llvm,
		.native = &native,
	}));

out:
	uc_search_path_free(&config.module_search_path);
	uc_vector_clear(&config.force_dynlink_list);
	uc_vector_clear(&embeds);

	uc_source_put(source);

	uc_vm_free(&vm);

	return rv;
}
