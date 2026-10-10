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
