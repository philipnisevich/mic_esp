#!/usr/bin/env bash
# ArduinoWebsockets 0.5.4 returns a bare `false` from five different points in
# connect(), which makes an intermittent failure impossible to diagnose. This
# adds a wait for the upgrade response and a message at each exit.
#
# Re-run after any reinstall or upgrade of the library. Each patch is applied
# independently, so a partially-patched file is repaired rather than skipped.
set -euo pipefail

SRC="${1:-$HOME/Documents/Arduino/libraries/ArduinoWebsockets/src/websockets_client.cpp}"
[ -f "$SRC" ] || { echo "not found: $SRC" >&2; exit 1; }

python3 - "$SRC" <<'PY'
import sys
p = sys.argv[1]
s = open(p).read()
applied, skipped = [], []

def patch(name, marker, old, new):
    """marker must be text unique to the patched version - checking the first
    line of `new` is wrong, since for several of these it is unchanged code."""
    global s
    if marker in s:
        skipped.append(name); return
    if old not in s:
        skipped.append(name + " (upstream text not found)"); return
    s = s.replace(old, new, 1)
    applied.append(name)

# 1. Wait for the upgrade response. The original checks availability the instant
#    after send(), but there is a full round trip in between, and a false here
#    also closes the socket.
patch("wait-for-response",
      "ws no response to the upgrade request",
"""        // This check is needed because of an ESP32 lib bug that wont signal that the connection had
        // failed in `->connect` (called above), sometimes the disconnect will only be noticed here (after a `send`)
        if(!available()) {
            return false;
        }""",
"""        // MicScribe: wait for the upgrade response before deciding it failed.
        {
            unsigned long _deadline = millis() + 8000UL;
            while (this->_client && !this->_client->available() && millis() < _deadline) {
                delay(5);
            }
        }

        if(!available()) {
            Serial.printf("# ws no response to the upgrade request\\n");
            return false;
        }""")

# 2. The TCP/TLS connect itself
patch("connect-failure",
      "ws tcp/tls connect failed",
"""        this->_connectionOpen = this->_client->connect(internals::fromInterfaceString(host), port);
        if (!this->_connectionOpen) return false;""",
"""        // MicScribe: name the transport failure
        this->_connectionOpen = this->_client->connect(internals::fromInterfaceString(host), port);
        if (!this->_connectionOpen) {
            Serial.printf("# ws tcp/tls connect failed to %s:%d\\n",
                          internals::fromInterfaceString(host).c_str(), port);
            return false;
        }""")

# 3. Non-101 status
patch("status-line",
      "ws upgrade rejected, server said",
"""        auto head = this->_client->readLine();
        if(!doestStartsWith(head, "HTTP/1.1 101")) {
            close(CloseReason_ProtocolError);
            return false;
        }""",
"""        auto head = this->_client->readLine();
        if(!doestStartsWith(head, "HTTP/1.1 101")) {
            // MicScribe: show what the server actually returned
            Serial.printf("# ws upgrade rejected, server said: %.120s\\n", head.c_str());
            close(CloseReason_ProtocolError);
            return false;
        }""")

# 4. Truncated headers
patch("header-truncated",
      "ws header read truncated",
"""            if (line.size() < 2) {
                close(CloseReason_ProtocolError);
                return false;
            }""",
"""            if (line.size() < 2) {
                // MicScribe: connection died mid-response
                Serial.printf("# ws header read truncated\\n");
                close(CloseReason_ProtocolError);
                return false;
            }""")

# 5. Accept-key mismatch
patch("accept-mismatch",
      "ws handshake invalid",
"""        if(parsedResponse.isSuccess == false || serverAcceptMismatch) {
            close(CloseReason_ProtocolError);
            return false;
        }""",
"""        if(parsedResponse.isSuccess == false || serverAcceptMismatch) {
            // MicScribe: the handshake parsed but did not validate
            Serial.printf("# ws handshake invalid: success=%d acceptMismatch=%d\\n",
                          (int)parsedResponse.isSuccess, (int)serverAcceptMismatch);
            close(CloseReason_ProtocolError);
            return false;
        }""")

open(p, 'w').write(s)
print("applied:", ", ".join(applied) if applied else "(none)")
print("skipped:", ", ".join(skipped) if skipped else "(none)")
PY
