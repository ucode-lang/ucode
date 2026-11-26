# Removes the target CPU, feature and float ABI attributes from the functions
# of the runtime. LLVM inlines a function only into callers with compatible
# target attributes, and the generated functions of a program have none, so
# with them no helper is inlined. Without them, the code generation of the
# program sets the CPU and the ABI for all functions alike.
#
# The attributes are matched in the textual IR; checked with LLVM 19 only.
#
# Usage: cmake -DINPUT=linked.ll -DOUTPUT=libucode-native.ll -P native-strip.cmake

file(READ ${INPUT} ir)

string(REGEX REPLACE " \"(target-cpu|target-features|tune-cpu|use-soft-float)\"=\"[^\"]*\"" ""
       ir "${ir}")

file(WRITE ${OUTPUT} "${ir}")
