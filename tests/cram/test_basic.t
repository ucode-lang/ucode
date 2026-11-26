setup common environment:

  $ [ -n "$BUILD_BIN_DIR" ] && export PATH="$BUILD_BIN_DIR:$PATH"
  $ alias ucode="$UCODE_BIN"

  $ for m in $BUILD_BIN_DIR/*.so; do
  >   ln -s "$m" "$(pwd)/$(basename $m)"; \
  > done

check that ucode provides exepected help:

  $ ucode | sed 's/ucode-san/ucode/'
  Usage:
    ucode -h
    ucode -e "expression"
    ucode input.uc [input2.uc ...]
    ucode -c [-s] [-o output.uc] input.uc [input2.uc ...]
  
  -h
    Help display this help.
  
  -e "expression"
    Execute the given expression as ucode program.
  
  -p "expression"
    Like `-e` but print the result of expression.
  
  -t
    Enable VM execution tracing.
  
  -g interval
    Perform periodic garbage collection every `interval` object
    allocations.
  
  -S
    Enable strict mode.
  
  -R
    Process source file(s) as raw script code (default).
  
  -T[flag,flag,...]
    Process the source file(s) as templates, not as raw script code.
    Supported flags: no-lstrip (don't strip leading whitespace before
    block tags), no-rtrim (don't strip trailing newline after block tags).
  
  -D [name=]value
    Define global variable. If `name` is omitted, a JSON dictionary is
    expected with each property becoming a global variable set to the
    corresponding value. If `name` is specified, it is defined as global
    variable set to `value` parsed as JSON (or the literal `value` string
    if JSON parsing fails).
  
  -F [name=]path
    Like `-D` but reading the value from the file in `path`. The given
    file must contain a single, well-formed JSON dictionary.
  
  -U name
    Undefine the given global variable name.
  
  -l [name=]library
    Preload the given `library`, optionally aliased to `name`.
  
  -L pattern
    Prepend given `pattern` to default library search paths. If the pattern
    contains no `*`, it is added twice, once with `/*.so` and once with
    `/*.uc` appended to it.
  
  -c[flag,flag,...]
    Compile the given source file(s) to bytecode instead of executing them.
    Supported flags: no-interp (omit interpreter line), interp=... (over-
    ride interpreter line with ...), dynlink=... (force import from ... to
    be treated as shared extensions loaded at runtime), module (build load-
    able library), llvm (write LLVM IR of a native program, if built with
    LLVM support; the IR must be compiled for the CPU and float ABI that
    NATIVE_TARGET_FLAGS of the runtime name, as clang with these flags
    does), target=... (LLVM target triple of the native program),
    embed=[path=]file (also embed `file` or the scripts below it, which the
    program loads as `path` by computed names), embed-template=[path=]file
    (the same for templates), debug (embed debug information and source
    text for complete stack traces), map=... (write the program with debug
    information to ..., for ucode-trace to resolve its stack traces).
  
  -o path
    Output file path when compiling. If omitted, the compiled byte code
    is written to `./uc.out`. Only meaningful in conjunction with `-c`.
  
  -s
    Omit (strip) debug information when compiling files.
    Only meaningful in conjunction with `-c`.
  
  -x[expr]
    Start program in interactive debugger. If given, stop at the location
    described by `expr` (a function name, `path:line[:offset]`, or a ucode
    expression evaluating to a function - the same grammar the `break`
    debugger CLI command accepts) instead of the first instruction.
  
  -X[expr]
    Enable debugger infrastructure (SIGUSR1 break, uloop) without
    launching the interactive debugger automatically. If given, `expr` is
    resolved the same way as for `-x` and a breakpoint is installed at
    that location; once hit, execution pauses and waits for a remote
    debugger to attach, the same way the SIGUSR1 break does.
  



check that ucode prints greetings:

  $ ucode -e "print('hello world')"
  hello world (no-eol)

check that ucode provides proper error messages:

  $ touch lib.uc; ucode -l lib
  Require either -e/-p expression or source file
  [1]

  $ ucode -l foo -e ' '
  Runtime error: No module named 'foo' could be found
  
  [1]

  $ touch moo; ucode -l foo moo
  Runtime error: No module named 'foo' could be found
  
  [1]

check that ucode can load fs module:

  $ ucode -l fs
  Require either -e/-p expression or source file
  [1]

  $ ucode -l fs -e ' '

  $ touch moo; ucode -l fs moo
