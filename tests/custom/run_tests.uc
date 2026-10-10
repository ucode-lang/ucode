#!/usr/bin/env -S ucode -S

import * as fs from 'fs';

let testdir = sourcepath(0, true);
let topdir = fs.realpath(`${testdir}/../..`);

let line = '........................................';
let ucode_bin = getenv('UCODE_BIN') || `${topdir}/build/ucode`;
let ucode_lib = getenv('UCODE_LIB') || `${topdir}/build`;
let platform = trim(fs.popen('uname -s', 'r').read('all'));

function mkdir_p(path) {
	let parts = split(rtrim(path, '/') || '/', /\/+/);
	let current = '';

	for (let part in parts) {
		current += part + '/';

		let s = fs.stat(current);

		if (s == null) {
			if (!fs.mkdir(current))
				die(`Error creating directory '${current}': ${fs.error()}`);
		}
		else if (s.type != 'directory') {
			die(`Path '${current}' exists but is not a directory`);
		}
	}
}

function shellquote(s) {
	return `'${replace(s, "'", "'\\''")}'`;
}

function getpid() {
	return +fs.popen('echo $PPID', 'r').read('all');
}

function has_expectations(testcase)
{
	return (testcase?.stdout != null || testcase?.stderr != null || testcase?.exitcode != null);
}

function parse_testcases(file, dir) {
	let fp = fs.open(file, 'r') ?? die(`Unable to open ${file}: ${fs.error()}`);
	let testcases, testcase, section, m;
	let code_first = false;

	for (let line = fp.read('line'); length(line); line = fp.read('line')) {
		if (line == '-- Args --\n') {
			section = [ 'args', [] ];
		}
		else if (line == '-- Platform --\n') {
			section = [ 'platform', [] ];
		}
		else if (line == '-- Vars --\n') {
			section = [ 'env', {} ];
		}
		else if (line == '-- Testcase --\n') {
			section = [ 'code', '' ];
		}
		else if ((m = match(line, /^-- Expect (stdout|stderr|exitcode) --$/)) != null) {
			section = [ m[1], '' ];
		}
		else if ((m = match(line, /^-- File (.*)--$/)) != null) {
			section = [ 'file', `${dir}/files/${trim(m[1]) || 'file'}`, '' ];
		}
		else if ((m = match(line, /^-- End( \(no-eol\))? --$/)) != null) {
			if (m[1] != null && type(section[-1]) == 'string')
				section[-1] = substr(section[-1], 0, -1);

			if (section[0] == 'code') {
				if (testcases == null && !has_expectations(testcase))
					code_first = true;

				if (code_first) {
					if (testcase?.code != null) {
						push(testcases ??= [], testcase);
						testcase = null;
					}

					(testcase ??= {}).code = section[1];
				}
				else {
					push(testcases ??= [], { ...testcase, code: section[1] });
					testcase = null;
				}
			}
			else if (section[0] == 'file') {
				((testcase ??= {}).files ??= {})[section[1]] = section[2];
			}
			else {
				(testcase ??= {})[section[0]] = section[1];
			}

			section = null;
		}
		else if (section) {
			switch (section[0]) {
			case 'args':
			case 'platform':
				if ((m = trim(line)) != '')
					push(section[1], ...split(m, /[ \t\r\n]+/));
				break;

			case 'env':
				if ((m = match(line, /^([A-Za-z_][A-Za-z0-9_]*)=(.*)$/)) != null)
					section[1][m[1]] = m[2];
				break;

			default:
				section[-1] += line;
				break;
			}
		}
	}

	if (code_first && testcase.code != null && has_expectations(testcase))
		push(testcases ??= [], testcase);

	return testcases;
}

function diff(tag, ...ab) {
	let cmd = [ 'diff', '-au', '--color=always', `--label=Expected ${tag}`, `--label=Resulting ${tag}` ];
	let tmpfiles = [];

	for (let i, f in ab) {
		if (type(f) != 'resource') {
			push(tmpfiles, fs.mkstemp());
			tmpfiles[-1].write(f);
			f = tmpfiles[-1];
		}

		f.seek(0);
		push(cmd, `/dev/fd/${f.fileno()}`);
	}

	system(cmd);
}

function run_testcase(num, dir, testcase) {
	let fout = fs.mkstemp(`${dir}/stdout.XXXXXX`);
	let ferr = fs.mkstemp(`${dir}/stderr.XXXXXX`);

	let eout = testcase.stdout ?? '';
	let eerr = testcase.stderr ?? '';
	let ecode = testcase.exitcode ? +testcase.exitcode : null;

	let cmd = join(' ', [
		...map(keys(testcase.env) ?? [], k => `export ${k}=${shellquote(testcase.env[k])};`),
		`cd ${shellquote(dir)};`,
		`exec ${ucode_bin}`,
		`-T','`,
		`-L ${shellquote(`${ucode_lib}/*.so`)}`,
		`-D TESTFILES_PATH=${shellquote(`${fs.realpath(dir)}/files`)}`,
		`-D UCODE_BIN=${shellquote(`${ucode_bin}`)}`,
		`${join(' ', map(testcase.args ?? [], shellquote))} -`,
		`>/dev/fd/${fout.fileno()} 2>/dev/fd/${ferr.fileno()}`
	]);

	let proc = fs.popen(cmd, 'w') ?? die(`Error launching test command "${cmd}": ${fs.error()}\n`);

	if (testcase.code != null)
		proc.write(testcase.code);

	let exitcode = proc.close();

	if (exitcode == 77 && ecode != 77)
		return null;

	fout.seek(0);
	ferr.seek(0);

	let ok = true;

	if (replace(ferr.read('all'), dir, '.') != eerr) {
		if (ok) print('!\n');
		printf("Testcase #%d: Expected stderr did not match:\n", num);
		diff('stderr', eerr, ferr);
		print("---\n");
		ok = false;
	}

	if (replace(fout.read('all'), dir, '.') != eout) {
		if (ok) print('!\n');
		printf("Testcase #%d: Expected stdout did not match:\n", num);
		diff('stdout', eout, fout);
		print("---\n");
		ok = false;
	}

	if (ecode != null && exitcode != ecode) {
		if (ok) print('!\n');
		printf("Testcase #%d: Expected exit code did not match:\n", num);
		diff('code', `${ecode}\n`, `${exitcode}\n`);
		print("---\n");
		ok = false;
	}

	return ok;
}

function run_test(file) {
	let name = fs.basename(file);
	printf('%s %s ', name, substr(line, length(name)));

	let tmpdir = sprintf('/tmp/test.%d', getpid());
	let testcases = parse_testcases(file, tmpdir);
	let failed = 0, skipped = 0;

	fs.mkdir(tmpdir);

	try {
		for (let i, testcase in testcases) {
			if (testcase.platform && index(testcase.platform, platform) < 0) {
				skipped++;
				continue;
			}

			for (let path, data in testcase.files) {
				mkdir_p(fs.dirname(path));
				fs.writefile(path, data) ?? die(`Error writing testcase file "${path}": ${fs.error()}\n`);
			}

			let ok = run_testcase(i + 1, tmpdir, testcase);

			if (ok == null)
				skipped++;
			else
				failed += !ok;
		}
	}
	catch (e) {
		warn(`${e.type}: ${e.message}\n${e.stacktrace[0].context}\n`);
	}

	system(['rm', '-r', tmpdir]);

	if (failed == 0 && skipped > 0)
		printf('OK (%d/%d skipped)\n', skipped, length(testcases));
	else if (failed == 0)
		print('OK\n');
	else
		printf('%s %s FAILED (%d/%d)\n', name, substr(line, length(name)), failed, length(testcases));

	return failed;
}

let n_tests = 0;
let n_fails = 0;
let select_tests = filter(map(ARGV, p => fs.realpath(p)), length);

function use_test(input) {
	return fs.access(input = fs.realpath(input)) &&
		(!length(select_tests) || filter(select_tests, p => p == input)[0]);
}

function lib_missing(catdir) {
	let m = match(fs.basename(catdir), /^([0-9][0-9])_lib_(.+)$/);

	if (m == null)
		return false;

	for (let ext in ['so', 'dylib', 'dll', 'uc'])
		if (fs.access(`${ucode_lib}/${m[2]}.${ext}`))
			return false;

	return true;
}

for (let catdir in fs.glob(`${testdir}/[0-9][0-9]_*`)) {
	if (fs.stat(catdir)?.type != 'directory')
		continue;

	if (lib_missing(catdir)) {
		printf('\n##\n## Skipping %s tests (no library found)\n##\n\n', substr(fs.basename(catdir), 3));
		continue;
	}

	printf('\n##\n## Running %s tests\n##\n\n', substr(fs.basename(catdir), 3));

	for (let testfile in fs.glob(`${catdir}/[0-9][0-9]_*`)) {
		if (!use_test(testfile)) continue;

		n_tests++;
		n_fails += run_test(testfile);
	}
}

printf('\nRan %d tests, %d okay, %d failures\n', n_tests, n_tests - n_fails, n_fails);
exit(n_fails);
