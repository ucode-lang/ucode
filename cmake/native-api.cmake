# Writes the symbols that a native program must keep exported, one per line:
# its main() and every function or variable of the runtime with default
# visibility, which bindings may call. All other symbols of the runtime can
# become internal when it is linked with a program at the IR level.
#
# Usage: cmake -DINPUT=libucode-native.ll -DOUTPUT=libucode-native.api -P native-api.cmake

file(READ ${INPUT} ir)

# matched as a whole, since IR lines do not split into CMake list elements
string(REGEX MATCHALL "\ndefine dso_local [^\n@]*@[A-Za-z0-9_.]+\\(" functions "${ir}")
string(REGEX MATCHALL "\n@[A-Za-z0-9_.]+ = dso_local " variables "${ir}")

set(symbols "main")

foreach(match IN LISTS functions variables)
  if(match MATCHES "@([A-Za-z0-9_.]+)")
    list(APPEND symbols ${CMAKE_MATCH_1})
  endif()
endforeach()

list(REMOVE_DUPLICATES symbols)
list(JOIN symbols "\n" content)
file(WRITE ${OUTPUT} "${content}\n")
