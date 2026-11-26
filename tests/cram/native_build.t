setup common environment:

  $ [ -n "$BUILD_BIN_DIR" ] && export PATH="$BUILD_BIN_DIR:$PATH"
  $ native() {
  >   out=$1; flags=$2; shift 2
  >   "$BUILD_BIN_DIR/ucode" -c,llvm$flags -o "$out.ll" "$@" &&
  >   "$LLVM_LINK" "$out.ll" "$BUILD_BIN_DIR/libucode-native.bc" -o "$out.all.bc" &&
  >   "$OPT" -passes='internalize,default<O1>' \
  >     -internalize-public-api-file="$BUILD_BIN_DIR/libucode-native.api" \
  >     "$out.all.bc" -o "$out.bc" &&
  >   "$LLC" -O1 -relocation-model=pic -filetype=obj "$out.bc" -o "$out.o" &&
  >   "$CC" "$out.o" -lm -ldl -rdynamic -Wl,-rpath,"${RUNPATH-$BUILD_BIN_DIR/native-abi}" -o "$out"
  > }

a native program behaves as the interpreter, also when it fails:

  $ cat > hello.uc <<'EOF'
  > function fib(n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }
  > print(fib(15), " ", ARGV, " ", defined, "\n");
  > null.x;
  > EOF

  $ native hello ",debug" hello.uc
  $ ./hello --ucode-define defined=42 -- -D x
  Reference error: left-hand side expression is null
  In hello.uc, line 3, byte 6:
  
   `null.x;`
    ~~~~~^~
  
  
  610 [ "-D", "x" ] 42
  [254]

it contains the interpreter and the bytecode loader for the scripts that it
does not contain, but not the compiler:

  $ nm hello | grep -E ' (uc_compile_from_source|uc_lexer_init)$'
  [1]

without the debug flag, a native program carries no debug information and
its stack traces name functions and instruction offsets. The map file
written at build time resolves them later from a log:

  $ native small ",map=hello.map" hello.uc
  $ ./small
  Reference error: left-hand side expression is null
  In function #1 at offset 63 of hello.uc
  
  
  610 [ ] 
  [254]
  $ ./small > small.log 2>&1
  [254]
  $ "$BUILD_BIN_DIR/ucode-trace" hello.map small.log
  Reference error: left-hand side expression is null
  In hello.uc, line 3, byte 6:
  
   `null.x;`
    ~~~~~^~
  
  
  610 [ ] 
  $ printf 'In function foo [C]\n' | "$BUILD_BIN_DIR/ucode-trace" hello.map
  In function foo [C]

the scripts a program loads with literal names are embedded, so that it
runs without its source files:

  $ mkdir -p app/lib app/inc
  $ cat > app/main.uc <<'EOF'
  > let lib = require("lib.counter");
  > print(lib.next(), " ", lib.next(), " ", lib === require("lib.counter"), "\n");
  > include("inc/part.uc", { n: 7 });
  > print(render("inc/page.ut", { items: [1, 2] }), "\n");
  > print(sourcepath(), "\n");
  > EOF
  $ echo 'let n = 0; return { next: () => ++n };' > app/lib/counter.uc
  $ echo 'print("part ", n, "\n"); include("leaf.uc");' > app/inc/part.uc
  $ echo 'print("leaf\n");' > app/inc/leaf.uc
  $ printf '{%% for (let i in items): %%}[{{ i }}]{%% endfor %%}' > app/inc/page.ut

  $ cd app && native ../bundled "" -L "$PWD/*.uc" main.uc && cd ..
  $ mv app app.moved && mkdir app && cd app
  $ ../bundled --ucode-search-path "$PWD/*.uc"
  1 2 true
  part 7
  leaf
  [1][2]
  /.*/app/main.uc (re)
  $ cd .. && rm -r app && mv app.moved app

scripts loaded by computed names are embedded with compile flags, under
the path the program loads them by:

  $ cat > plugins.uc <<'EOF'
  > for (let path in sort(bundle_files()))
  >   if (wildcard(path, "/usr/share/demo/*.uc"))
  >     include(path);
  > let name = "plug" + "in";
  > include(`/usr/share/demo/${name}.uc`);
  > EOF
  $ mkdir demo && echo 'print("plugin\n"); include("helper.uc");' > demo/plugin.uc
  $ echo 'print("helper\n");' > demo/helper.uc

  $ native plugins ",embed=/usr/share/demo=$PWD/demo" plugins.uc
  $ rm -r demo && ./plugins
  helper
  plugin
  helper
  plugin
  helper

a script that fails to compile fails to load as in the interpreter, and
source code that the program compiles at runtime is compiled by the ucode
CLI, which PATH finds, and runs in the interpreter:

  $ echo 'let = 1;' > broken.uc
  $ echo 'try { include("broken.uc"); } catch (e) { print(e.message, "\n"); }' > load.uc
  $ native load "" load.uc && ./load
  Unable to compile source file '/.*/broken.uc': (re)
  
    | Syntax error: Expecting variable name
    | In line 1, byte 5:
    |
    |  `let = 1;`
    |       ^-- Near here
  
  $ echo 'print(loadstring("return 6 * 7;")(), "\n");' > dynamic.uc
  $ native dynamic "" dynamic.uc && ./dynamic
  42

a binding that depends on libucode gets the library in the native-abi
directory, which the program finds through its RUNPATH and which contains
no interpreter. Without it, the program warns, as the binding loads the
interpreter library:

  $ cat > binding.uc <<'EOF'
  > require("debug");
  > let maps = split(require("fs").readfile("/proc/self/maps"), "\n");
  > let libs = uniq(map(filter(maps, l => index(l, "libucode.so") >= 0), l => match(l, /[^ ]*$/)[0]));
  > print(length(libs), " ", index(libs[0], "/native-abi/") >= 0, "\n");
  > EOF
  $ native binding "" binding.uc
  $ ./binding --ucode-search-path "$BUILD_BIN_DIR/*.so"
  1 true
  $ RUNPATH="$PWD/none" native nostub "" binding.uc
  $ ./nostub --ucode-search-path "$BUILD_BIN_DIR/*.so"
  libucode.so.[0-9]+ not found, bindings that depend on it load the interpreter library (re)
  1 false

the dynlink flag defers imports to the run time as in the interpreter; the
program embeds the modules and runs without their source files. dyn()
compares the output with that of the interpreter, which runs bytecode with
the same flag, as the interpreter runs no source with it:

  $ dyn() {
  >   flags=; for n in $1; do flags="$flags,dynlink=$n"; done
  >   native ./native "$flags" -L "$PWD/*.uc" main.uc &&
  >   if [ -f loader.uc ]; then
  >     "$BUILD_BIN_DIR/ucode" -L "$PWD/*.uc" -c,no-interp$flags -o loader.ucb loader.uc &&
  >     mv loader.ucb loader.uc
  >   fi &&
  >   "$BUILD_BIN_DIR/ucode" -L "$PWD/*.uc" -c,no-interp$flags -o ref.ucb main.uc &&
  >   "$BUILD_BIN_DIR/ucode" -L "$PWD/*.uc" ref.ucb > ref.out &&
  >   mkdir hidden && mv *.uc hidden/ &&
  >   ./native --ucode-search-path "$PWD/*.uc" > native.out &&
  >   cmp ref.out native.out && cat native.out && echo
  > }
  $ loader='import * as ns from "dep";return ns;'

named imports are live:

  $ mkdir dyn1 && cd dyn1
  $ printf '%s' 'import {n,inc} from "dep";print(n," ",inc()," ",n);' > main.uc
  $ printf '%s' 'export let n=1;export function inc(){gc();return ++n;}' > dep.uc
  $ dyn dep
  1 2 2
  $ cd ..

a default, a namespace and a static import of the module:

  $ mkdir dyn2 && cd dyn2
  $ printf '%s' 'import d,* as ns from "dep";print(d(),ns.n," ",ns.inc()," ",ns.n);' > main.uc
  $ printf '%s' 'import {x} from "./base.uc";export let n=x;export function inc(){gc();return ++n;}export default ()=>n;' > dep.uc
  $ printf '%s' 'export let x=10;' > base.uc
  $ dyn dep
  1010 11 11
  $ cd ..

require() after and before an import of the same module:

  $ mkdir dyn3 && cd dyn3
  $ printf '%s' 'import * as x from "dep";print(require("dep").n," ",x.n);' > main.uc
  $ printf '%s' 'export let n=42;' > dep.uc
  $ dyn dep
  42 42
  $ cd ..
  $ mkdir dyn4 && cd dyn4
  $ printf '%s' 'let x=require("dep");import {value} from "dep";print(value," ",x.value);' > main.uc
  $ printf '%s' 'print("once ");return {value:7};' > dep.uc
  $ dyn dep
  once 7 7
  $ cd ..
  $ mkdir dyn5 && cd dyn5
  $ printf '%s' 'import * as x from "dep";print(x," ",require("dep"));' > main.uc
  $ printf '%s' 'print("once ");let value=7;' > dep.uc
  $ dyn dep
  once { } { }
  $ cd ..
  $ mkdir dyn6 && cd dyn6
  $ printf '%s' 'import * as x from "dep"; print(x);' > main.uc
  $ printf '%s' 'let value=1;' > dep.uc
  $ dyn dep
  { }
  $ cd ..

a reload keeps the cells of the first load, and a failed load is not cached:

  $ mkdir dyn7 && cd dyn7
  $ printf '%s' 'let a=require("loader");delete modules.loader;delete modules.dep;let b=require("loader");gc();print(a.inc()," ",a.n," ",b.n," ",b.inc());' > main.uc
  $ printf '%s' "$loader" > loader.uc
  $ printf '%s' 'export let n=0;export function inc(){gc();return ++n;}' > dep.uc
  $ dyn dep
  1 1 0 1
  $ cd ..
  $ mkdir dyn8 && cd dyn8
  $ printf '%s' 'global.tries=0;try{require("loader");}catch(e){print(e.message," ");}print(exists(modules,"dep")," ",require("loader").n);' > main.uc
  $ printf '%s' "$loader" > loader.uc
  $ printf '%s' 'if(++tries==1)die("retry");export let n=tries;' > dep.uc
  $ dyn dep
  retry false 2
  $ cd ..

a namespace is read only, and a module in the registry is not loaded:

  $ mkdir dyn9 && cd dyn9
  $ printf '%s' 'import * as ns from "dep";try{ns.n=4;}catch(e){print(e.type);}print(ns.n);' > main.uc
  $ printf '%s' 'export let n=3;' > dep.uc
  $ dyn dep
  Type error3
  $ cd ..
  $ mkdir dyn10 && cd dyn10
  $ printf '%s' 'modules.dep={n:8};import {n} from "dep";print(n);' > main.uc
  $ printf '%s' 'die("must not execute");export let n=0;' > dep.uc
  $ dyn dep
  8
  $ cd ..

a deferred module can defer an import of its own:

  $ mkdir dyn11 && cd dyn11
  $ printf '%s' 'import {sum} from "dep";print(sum());' > main.uc
  $ printf '%s' 'import {n,inc} from "other";export function sum(){return inc()+n;}' > dep.uc
  $ printf '%s' 'export let n=10;export function inc(){gc();return ++n;}' > other.uc
  $ dyn "dep other"
  22
  $ cd ..

errors of deferred imports:

  $ mkdir dyn12 && cd dyn12
  $ printf '%s' 'try{require("loader");}catch(e){print(e.type);}print(exists(modules,"dep"));' > main.uc
  $ printf '%s' "$loader" > loader.uc
  $ printf '%s' 'return {value:7};' > dep.uc
  $ dyn dep
  Runtime errorfalse
  $ cd ..
  $ mkdir dyn13 && cd dyn13
  $ printf '%s' 'try{require("loader");}catch(e){print(e.type);}' > main.uc
  $ printf '%s' 'import {missing} from "dep";return missing;' > loader.uc
  $ printf '%s' 'export let n=1;' > dep.uc
  $ dyn dep
  Reference error
  $ cd ..
  $ mkdir dyn14 && cd dyn14
  $ printf '%s' 'try{require("dep");}catch(e){print(e.type);}import {n} from "dep";print(n);' > main.uc
  $ printf '%s' 'export let n=42;' > dep.uc
  $ dyn dep
  Runtime error42
  $ cd ..
  $ mkdir dyn15 && cd dyn15
  $ printf '%s' 'try{require("dep");}catch(e){print(e.type);}print(require("loader").n);' > main.uc
  $ printf '%s' "$loader" > loader.uc
  $ printf '%s' 'export let n=42;' > dep.uc
  $ dyn dep
  Runtime error42
  $ cd ..
