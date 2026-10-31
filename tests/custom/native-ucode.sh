#!/bin/sh
# Drop-in replacement for the ucode CLI that runs the given program as a
# natively compiled executable instead of in the interpreter. The test runner
# uses it through UCODE_BIN to run the test suite against native programs.
#
# Environment: UCODE (the ucode CLI built with LLVM_SUPPORT), UCODE_NATIVE
# (the runtime libucode-native.bc, with libucode-native.api and the
# native-abi directory beside it),
# NATIVE_LIBS (the libraries it needs), NATIVE_TARGET (the target triple when
# cross compiling), NATIVE_TARGET_FLAGS (the flags the runtime was built with,
# which make clang instead of llc compile the program), LLVM_LINK, OPT, LLC,
# CLANG, CC, LLVM_OPT_LEVEL, and NATIVE_RUNNER, a command to run the native
# executable with, such as valgrind or qemu.

ucode=${UCODE:-ucode}
runtime=${UCODE_NATIVE:?UCODE_NATIVE names no libucode-native.bc}
opt=${OPT:-opt}
llc=${LLC:-llc}
clang=${CLANG:-clang}
cc=${CC:-cc}
level=${LLVM_OPT_LEVEL:-1}
libs=${NATIVE_LIBS:--lm -ldl}

compile_args=
run_args=
source=

quote() {
	printf "'%s'" "$(printf '%s' "$1" | sed "s/'/'\\\\''/g")"
}

add_compile() { compile_args="$compile_args $(quote "$1")"; }
add_run() { run_args="$run_args $(quote "$1")"; }

# scripts that the program does not contain are compiled by the CLI of the
# build, not by one that PATH may find
add_run --ucode-compiler; add_run "$ucode"

# Modes that do not run a program, or that native programs do not provide,
# are left to the interpreter.
for arg in "$@"; do
	case "$arg" in
		--) break ;;
		-h|-p*|-c*|-t|-x*|-X*|-s|-o*) exec $ucode "$@" ;;
		-*) ;;
		*) break ;;
	esac
done

while [ $# -gt 0 ]; do
	case "$1" in
		-T*|-R|-S)
			add_compile "$1"
			shift
			;;
		-L)
			add_compile -L; add_compile "$2"
			add_run --ucode-search-path; add_run "$2"
			shift 2
			;;
		-L*)
			add_compile "$1"
			add_run --ucode-search-path; add_run "${1#-L}"
			shift
			;;
		-D|-F|-U|-l|-g)
			case "$1" in
				-D) add_run --ucode-define ;;
				-F) add_run --ucode-define-file ;;
				-U) add_run --ucode-undefine ;;
				-l) add_run --ucode-library ;;
				-g) add_run --ucode-gc ;;
			esac
			add_run "$2"
			shift 2
			;;
		-e)
			add_compile -e; add_compile "$2"
			source=expr
			shift 2
			break
			;;
		--)
			shift
			break
			;;
		*)
			source=$1
			shift
			break
			;;
	esac
done

tmp=$(mktemp -d) || exit 1
trap 'rm -rf "$tmp"' EXIT

# The test cases load the files of their "-- File --" sections also by
# computed names, so the program embeds all of them.
flags=llvm,debug${NATIVE_TARGET:+,target=$NATIVE_TARGET}

if [ -d files ]; then
	flags="$flags,embed=$PWD/files,embed-template=$PWD/files"
fi

if [ "$source" = expr ]; then
	eval "$ucode $compile_args -c,$flags -o \"\$tmp/prog.ll\"" || exit $?
else
	eval "$ucode $compile_args -c,$flags -o \"\$tmp/prog.ll\" \"\$source\"" || exit $?
fi

# llc takes the CPU and the float ABI in a syntax of its own
codegen() {
	if [ -n "$NATIVE_TARGET_FLAGS" ]; then
		$clang ${NATIVE_TARGET:+--target=$NATIVE_TARGET} $NATIVE_TARGET_FLAGS -O$level -fPIC \
			-c "$1" -o "$2"
	else
		$llc -O$level -relocation-model=pic -filetype=obj "$1" -o "$2"
	fi
}

${LLVM_LINK:-llvm-link} "$tmp/prog.ll" "$runtime" -o "$tmp/all.bc" &&
$opt -passes="internalize,default<O$level>" -internalize-public-api-file="${runtime%.bc}.api" \
	"$tmp/all.bc" -o "$tmp/prog.bc" &&
codegen "$tmp/prog.bc" "$tmp/prog.o" &&
$cc "$tmp/prog.o" $libs -rdynamic -Wl,-rpath,"${runtime%/*}/native-abi" -o "$tmp/prog" || exit 125

eval "$NATIVE_RUNNER \"\$tmp/prog\" $run_args -- \"\$@\""
