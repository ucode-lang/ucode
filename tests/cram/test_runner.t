setup common environment:

  $ export UCODE_LIB="$BUILD_BIN_DIR"
  $ custom="$TESTDIR/../custom"
  $ run_tests() {
  >   "$BUILD_BIN_DIR/ucode" -L "$BUILD_BIN_DIR/*.so" -S "$@"
  > }

check that the test runner accepts a TMPDIR with a trailing slash:

  $ (export TMPDIR="$TMPDIR/"; run_tests "$custom/run_tests.uc" "$custom/04_modules/07_import_default") > out.txt
  $ tail -n 1 out.txt
  Ran 1 tests, 1 okay, 0 failures

check that the test runner accepts a TMPDIR behind a symbolic link:

  $ mkdir tmp.real && ln -s tmp.real tmp.link
  $ (export TMPDIR="$PWD/tmp.link"; run_tests "$custom/run_tests.uc" "$custom/04_modules/07_import_default") > out.txt
  $ tail -n 1 out.txt
  Ran 1 tests, 1 okay, 0 failures

check that the test runner creates its work directory below TMPDIR:

  $ mkdir -p where/00_dir && cp "$custom/run_tests.uc" where/
  $ cat > where/00_dir/01_workdir <<EOF
  > -- Testcase --
  > {% system('pwd > "\$MARKER"') %}
  > -- End --
  > -- Expect stdout --
  > -- End --
  > EOF
  $ (export TMPDIR="$PWD/tmp.link" MARKER="$PWD/marker"; run_tests where/run_tests.uc) > out.txt
  $ tail -n 1 out.txt
  Ran 1 tests, 1 okay, 0 failures
  $ grep -c "^$(pwd -P)/tmp.real/test\.[0-9]*\$" marker
  1

check that the test runner refuses a TMPDIR that does not exist:

  $ (export TMPDIR="$PWD/missing"; run_tests "$custom/run_tests.uc" "$custom/04_modules/07_import_default")
  Unable to use the temporary directory '*/missing': No such file or directory (glob)
  [1]
