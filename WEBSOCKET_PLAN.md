# ucode `websocket` Module — Design & Tracking Plan

> Status: **M5 COMPLETE — PR-ready; M6 (TLS) optional**
> Created: 2026-09-01 · Last update: 2026-09-01
> Target repo path: `lib/websocket.c` (single-file ucode module)

---

## 1. Goals

- Provide WebSocket (RFC 6455) **client** support to ucode scripts.
- Minimum memory footprint (embedded/OpenWrt targets: mips, arm, 32–128 MB RAM devices).
- Depend only on a small, stable, well-maintained C library (see §3).
- Async, event-driven API integrated with `uloop` (pattern from `lib/uloop.c`).
- Play nice with the existing `socket` module philosophy (resources, errno-style errors).

### Non-goals (explicit)

- ❌ WebSocket **server** role (phase 2+, only if a real need appears).
- ❌ `permessage-deflate` compression — rejected: memory/CPU cost defeats the
  minimum-footprint goal on embedded targets.
- ❌ Auto-reconnect / reconnect backoff — userland responsibility by design.
- ❌ Thread pool / background threads — single-threaded, uloop-driven only.

---

## 2. Constraints & Budget

| Metric | Target |
|---|---|
| Code size (stripped .so, mips16) | < 100 KB |
| RSS delta per idle connection | < 60 KB |
| Default recv ring buffer | 4–8 KB (fixed, no per-message malloc) |
| Default max frame size cap | 256 KB (user-tunable, hard floor enforced) |
| External deps | wslay only (vendored), libc, libubox (uloop) |

---

## 3. Library Decision

| Option | Footprint | Maintenance | Verdict |
|---|---|---|---|
| **wslay** (MIT) | ~30 KB, zero-copy, no threads, no internal buffers (caller supplies buffers + I/O) | Stable; RFC 6455 is a frozen spec (2011) → code does not rot | ✅ **CHOSEN** (framing only) |
| libwebsockets | Large (HTTP/SSL machinery) | Excellent, very active | ❌ Overkill for client-only, too heavy |
| libuwsc | Small | Effectively unmaintained | ❌ Dead project |

**Architecture:** wslay is used *purely as the RFC 6455 framing engine*.
Transport is plain POSIX non-blocking sockets owned by the module, integrated
with `uloop` for readiness events. TLS (mbedTLS, already packaged in OpenWrt)
is a phase-2 optional add-on behind the same API.

---

## 4. Proposed API

```javascript
import { connect } from 'websocket';

let ws = connect('ws://10.0.0.1:8080/path', {
    headers: { Authorization: 'Bearer …' },
    max_frame_size: 262144,
    recv_buffer_size: 8192,
    timeout: 15000
});

ws.on('open',    (ws) => { … });
ws.on('message', (ws, data, is_text) => { … });
ws.on('close',   (ws, code, reason) => { … });
ws.on('error',   (ws, err) => { … });

ws.send('hello');              // string => text frame
ws.send(new Uint8Array(…));    // typed array => binary frame
ws.ping('are you there?');
ws.close(1000, 'bye');

ws.state;                      // 'connecting' | 'open' | 'closing' | 'closed'
```

Error reporting convention: match `lib/socket.c` — store last error, expose
`last_error()` / string form; throw ucode exceptions on programmer errors
(bad arguments), report runtime/network errors via the `error` event.

---

## 5. Architecture Notes

- Resource type via `ucv_resource_create_ex()` + `ucv_resource_persistent_set()`
  (pattern: `lib/uloop.c:88-101`).
- One fixed receive ring buffer per connection; partial frames resume across
  uloop wakeups — **no allocation per message**.
- Backpressure: stop registering the read event (`uloop_handle` delete) when the
  user's message callback is still running; resume after callback returns.
- Ping/pong and close handshake handled internally and transparently.
- Handshake `Sec-WebSocket-Key`/`Accept`: SHA-1 + base64 implemented locally
  (mirror the primitives used by `lib/digest.c`; no new dependency).
- GC finalizer calls `close()` safely if the user forgot (idempotent teardown).

---

## 6. Milestones & Step Tracking

Legend: `[ ]` pending · `[~]` in progress · `[x]` done · `[!]` blocked (see §9)

### M1 — Scaffold ✅ (2026-09-01)
- [x] ~~Vendor wslay sources under `lib/wslay/`~~ → superseded by D8: **external lib, zlib pattern**
- [x] CMake: `find_library(wslay)`/`find_path` + auto-gated `WEBSOCKET_SUPPORT` + link (like `zlib_lib`)
- [x] Module registration skeleton + doc header (`@module websocket`) in `lib/websocket.c`
      (plus `connect()` stub raising a "not implemented yet" exception)
- [x] Verified `ucode -lwebsocket` loads and registers `connect` (T1 ✅, re-verified after D8 refactor)
- [x] `libwslay` buildroot package (`../openwrt/package/wslay`, v1.1.1, hash pinned) —
      **cross-compiled + staged for aarch64** (pulled forward from M5)
- [x] `ucode-mod-websocket` added to `openwrt/ucode/Makefile` (DEPENDS `+libubox +libwslay`)
- Note: `websocket.so` has no wslay DT_NEEDED yet — stub references no symbols and the
  linker uses `--as-needed`; real linkage appears in M2

### M2 — Core client ✅ (2026-09-01)
- [x] URL parsing (`ws://` supported; `wss://` rejected with clear "TLS not supported yet";
      userinfo rejected; explicit port + query + IPv6 literal `[::1]:8080` handled —
      ⚠️ initial parser broke on `[::1]` (first colon read as port separator); found in
      pre-M3 review, fixed in M2 commit `f66c9d8` and verified end-to-end against a server on ::1)
- [x] DNS via synchronous `getaddrinfo` (see D12) + non-blocking TCP connect via uloop
- [x] HTTP Upgrade request generation incl. `headers` option (CRLF-injection guarded)
- [x] `Sec-WebSocket-Accept` validation (local SHA-1 + base64, no libmd dep)
- [x] wslay evented send/recv callbacks wired to the non-blocking fd
- [x] `on()` event dispatch (open/message/close/error) — callbacks receive the
      connection as **explicit first argument** (ucode arrows have no `this`, see D11)
- [x] `send()` (string → text, array → binary), `ping()`, `close(code, reason)`,
      plus `state()` and `fileno()` methods
- [x] Receive path: wslay fixed 4 KiB ibuf + capped message buffer (see D10)
- [x] Smoke-tested end-to-end against stdlib-Python WS echo server:
      T2 handshake ✓, T3 text echo ✓, T4 binary echo ✓, T7 ping/pong ✓,
      T8 close handshake ✓ (code 1000 round-trip), dead-port → `ERROR Connection
      refused` + clean exit, bad URL → type exception
- [x] Committed as `fe57635` on `websocket-module`

### M3 — Hardening ✅ (2026-09-01)
- [x] Frame-size cap enforcement — **T6 passes**: server frame > `max_frame_size`
      → close event code **1009**, completes in ~0.1 s (no timeout linger)
- [x] `EINTR`/`EAGAIN` audit (loops already retry); `EMFILE` path audited
      (connect loop closes fd per attempt, error propagates)
- [x] Idempotent `close()` (no-op while closing), abort semantics for `close()`
      during connecting/handshake (close 1006 + immediate teardown), GC finalizer
      (`ws_free_resource` via `uc_type_declare`) — dropped-resource scenario ASan clean
- [x] uloop fd/timeout deregistration on all error paths (teardown centralized;
      `uloop_fd_delete` on unregistered fd verified safe against libubox source)
- [x] Close-phase timeout (5 s) + re-entrancy guard (`dispatching`/`need_flush`)
      so `send()/close()` inside event callbacks never call into wslay re-entrantly
- [~] ASan/LSan clean — **zero findings** across happy/IPv6/oversized/abort/
      dead-port/dropped-resource scenarios (gcc `-fsanitize=address` build);
      "full cram suite" part lands with M4
- Bugs found & fixed during M3 (commit `2b30672`):
  - **UAF/heap corruption**: connection resource lacked its own `ucv_get()`
    reference — C side and script shared one refcount (the uloop.c pattern
    exists for a reason)
  - Handshake READ polling regression from the M2→M3 refactor (all connects timed out)
  - `ws://host:port/` host off-by-one introduced by the IPv6 fix (caught by
    re-running the v4 suite — regression tests matter)

### M4 — Tests & docs ✅ (2026-09-01)
- [x] C fixture WS server: `tests/cram/fixtures/ws-fixture.c` — scripted scenarios
      by request path (`/announce /echo /close /big<N> /frag<N> /flood<N> /ping
      /reset /badaccept /http200`), independent hand-rolled framing (no wslay
      on the server side), dual-stack listener
- [x] Cram tests: `tests/cram/test_websocket.t` — T1, T2, T3+T4, T7, T8, T5, T6,
      T11 (flood2000 no-loss), T9, badaccept, http200, T12 — all green
- [x] Test gating: fixture target + test file registered only under
      `WEBSOCKET_SUPPORT`; skips gracefully in wslay-less CI builds
- [x] jsdoc: full `connect()` documentation (options, events, example); module
      header covers usage patterns
- [x] ASan regression sweep after suite-driven fixes: announce + oversized clean
- Committed as `4543a6c` (history rewritten 2026-09-01: fix commits squashed into their milestone commits — c1bc024→M2, M4's module fixes→M3; each milestone verified to build; final tree byte-identical to pre-rewrite). Suite-driven bug fixes (all in same commit):
  un-zeroed resource struct, pipelined handshake bytes lost, close reason
  dropped, ECONNRESET unhandled, `on()` callbacks silently dropped (mangled
  guard), whole options object serialized as HTTP headers

### M5 — OpenWrt packaging & cross validation ✅ (2026-09-01)
- [x] Core `package/utils/ucode/Makefile`: `ucode-mod-websocket` added via the
      `UcodeModule` macro (`+libubox +libwslay`) — clean upstream-ready change
      on branch **`ucode-mod-websocket`** (commit `17829e4c4e`, worktree
      `/tmp/opencode/ucode-mod-pr`, based on upstream master)
- [x] Local cross-build: buildroot `package/utils/ucode/Makefile` temporarily
      overridden to `PKG_SOURCE_URL:=file:///home/nicolo/openwrt_ucode` pinned
      to `4543a6cb` (stock file backed up as `Makefile.stock`) → **aarch64
      `websocket.so` + `ucode-mod-websocket.apk` + `libwslay.apk` all built**
- [x] **Runtime cross-architecture validation**: aarch64/musl ucode executed
      under `qemu-aarch64` completed a full WebSocket session (handshake,
      pipelined announce message, close handshake) against the x86 fixture —
      see §8 for the exact invocation recipe
- [x] `.config`: `CONFIG_PACKAGE_libwslay=m`, `CONFIG_PACKAGE_ucode-mod-websocket=m`
- TLS moved to **M6**; server role remains deferred (non-goal until a use case)

### M6 — TLS via mbedTLS (future, optional)
- [ ] `wss://` support behind the same API (mbedTLS, non-blocking handshake
      integrated into the uloop state machine)
- [ ] Certificate verification policy options (ca_file, verify depth, insecure)
- [ ] Tests with an mbedTLS-based `wss` fixture endpoint

---

## 7. Decisions Log

| # | Date | Decision | Rationale |
|---|---|---|---|
| D1 | 2026-09-01 | Use wslay for RFC 6455 framing only; own transport on POSIX + uloop | Min memory; frozen spec = eternal; no hidden buffers/threads |
| D2 | 2026-09-01 | No permessage-deflate | Footprint goal on embedded targets |
| D3 | 2026-09-01 | Client role only in phase 1 | YAGNI; halves handshake/state-machine surface |
| D4 | 2026-09-01 | Fixed ring buffer, no per-message malloc | Deterministic memory; predictable RSS |
| D5 | 2026-09-01 | No auto-reconnect | Userland concern; keeps module stateless re: policy |
| D6 | 2026-09-01 | Vendor wslay in-tree, **statically linked into `websocket.so`** (like `ffi_lib` bundles its sources, `CMakeLists.txt:459`); optional `libwslay` feed package only in M5 | OpenWrt does **not** package wslay (verified in openwrt/packages master + snapshot indexes); OpenWrt ucode build compiles from this tree (`openwrt/ucode/Makefile:18`) so in-tree sources need no feed dependency. `ucode-mod-websocket` DEPENDS stays `ucode +libubox` |
| D7 | 2026-09-01 | Pin **wslay v1.1.1**; compile flags need no autotools `config.h` when building externally | Core sources compile warning-free under repo flags; autotools `configure` handles the `HAVE_*` detection when built as a proper library |
| D8 | 2026-09-01 | **Supersedes D6**: no in-tree vendoring — follow the **zlib pattern** instead: `find_library(wslay)` + `find_path(wslay/wslay.h)`, `WEBSOCKET_SUPPORT` auto-gates on discovery, module links external `libwslay`. OpenWrt side: `libwslay` package in the local buildroot (`../openwrt/package/wslay`, staged OK) + `ucode-mod-websocket` DEPENDS `+libwslay`. Local dev: wslay built into `/tmp/opencode/wslay-prefix`, pass `-DCMAKE_PREFIX_PATH` | User preference + matches ucode's zero-vendored-deps convention (`zlib_lib` links `ZLIB::ZLIB`, only the binding is in-tree) |
| D9 | 2026-09-01 | wslay goes to **core OpenWrt** (`package/libs/wslay`), not the packages feed | ucode is a core package (`package/utils/ucode`, source-pinned from jow-/ucode); core cannot depend on feeds; precedent: every existing ucode module dependency (libubox, libnl-tiny, libuci) is core. Feed alternative (standalone feed module, luci's ucode-mod-html pattern) kept as fallback if core review pushes back |
| D10 | 2026-09-01 | Receive memory model: wslay **default buffering** with `max_recv_msg_length` = `max_frame_size` option (default 256 KiB, 1 KiB–16 MiB). No custom ring buffer — wslay already owns fixed 4 KiB ibuf/obuf per context; our layer performs **zero per-message allocations** | Supersedes the "fixed ring buffer" design in §5: wslay's internal buffer is exactly the bounded, reused buffer we planned to write; duplicating it adds code without saving memory. Idle-connection fixed cost ≈ wslay ctx (~8.5 KiB) + handshake buffer (2 KiB) |
| D11 | 2026-09-01 | Event callbacks receive the connection resource as **explicit first argument** (`(ws, data, is_text)`), not via `this` | ucode arrow functions have no `this` binding; explicit args are the only portable form. Unhandled callback exceptions route through the shared `uloop.ex_handler` registry convention (falls back to `uloop_end()`) |
| D12 | 2026-09-01 | DNS resolution is synchronous (`getaddrinfo`) inside `connect()`; TCP connect + handshake + session are fully async | Matches `socket` module behavior (also sync connect); async DNS (uloop process or resolv-based) deferred until a real use case blocks on it |
| D13 | 2026-09-01 | Close completion semantics: when our close frame is **sent** and reads are **disabled** (wslay fatal recv condition), complete immediately with the close code we sent; only wait for the peer reply while reads remain enabled (bounded by 5 s `WS_CLOSE_TIMEOUT`) | wslay disables reads on oversize/protocol errors, so the peer's close reply can never be processed — waiting for it wedged connections for the full timeout |
| D14 | 2026-09-01 | Docs live in jsdoc headers (module + `connect()`), repo README untouched; test docs = cram file itself | ucode README describes the interpreter, not individual modules |
| D15 | 2026-09-01 | Fixture server implements framing independently (no wslay) — protocol bugs cannot hide behind a shared implementation; fixture + test file gated on `WEBSOCKET_SUPPORT` | Independent-implementation testing principle; keeps wslay-less CI green |
| D16 | 2026-09-01 | ucode grammar quirks to remember for tests: `typeof` is a prefix **operator** (`typeof(x)` in argument position misparses — assign to a variable first); heredoc-fed scripts (`ucode - <<EOF`) resolve `./`-relative modules from cwd | Cost hours during M4; recorded so M5+ tests avoid the traps |

*(Append new rows; never delete old ones.)*

---

## 8. Notes / Open Questions

### Build environment (this workstation)
- `libjson-c-dev` missing system-wide and sudo is not available non-interactively →
  built json-c **0.18** (same tarball as buildroot's `../openwrt/dl/json-c-0.18-nodoc.tar.gz`)
  into `/tmp/opencode/prefix`; configure ucode with
  `PKG_CONFIG_PATH=/tmp/opencode/prefix/lib/pkgconfig`. Needs
  `-DCMAKE_POLICY_VERSION_MINIMUM=3.5` (CMake 4 vs json-c's `cmake_minimum_required(2.8)`).
- **wslay** built into `/tmp/opencode/wslay-prefix` (configure/make/install from the
  v1.1.1 release tarball, shared lib) → add `-DCMAKE_PREFIX_PATH=/tmp/opencode/wslay-prefix`.
- **libubox** (needed since M2 for uloop) built into `/tmp/opencode/prefix` from the
  buildroot tarball (`../openwrt/dl/libubox-*.tar.zst`), cmake `-DBUILD_LUA=OFF`.
  Full local configure+verify:
  `PKG_CONFIG_PATH=/tmp/opencode/prefix/lib/pkgconfig cmake -B build -DCMAKE_PREFIX_PATH="/tmp/opencode/wslay-prefix;/tmp/opencode/prefix" -DULOOP_SUPPORT=ON`
  and at runtime `LD_LIBRARY_PATH=/tmp/opencode/prefix/lib:/tmp/opencode/wslay-prefix/lib:build`.
  Run test scripts from inside `build/` (import resolution uses the script's directory).
- Test fixture: `/tmp/opencode/wsserver.py` (stdlib-only RFC 6455 echo server) +
  `/tmp/opencode/ws_test.uc` — kept in /tmp, formal C fixture server is M4.
- **ASan/LSan build** (M3+): configure a second build tree with
  `-DCMAKE_C_FLAGS="-fsanitize=address -fno-omit-frame-pointer -g -O1"` and
  `-DCMAKE_EXE_LINKER_FLAGS/-DCMAKE_MODULE_LINKER_FLAGS/-DCMAKE_SHARED_LINKER_FLAGS="-fsanitize=address"`,
  run with `ASAN_OPTIONS=detect_leaks=1`.
- ⚠️ valgrind not installed and no sudo → ASan/LSan build is the leak-check method (W1 ✅).
- Cross-build path: `../openwrt` buildroot (mediatek/filogic, aarch64_cortex-a53, musl, apk
  backend). `libwslay` package created + staged. The buildroot's `package/utils/ucode/Makefile`
  currently carries the **local override** (`PKG_SOURCE_URL:=file:///home/nicolo/openwrt_ucode`,
  pinned to `4543a6cb`, mirror skipped; stock version saved next to it as `Makefile.stock`).
  Cross-build + qemu runtime recipe (validated 2026-09-01):
  ```
  make package/ucode/compile V=s          # in ../openwrt
  B=../openwrt/build_dir/target-aarch64_cortex-a53_musl/ucode-*/ipkg-install
  R=../openwrt/staging_dir/target-aarch64_cortex-a53_musl/root-mediatek
  qemu-aarch64 -L $R -E LD_LIBRARY_PATH=$R/usr/lib:. $B/usr/bin/ucode \
      -lwebsocket -luloop test.uc        # modules copied to cwd
  ```
- ucode syntax gotcha for tests: `typeof` is a global **function** (`typeof(x)`),
  not a unary operator — `print(typeof websocket)` is a syntax error.

### Open questions
- [x] Confirm pinned wslay release version + vendoring layout acceptable upstream
      (ucode currently vendors nothing — check maintainer preference).
      → 2026-09-01: **OpenWrt has no wslay package** (checked openwrt/packages
      master tree + snapshot .apk indexes). WebSocket libs actually packaged:
      `libwebsockets` 4.5.8 (~250 KB), `websocketpp` (C++), `libuwsc` (dead upstream).
      → Consequence (D6): vendoring is mandatory unless we contribute a feed package.
      → Data point: `lua-eco-websocket` is only 5.4 KB → tiny hand-rolled client
      is a viable fallback if vendoring is rejected upstream.
- [ ] Decide: reexport SHA-1/base64 helpers or link against existing digest code paths?
- [x] IPv6 literal URLs (`ws://[::1]:8080/`) — **verified 2026-09-01** (after fixing
      `c1bc024`); parser strips brackets for getaddrinfo, re-adds them for the Host header.
- [ ] Callback re-entrancy rules: document what happens if user calls `send()`
      from inside a `message` callback, and `close()` from `open`.
- [ ] Subprotocols (`Sec-WebSocket-Protocol`) — cheap to add in M2, decide default behavior.

---

## 9. Blockers / Watchpoints

| # | Date | Item | Impact | Status |
|---|---|---|---|---|
| W1 | 2026-09-01 | No valgrind on workstation, no sudo | M3 T9/T10 verification method | ✅ resolved 2026-09-01: gcc ASan/LSan build (`/tmp/opencode/build-asan`, see §8) — all scenarios zero findings |
| W2 | 2026-09-01 | Python fixture server blocks in `sendall` on big frames | ✅ resolved 2026-09-01: C fixture exits on `EPIPE` when the client aborts an oversize read, so blocking writes are sufficient |

*(Add rows when blocked; remove/annotate when resolved — resolved rows move to §7 or §8.)*

---

## 10. Test Plan

| ID | Area | Test | Milestone |
|---|---|---|---|
| T1 | load | `ucode -lwebsocket` → `websocket` object with native `connect` function; stub call raises expected exception — **PASSED 2026-09-01** | M1 ✅ |
| T2 | handshake | connect to fixture server; assert `open` fired + correct path/headers received server-side | M2 |
| T3 | echo-text | send string → receive identical string, `is_text == true` | M2 |
| T4 | echo-binary | send `Uint8Array` → receive identical bytes | M2 |
| T5 | fragmentation | server sends 1 MB message in 1 KB fragments; assert reassembly + cap behavior | M2/M3 |
| T6 | oversized | server sends frame > `max_frame_size`; expect close code 1009 | M3 |
| T7 | ping/pong | server pings; expect pong + no user-visible event | M2 |
| T8 | close-handshake | clean close from both sides; assert code+reason surface in `close` event | M2 |
| T9 | abrupt-reset | server RSTs connection; `error` event + no fd/uloop leak | M3 |
| T10 | gc-teardown | drop reference without `close()`; GC; assert no leak (ASan/LSan run) | M3 |
| T11 | backpressure | slow consumer callback; assert read event paused/resumed (fixture sends unboundedly) | M3 |
| T12 | uri-validation | bad schemes/ports/userinfo → clear exception, no fd created | M2 |

Infrastructure notes:
- Fixture server: small C program in `tests/cram/fixtures/` (libevent-free, plain poll())
  able to run scripted scenarios: echo, fragment, oversized, reset, ping-flood.
- All tests must run under `ctest` via the existing cram harness (`tests/CMakeLists.txt`).
- Memory assertions: run selected tests under `valgrind --leak-check=full` in CI (x86_64 only).

---

## 11. PR Preparation (2026-09-01)

### PR 1 — wslay → core OpenWrt (`openwrt/openwrt` master)
- Branch: **`wslay-package`** (in the `../openwrt` fork repo, checked out in the
  worktree `/tmp/opencode/wslay-pr`, based on current upstream master `9550b20e42`)
- Commit: `893873a4e8` — `package/libs/wslay: add wslay WebSocket library` (signed off)
- Push + open PR:
  ```
  git -C ../openwrt push origin wslay-package
  # then PR: hitech95:wslay-package -> openwrt:master
  ```
- Draft description:
  > Adds wslay v1.1.1 (MIT), a non-IO WebSocket (RFC 6455) framing library, as
  > `package/libs/wslay`. It performs no I/O itself — the caller drives the event
  > loop through send/recv callbacks — making it suitable for memory-constrained,
  > event-driven use. Needed by the upcoming `ucode-mod-websocket` module
  > (companion PR in jow-/ucode, which requires this lib in core since ucode is
  > a core package and core cannot depend on feeds). Static + shared libs built;
  > InstallDev ships headers + pkg-config.

### PR 1b — ucode-mod-websocket → core OpenWrt (`openwrt/openwrt` master)
- Branch: **`ucode-mod-websocket`** (worktree `/tmp/opencode/ucode-mod-pr`,
  commit `17829e4c4e`): adds the module package via the `UcodeModule` macro
  (`+libubox +libwslay`). **Hold** until the ucode-side PR is merged, then
  combine with the `PKG_SOURCE_VERSION` bump in the same PR.
- Push: `git -C ../openwrt push origin ucode-mod-websocket`

### PR 2 — websocket module → upstream ucode (`jow-/ucode` master)
- Branch: **`websocket-module`** (in this repo, `4543a6c` — M1–M4 complete, one clean commit per milestone:
  implementation, hardening, cram suite with fixture server, jsdoc)
- M2–M4 are done → **ready to submit** (was: hold until M2–M4)
- Push (needs a jow-/ucode fork first) + open PR:
  ```
  git remote add fork git@github.com:<user>/ucode.git
  git push -u fork websocket-module
  # then PR: <user>:websocket-module -> jow-:master
  ```
- Draft description:
  > Adds a `websocket` module providing RFC 6455 client connectivity with an
  > event-driven, uloop-integrated API (`connect()`, `on('open'|'message'|
  > 'close'|'error')`, `send()`, `close()`). Framing is delegated to wslay
  > (linked externally, zlib-style: `WEBSOCKET_SUPPORT` auto-gates on library
  > discovery). Fixed receive ring buffer, frame-size caps and uloop-driven
  > backpressure keep the memory footprint deterministic. Requires libwslay
  > (core PR package/libs/wslay).
- Follow-up after merge: bump `PKG_SOURCE_VERSION` of core `package/utils/ucode`
  and add `ucode-mod-websocket` there (sync with `openwrt/ucode/Makefile`).
- `WEBSOCKET_PLAN.md` intentionally untracked (internal tracking doc).

---

## 12. References

- RFC 6455 — The WebSocket Protocol
- wslay — https://github.com/tatsuhiro-t/wslay (MIT)
- Repo patterns: `lib/socket.c` (resources/errors), `lib/uloop.c:88-101`
  (persistent resources + callbacks), `lib/digest.c` (SHA-1 primitives)
- Existing test harness: `tests/cram/`, `tests/CMakeLists.txt`

### Commit history (post-rewrite 2026-09-01)
- `ce8ae99` M1 skeleton · `f66c9d8` M2 implementation (+IPv6, suite-class fixes) · `2b30672` M3 hardening (+test-uncovered fixes) · `4543a6c` M4 tests+fixture+jsdoc
- Backup of pre-rewrite history: `websocket-module-backup`
