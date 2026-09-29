#!/usr/bin/env python3
"""Allocate a free TCP port for QMP sockets."""

import socket


def free_port():
    """Return a free TCP port on localhost."""
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(('127.0.0.1', 0))
        s.listen(1)
        port = s.getsockname()[1]
    return port
