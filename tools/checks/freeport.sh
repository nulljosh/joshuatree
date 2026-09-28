#!/usr/bin/env bash
# Allocate a free TCP port for QMP sockets

free_port() {
    python3 -c "import socket; s = socket.socket(); s.bind(('127.0.0.1', 0)); print(s.getsockname()[1]); s.close()"
}
