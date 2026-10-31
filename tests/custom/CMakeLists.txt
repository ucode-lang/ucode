ADD_TEST(
	NAME custom
	COMMAND $<TARGET_FILE:ucode> -L $<TARGET_FILE_DIR:fs_lib>/*.so -S run_tests.uc
	WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
)
SET_PROPERTY(TEST custom APPEND PROPERTY ENVIRONMENT
	"UCODE_BIN=valgrind --quiet --leak-check=full --child-silent-after-fork=yes --suppressions=${CMAKE_CURRENT_SOURCE_DIR}/valgrind.supp $<TARGET_FILE:ucode>"
	"UCODE_LIB=${CMAKE_BINARY_DIR}"
)

IF(CMAKE_C_COMPILER_ID STREQUAL "Clang")
	ADD_TEST(
		NAME custom-san
		COMMAND $<TARGET_FILE:ucode> -L $<TARGET_FILE_DIR:fs_lib>/*.so -S run_tests.uc
		WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
	)

	SET_PROPERTY(TEST custom-san APPEND PROPERTY ENVIRONMENT
		"UCODE_BIN=$<TARGET_FILE:ucode-san>"
		"UCODE_LIB=${CMAKE_BINARY_DIR}"
	)
ENDIF()

IF(LLVM_SUPPORT)
	FIND_PROGRAM(LLVM_OPT NAMES opt opt-21 opt-20 opt-19 opt-18 opt-17 opt-16)
	FIND_PROGRAM(LLVM_LLC NAMES llc llc-21 llc-20 llc-19 llc-18 llc-17 llc-16)

	IF(LLVM_OPT AND LLVM_LLC)
		ADD_TEST(
			NAME custom-native
			COMMAND $<TARGET_FILE:ucode> -L $<TARGET_FILE_DIR:fs_lib>/*.so -S run_tests.uc
			WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
		)

		SET_PROPERTY(TEST custom-native APPEND PROPERTY ENVIRONMENT
			"UCODE_BIN=${CMAKE_CURRENT_SOURCE_DIR}/native-ucode.sh"
			"UCODE_LIB=${CMAKE_BINARY_DIR}"
			"UCODE_TEST_SKIP=${CMAKE_CURRENT_SOURCE_DIR}/native-unsupported"
			"NATIVE_RUNNER=valgrind --quiet --leak-check=full --child-silent-after-fork=yes --suppressions=${CMAKE_CURRENT_SOURCE_DIR}/valgrind.supp"
			"UCODE=$<TARGET_FILE:ucode>"
			"UCODE_NATIVE=${CMAKE_BINARY_DIR}/libucode-native.bc"
			"LLVM_LINK=${NATIVE_LLVM_LINK}"
			"OPT=${LLVM_OPT}"
			"LLC=${LLVM_LLC}"
			"CC=${CMAKE_C_COMPILER}"
		)
	ELSE()
		MESSAGE(WARNING "LLVM opt and llc not found, not running the tests against native programs")
	ENDIF()
ENDIF()

# Debugger tests
ADD_TEST(
	NAME debugger
	COMMAND $<TARGET_FILE:ucode> -L $<TARGET_FILE_DIR:fs_lib>/*.so -S 99_debugger/run_debugger_tests.uc
	WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
)
SET_PROPERTY(TEST debugger APPEND PROPERTY ENVIRONMENT
	"UCODE_BIN=valgrind --quiet --leak-check=full $<TARGET_FILE:ucode>"
	"UCODE_LIB=${CMAKE_BINARY_DIR}"
)

IF(CMAKE_C_COMPILER_ID STREQUAL "Clang")
	ADD_TEST(
		NAME debugger-san
		COMMAND $<TARGET_FILE:ucode> -L $<TARGET_FILE_DIR:fs_lib>/*.so -S 99_debugger/run_debugger_tests.uc
		WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
	)

	SET_PROPERTY(TEST debugger-san APPEND PROPERTY ENVIRONMENT
		"UCODE_BIN=$<TARGET_FILE:ucode-san>"
		"UCODE_LIB=${CMAKE_BINARY_DIR}"
	)
ENDIF()
