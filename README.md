# The ucode language

ucode is a small scripting language for Linux systems. It features an
ECMAScript-like syntax, native JSON datatypes, a built-in template engine and
an embeddable C core, and it ships preinstalled on OpenWrt, where it powers
the nftables firewall and the LuCI web interface.

For a comprehensive treatment of the language, the standard library and the C
embedding API, see the [ucode manual](https://ucode-lang.org/manual/).

## Tutorials

- [Usage](tutorial-01-usage.html) — the `ucode` command line options and how
  scripts receive arguments and data.
- [Arrays](tutorial-02-arrays.html) — recipes for set operations, chunking,
  flattening and deep copying, and a common pitfall when mapping builtins.
- [Objects](tutorial-03-objects.html) — recipes for merging, filtering and
  comparing objects, and using them as caches and lookup tables.
- [Metamethods](tutorial-04-metamethods.html) — patterns for making values
  callable, synthesizing properties and routing writes to backing stores.

## Module reference

- [core](module-core.html) — the builtin functions available to every script.
- [debug](module-debug.html) — breakpoints, backtraces, memory dumps and the
  line-based debug protocol.
- [digest](module-digest.html) — MD2/4/5, SHA1/256/384/512 and FNV-1a digests.
- [ffi](module-ffi.html) — foreign function interface to C libraries.
- [fs](module-fs.html) — file and directory operations, `popen()` and friends.
- [io](module-io.html) — POSIX file descriptor I/O.
- [log](module-log.html) — syslog and OpenWrt ulog output.
- [math](module-math.html) — mathematical functions and pseudo-random numbers.
- [netaddr](module-netaddr.html) — IPv4, IPv6 and MAC address validation and
  range operations.
- [nl80211](module-nl80211.html) — wireless extension over the nl80211 netlink
  interface.
- [resolv](module-resolv.html) — DNS name resolution.
- [rtnl](module-rtnl.html) — interfaces, addresses and routes over rtnetlink.
- [serial](module-serial.html) — serial port attributes and I/O.
- [socket](module-socket.html) — BSD sockets.
- [struct](module-struct.html) — packing and unpacking binary data.
- [ubus](module-ubus.html) — ubus IPC: object calls, events and subscriptions.
- [uci](module-uci.html) — OpenWrt configuration (UCI) access.
- [uloop](module-uloop.html) — the event loop: timers, signals, processes and
  tasks.
- [zlib](module-zlib.html) — gzip and deflate compression and decompression.
