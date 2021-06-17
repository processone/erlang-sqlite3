# Erlang wrapper for SQLite3

[![CI](https://github.com/processone/erlang-sqlite3/actions/workflows/ci.yml/badge.svg)](https://github.com/processone/erlang-sqlite3/actions/workflows/ci.yml)
[![Coverage Status](https://coveralls.io/repos/processone/erlang-sqlite3/badge.svg?branch=master&service=github)](https://coveralls.io/github/processone/erlang-sqlite3?branch=master)
[![Hex version](https://img.shields.io/hexpm/v/sqlite3.svg "Hex version")](https://hex.pm/packages/sqlite3)

This library allows you to work with SQLite3 databases from Erlang.

It is compatible with Windows and Linux, and should probably work on other OSes as well.

See also [esqlite](https://github.com/mmzeeman/esqlite) for an alternative library.

## Requirements

Erlang/OTP R14B or later is required (tested up to 17.3 at this writing), and SQLite 3 minimum version is 3.6.1.

## Compiling

### Linux

1. Install SQLite3 by running `sudo apt-get install sqlite3` or the equivalent for your package manager, or by [compiling from the source](http://source.online.free.fr/Linux_HowToCompileSQLite.html).

2. `make`.

### Cross-compiling

If you want to use erlang-sqlite3 on an embedded device, it can be cross-compiled.

1. Cross-compile [SQLite3](http://www.sqlite.org/cvstrac/wiki?p=HowToCompile) and [Erlang](http://www.erlang.org/doc/installation_guide/INSTALL-CROSS.html).

2. Change variables and paths in `rebar.cross_compile.config.sample` to the desired values and rename it to `rebar.cross_compile.config`.

3. `make cross_compile`.

### Windows with MS Visual C++

To build both SQLite3 and sqlite3-erlang:

1. If MSVC tools (`cl`, `link`, etc.) are not in the path, run `vcvars32.bat` or `vcvars64.bat` depending on whether you use 32-bit or 64-bit Erlang. `build_port_win32.bat` and `build_port_win64.bat` have the standard paths for VC10.0.

2. `nmake`.

Alternately, you can use prebuilt versions of `sqlite3.dll` and `sqlite3.def`. To make `sqlite3.lib`, use `lib /def:sqlite3.def`. Then remove `sqlite3.dll` and `sqlite3.lib` targets from `Makefile` and do as above.

### Potential compilation problems

* If SQLite was built with `SQLITE_OMIT_LOAD_EXTENSION` option, you'll need to undefine `ERLANG_SQLITE3_LOAD_EXTENSION` macro in <c_src/sqlite3_drv.h>.

## Running the test suite

### Linux

`make test`

### Windows

1. `nmake tests`

2. If you get the error `"Error loading sqlite3_drv: The specified module could not be found"`, this is because `sqlite3.dll` isn't in the search path.

## Example usage

See tests `test/sqlite3_test.erl` for a starting point. On Windows note that `sqlite3.dll` must be in your application's working directory or somewhere in the DLL search path.

## Authors

See ./AUTHORS
