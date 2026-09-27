# The ucode Language

The ucode language is a tiny general purpose scripting language featuring a
syntax closely resembling ECMAScript. It can be used in a stand-alone manner
by using the ucode command line interpreter or embedded into host applications
by linking libucode and utilizing its C language API. Additionally, ucode can
be invoked in template mode where control flow and expression logic statements
are embedded in Jinja-like markup blocks.

Besides aiming for small size, the major design goals of ucode are the ability
to trivially read and write JSON data, good embeddability into C applications,
template capabilities for output formatting, extensiblity through loadable
native extension modules and a straightforward set of built-in functions
mimicking those found in the Perl 5 language.

## History and Motivation

In spring 2021 it has been decided to rewrite the OpenWrt firewall framework on
top of nftables with the goal to replace the then current C application with a
kind of preprocessor generating nftables rulesets using a set of templates
instead of relying on built-in hardcoded rules like its predecessor.

That decision spurred the development of *ucode*, initially meant to be a
simple template processor solely for the OpenWrt nftables firewall but quickly
evolving into a general purpose scripting language suitable for a wider range
of system scripting tasks.

Despite OpenWrt predominantly relying on POSIX shell and Lua as system
scripting languages already, a new solution was needed to accomodate the needs
of the new firewall implementation; mainly the ability to efficiently deal with
JSON data and complex data structures such as arrays and dictionaries and the
ability to closely interface with OpenWrt's *ubus* message bus system.

Throughout the design process of the new firewall and its template processor,
the following design goals were defined for the *ucode* scripting language:

 - Ability to embed code logic fragments such as control flow statements,
   function calls or arithmetic expressions into plain text templates, using
   a block syntax and functionality roughly inspired by Jinja templates
 - Built-in support for JSON data parsing and serialization, without the need
   for external libraries
 - Distinct array and object types (compared to Lua's single table datatype)
 - Distinct integer and float types and guaranteed 64bit integer range
 - Built-in support for bit operations
 - Built-in support for (POSIX) regular expressions
 - A comprehensive set of built-in standard functions, inspired by the core
   functions found in the Perl 5 interpreter
 - Staying as close to ECMAScript syntax as possible due to higher developer
   familiarity and to be able to reuse existing tooling such as editor syntax
   highlighting
 - Bindings for all relevant Linux and OpenWrt APIs, such as *ubus*, *uci*,
   *uloop*, *netlink* etc.
 - Procedural, synchronous programming flow
 - Very small executable size (the interpreter and runtime is currently around
   64KB on ARM Cortex A9)
 - Embeddability into C host applications

Summarized, *ucode* can be described as synchronous ECMAScript without the
object oriented standard library.


## Installation

### OpenWrt

In OpenWrt 22.03 and later, *ucode* should already be preinstalled. If not,
it can be installed via the package manager, using the `opkg install ucode`
command.

### MacOS

To build on MacOS, first install *cmake*, *json-c* and *libmd* via
[Homebrew](https://brew.sh/), then clone the ucode repository and execute
*cmake* followed by *make*:

    $ brew install cmake json-c libmd
    $ git clone https://github.com/ucode-lang/ucode.git
    $ cd ucode/
    $ cmake -DUBUS_SUPPORT=OFF -DUCI_SUPPORT=OFF -DULOOP_SUPPORT=OFF -DCMAKE_BUILD_RPATH=/opt/homebrew/lib -DCMAKE_INSTALL_RPATH=/opt/homebrew/lib .
    $ make
    $ sudo make install

### Debian

The ucode repository contains build recipes for Debian packages, to build .deb
packages for local installation, first install required development packages,
then clone the repository and invoke *dpkg-buildpackage* to produce the binary
package files:

    $ sudo apt-get install build-essential devscripts debhelper libjson-c-dev cmake pkg-config
    $ git clone https://github.com/ucode-lang/ucode.git
    $ cd ucode/
    $ dpkg-buildpackage -b -us -uc
    $ sudo dpkg -i ../ucode*.deb ../libucode*.deb

### Other Linux systems

To install ucode from source on other systems, ensure that the json-c library
and associated development headers are installed, then clone and compile the
ucode repository:

    $ git clone https://github.com/ucode-lang/ucode.git
    $ cd ucode/
    $ cmake -DUBUS_SUPPORT=OFF -DUCI_SUPPORT=OFF -DULOOP_SUPPORT=OFF .
    $ make
    $ sudo make install
