The `ucode` interpreter executes programs from source files, expressions or
standard input. The first file argument is the program, every further argument
is passed to it as data; `ucode -h` prints the full help text.

## Options

| Option | Effect |
|---|---|
| `-h` | display help and exit |
| `-e expr` | execute the expression `expr` |
| `-p expr` | execute `expr` and print the result afterwards |
| `-R` | process the input as plain script code (the default) |
| `-T[flag,...]` | process the input as a template; `no-lstrip` keeps leading whitespace before block tags, `no-rtrim` keeps the trailing newline after them |
| `-D [name=]value` | define a global variable; without `name`, `value` must be a JSON dictionary whose properties become globals |
| `-F [name=]path` | like `-D`, but the value is read from the JSON file in `path` |
| `-U name` | remove the global variable `name` |
| `-l [name=]lib` | preload the module `lib`, optionally aliased to `name` |
| `-L pattern` | prepend `pattern` to the module search path; a pattern without `*` is added twice, once with `/*.so` and once with `/*.uc` appended |
| `-c[flag,...]` | compile to bytecode instead of executing; flags: `no-interp`, `interp=...`, `dynlink=...`, `module` |
| `-o path` | output file for `-c` (default `./uc.out`) |
| `-s` | omit debug information from compiled bytecode |
| `-t` | trace the VM execution to standard error |
| `-g interval` | run the cycle collector every `interval` object allocations |
| `-S` | enable strict mode |
| `-x[expr]` | start the interactive debugger, optionally breaking at `expr` |
| `-X[expr]` | enable the debugger for later remote attach, optionally with a breakpoint at `expr` |

## Examples

```console
$ ucode -p "2 ** 10"
1024

$ ucode program.uc

$ ucode -c -o program.out program.uc

$ ucode -T -D name=lan -D up=true iface.tpl
```

Preloading a module makes it available under its name (or the given alias):

```console
$ ucode -lfs -e 'printf("%J\n", fs.access("/etc"))'
true
```

## Script arguments

Extra command line parameters are passed to the script in the `ARGV` global
(an array of strings); `SCRIPT_NAME` holds the path the script was invoked
with:

```console
$ ucode foo.uc foo bar
$ ./foo.uc foo bar          # when the script carries a shebang line
```

```ucode
print(ARGV, "\n");          // [ "foo", "bar" ]
print(SCRIPT_NAME, "\n");   // ./foo.uc
```
