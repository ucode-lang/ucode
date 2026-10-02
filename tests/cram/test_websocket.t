setup common environment:

  $ [ -n "$BUILD_BIN_DIR" ] && export PATH="$BUILD_BIN_DIR:$PATH"
  $ if command -v valgrind >/dev/null 2>&1; then
  >   alias ucode="$UCODE_BIN"
  > else
  >   alias ucode="$BUILD_BIN_DIR/ucode"
  > fi

  $ for m in $BUILD_BIN_DIR/*.so; do
  >   ln -s "$m" "$(pwd)/$(basename $m)"; \
  > done

check that the websocket module loads:

  $ ucode -lwebsocket -e 'let f = websocket.connect; print(f)'
  function connect(...) { [native code] } (no-eol)


test connection establishment and handshake header validation (T2):

  $ PORT=28971
  $ $WS_FIXTURE $PORT 1 &
  $ sleep 0.2

  $ ucode -lwebsocket -luloop - <<'EOF'
  > import { connect } from 'websocket';
  > import * as uloop from 'uloop';
  > uloop.init();
  > let ws = connect('ws://127.0.0.1:28971/announce?x=1', { timeout: 5000 });
  > ws.on('message', (w, data, is_text) => {
  > 	print(`MSG ${data}\n`);
  > 	w.close(1000, 'done');
  > });
  > ws.on('close', (w, code) => { print(`CLOSE ${code}\n`); uloop.end(); });
  > ws.on('error', (w, e) => { print(`ERROR ${e}\n`); uloop.end(); });
  > uloop.run();
  > print('DONE\n');
  > EOF
  MSG path=/announce?x=1 host=127.0.0.1:28971
  CLOSE 1000
  DONE

  $ wait


test text and binary echo (T3, T4):

  $ PORT=28972
  $ $WS_FIXTURE $PORT 1 &
  $ sleep 0.2

  $ ucode -lwebsocket -luloop - <<'EOF'
  > import { connect } from 'websocket';
  > import * as uloop from 'uloop';
  > uloop.init();
  > let ws = connect('ws://127.0.0.1:28972/echo', { timeout: 5000 });
  > let count = 0;
  > ws.on('open', (w) => {
  > 	w.send('hello');
  > 	w.send([1, 2, 3, 250]);
  > });
  > ws.on('message', (w, data, is_text) => {
  > 	count++;
  > 	print(`MSG ${is_text} ${length(data)}\n`);
  > 	if (count >= 2)
  > 		w.close(1000, '');
  > });
  > ws.on('close', (w, code) => { print(`CLOSE ${code}\n`); uloop.end(); });
  > ws.on('error', (w, e) => { print(`ERROR ${e}\n`); uloop.end(); });
  > uloop.run();
  > print('DONE\n');
  > EOF
  MSG true 5
  MSG false 4
  CLOSE 1000
  DONE

  $ wait


test ping/pong handling (T7):

  $ PORT=28973
  $ $WS_FIXTURE $PORT 1 &
  $ sleep 0.2

  $ ucode -lwebsocket -luloop - <<'EOF'
  > import { connect } from 'websocket';
  > import * as uloop from 'uloop';
  > uloop.init();
  > let ws = connect('ws://127.0.0.1:28973/ping', { timeout: 5000 });
  > ws.on('open', (w) => {
  > 	w.ping('client-ping');
  > 	uloop.timer(500, () => w.close(1000, ''));
  > });
  > ws.on('message', (w, data, is_text) => {
  > 	print(`MSG ${data}\n`);
  > 	w.close(1000, '');
  > });
  > ws.on('close', (w, code) => { print(`CLOSE ${code}\n`); uloop.end(); });
  > ws.on('error', (w, e) => { print(`ERROR ${e}\n`); uloop.end(); });
  > uloop.run();
  > print('DONE\n');
  > EOF
  CLOSE 1000
  DONE

  $ wait


test server initiated close handshake with code and reason (T8):

  $ PORT=28974
  $ $WS_FIXTURE $PORT 1 &
  $ sleep 0.2

  $ ucode -lwebsocket -luloop - <<'EOF'
  > import { connect } from 'websocket';
  > import * as uloop from 'uloop';
  > uloop.init();
  > let ws = connect('ws://127.0.0.1:28974/close', { timeout: 5000 });
  > ws.on('message', (w, data, is_text) => print(`UNEXPECTED MSG\n`));
  > ws.on('close', (w, code, reason) => {
  > 	print(`CLOSE ${code} ${reason}\n`);
  > 	uloop.end();
  > });
  > ws.on('error', (w, e) => { print(`ERROR ${e}\n`); uloop.end(); });
  > uloop.run();
  > print('DONE\n');
  > EOF
  CLOSE 1001 server bye
  DONE

  $ wait


test fragmented message reassembly (T5):

  $ PORT=28975
  $ $WS_FIXTURE $PORT 1 &
  $ sleep 0.2

  $ ucode -lwebsocket -luloop - <<'EOF'
  > import { connect } from 'websocket';
  > import * as uloop from 'uloop';
  > uloop.init();
  > let ws = connect('ws://127.0.0.1:28975/frag8192', { timeout: 5000 });
  > ws.on('message', (w, data, is_text) => {
  > 	print(`MSG ${length(data)}\n`);
  > 	w.close(1000, '');
  > });
  > ws.on('close', (w, code) => { print(`CLOSE ${code}\n`); uloop.end(); });
  > ws.on('error', (w, e) => { print(`ERROR ${e}\n`); uloop.end(); });
  > uloop.run();
  > print('DONE\n');
  > EOF
  MSG 8192
  CLOSE 1000
  DONE

  $ wait


test oversized frame rejection with close code 1009 (T6):

  $ PORT=28976
  $ $WS_FIXTURE $PORT 1 &
  $ sleep 0.2

  $ ucode -lwebsocket -luloop - <<'EOF'
  > import { connect } from 'websocket';
  > import * as uloop from 'uloop';
  > uloop.init();
  > let ws = connect('ws://127.0.0.1:28976/big131072',
  >                  { timeout: 5000, max_frame_size: 65536 });
  > ws.on('message', (w, data, is_text) => print('UNEXPECTED MSG\n'));
  > ws.on('close', (w, code) => { print(`CLOSE ${code}\n`); uloop.end(); });
  > ws.on('error', (w, e) => { print(`ERROR ${e}\n`); uloop.end(); });
  > uloop.run();
  > print('DONE\n');
  > EOF
  CLOSE 1009
  DONE

  $ wait


test message flood is processed without loss (T11):

  $ PORT=28977
  $ $WS_FIXTURE $PORT 1 &
  $ sleep 0.2

  $ ucode -lwebsocket -luloop - <<'EOF'
  > import { connect } from 'websocket';
  > import * as uloop from 'uloop';
  > uloop.init();
  > let ws = connect('ws://127.0.0.1:28977/flood2000',
  >                  { timeout: 5000, max_frame_size: 65536 });
  > let count = 0;
  > ws.on('message', (w, data, is_text) => {
  > 	count++;
  > 	if (count == 2000) {
  > 		print(`MSGS ${count} ${length(data)}\n`);
  > 		w.close(1000, '');
  > 	}
  > });
  > ws.on('close', (w, code) => { print(`CLOSE ${code}\n`); uloop.end(); });
  > ws.on('error', (w, e) => { print(`ERROR ${e}\n`); uloop.end(); });
  > uloop.run();
  > print('DONE\n');
  > EOF
  MSGS 2000 1024
  CLOSE 1000
  DONE

  $ wait


test abrupt connection reset surfaces an error (T9):

  $ $WS_FIXTURE 28978 1 &
  $ sleep 0.2

  $ ucode -lwebsocket -luloop - <<'EOF'
  > import { connect } from 'websocket';
  > import * as uloop from 'uloop';
  > uloop.init();
  > let ws = connect('ws://127.0.0.1:28978/reset', { timeout: 5000 });
  > ws.on('open', (w) => print('OPEN\n'));
  > ws.on('close', (w, code) => { print(`CLOSE ${code}\n`); uloop.end(); });
  > ws.on('error', (w, e) => { print(`ERROR\n`); uloop.end(); });
  > uloop.run();
  > print('DONE\n');
  > EOF
  OPEN
  ERROR
  CLOSE 1006
  DONE

  $ wait


test handshake failure on wrong Sec-WebSocket-Accept:

  $ $WS_FIXTURE 28979 1 &
  $ sleep 0.2

  $ ucode -lwebsocket -luloop - <<'EOF'
  > import { connect } from 'websocket';
  > import * as uloop from 'uloop';
  > uloop.init();
  > let ws = connect('ws://127.0.0.1:28979/badaccept', { timeout: 5000 });
  > ws.on('open', (w) => print('UNEXPECTED OPEN\n'));
  > ws.on('close', (w, code) => { print(`CLOSE ${code}\n`); uloop.end(); });
  > ws.on('error', (w, e) => { print('ERROR invalid handshake response\n'); uloop.end(); });
  > uloop.run();
  > print('DONE\n');
  > EOF
  ERROR invalid handshake response
  DONE

  $ wait


test handshake failure on non-101 response:

  $ $WS_FIXTURE 28980 1 &
  $ sleep 0.2

  $ ucode -lwebsocket -luloop - <<'EOF'
  > import { connect } from 'websocket';
  > import * as uloop from 'uloop';
  > uloop.init();
  > let ws = connect('ws://127.0.0.1:28980/http200', { timeout: 5000 });
  > ws.on('open', (w) => print('UNEXPECTED OPEN\n'));
  > ws.on('close', (w, code) => { print(`CLOSE ${code}\n`); uloop.end(); });
  > ws.on('error', (w, e) => { print('ERROR invalid handshake response\n'); uloop.end(); });
  > uloop.run();
  > print('DONE\n');
  > EOF
  ERROR invalid handshake response
  DONE

  $ wait


test URL validation (T12):

  $ ucode -lwebsocket -e 'import { connect } from "websocket"; connect("http://example.org/");' ; echo "rc=$?"
  Type error: Invalid WebSocket URL
  In [-e argument], line 1, byte 67:
  
   `import { connect } from "websocket"; connect("http://example.org/");`
    Near here --------------------------------------------------------^
  
  
  rc=254

  $ ucode -lwebsocket -e 'import { connect } from "websocket"; connect("ws://user@example.org/");' ; echo "rc=$?"
  Type error: Invalid WebSocket URL
  In [-e argument], line 1, byte 70:
  
   `import { connect } from "websocket"; connect("ws://user@example.org/");`
    Near here -----------------------------------------------------------^
  
  
  rc=254

  $ ucode -lwebsocket -e 'import { connect } from "websocket"; connect("ws://example.org:70000/");' ; echo "rc=$?"
  Type error: Invalid WebSocket URL
  In [-e argument], line 1, byte 71:
  
   `import { connect } from "websocket"; connect("ws://example.org:70000/");`
    Near here ------------------------------------------------------------^
  
  
  rc=254

  $ ucode -lwebsocket -e 'import { connect } from "websocket"; connect("wss://example.org/");' ; echo "rc=$?"
  Type error: TLS (wss://) is not supported yet
  In [-e argument], line 1, byte 66:
  
   `import { connect } from "websocket"; connect("wss://example.org/");`
    Near here -------------------------------------------------------^
  
  
  rc=254

  $ ucode -lwebsocket -e 'import { connect } from "websocket"; connect(123)' ; echo "rc=$?"
  Type error: URL must be a string
  In [-e argument], line 1, byte 49:
  
   `import { connect } from "websocket"; connect(123)`
    Near here --------------------------------------^
  
  
  rc=254


test IPv6 literal with port yields a correct request path (review LOW):

  $ $WS_FIXTURE 28981 1 &
  $ sleep 0.2

  $ ucode -lwebsocket -luloop - <<'EOF'
  > import { connect } from 'websocket';
  > import * as uloop from 'uloop';
  > uloop.init();
  > let ws = connect('ws://[::1]:28981/announce?v6=1', { timeout: 5000 });
  > ws.on('message', (w, data, is_text) => {
  > 	print(`MSG ${data}\n`);
  > 	w.close(1000, '');
  > });
  > ws.on('close', (w, code) => { print(`CLOSE ${code}\n`); uloop.end(); });
  > ws.on('error', (w, e) => { print(`ERROR ${e}\n`); uloop.end(); });
  > uloop.run();
  > print('DONE\n');
  > EOF
  MSG path=/announce?v6=1 host=[::1]:28981
  CLOSE 1000
  DONE

  $ wait


test on() after teardown is rejected instead of crashing (review MEDIUM):

  $ $WS_FIXTURE 28982 1 &
  $ sleep 0.2

  $ ucode -lwebsocket -luloop - <<'EOF'
  > import { connect } from 'websocket';
  > import * as uloop from 'uloop';
  > uloop.init();
  > let ws = connect('ws://127.0.0.1:28982/echo', { timeout: 5000 });
  > ws.on('open', (w) => w.close(1000, 'bye'));
  > ws.on('close', (w, code) => {
  > 	print(`CLOSE ${code}\n`);
  > 	uloop.timer(300, () => {
  > 		let rv = ws.on('message', (w2, d) => print('never\n'));
  > 		print(`ON-After-TEARDOWN ${rv}\n`);
  > 		uloop.end();
  > 	});
  > });
  > ws.on('error', (w, e) => { print(`ERROR ${e}\n`); uloop.end(); });
  > uloop.run();
  > print('DONE\n');
  > EOF
  CLOSE 1000
  ON-After-TEARDOWN null
  DONE

  $ wait


test send() from message callback surviving peer reset (review HIGH):

  $ $WS_FIXTURE 28983 1 &
  $ sleep 0.2

  $ ucode -lwebsocket -luloop - <<'EOF'
  > import { connect } from 'websocket';
  > import * as uloop from 'uloop';
  > uloop.init();
  > let ws = connect('ws://127.0.0.1:28983/msgreset', { timeout: 5000 });
  > ws.on('message', (w, data) => {
  > 	print(`MSG ${data}\n`);
  > 	w.send('ack');
  > });
  > ws.on('close', (w, code) => { print(`CLOSE ${code}\n`); uloop.end(); });
  > ws.on('error', (w, e) => { print('ERROR\n'); uloop.end(); });
  > uloop.run();
  > print('DONE\n');
  > EOF
  MSG bye
  ERROR
  CLOSE 1006
  DONE

  $ wait
