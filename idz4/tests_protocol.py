#!/usr/bin/env python3
"""Проверка границ текстового TCP-протокола и отмены подготовки."""

import socket
import subprocess
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent


def receive_line(connection):
    data = bytearray()
    while True:
        chunk = connection.recv(1)
        if not chunk:
            raise RuntimeError("соединение закрыто до получения ответа")
        if chunk == b"\n":
            return data.decode("utf-8")
        if chunk != b"\r":
            data.extend(chunk)


def send_command(connection, command):
    connection.sendall(command.encode("utf-8") + b"\n")
    return receive_line(connection)


def connect_client(port, role, identifier):
    connection = socket.create_connection(("127.0.0.1", port), timeout=3)
    response = send_command(connection, f"HELLO {role} {identifier}")
    assert response.startswith("WELCOME "), response
    return connection


def main():
    with tempfile.TemporaryDirectory() as temporary_directory:
        with socket.socket() as probe:
            probe.bind(("127.0.0.1", 0))
            port = probe.getsockname()[1]

        server = subprocess.Popen(
            [str(ROOT / "bin/idz4"), "server", "--bind", "127.0.0.1",
             "--port", str(port), "--strategy", "WAIT_NEW", "--size", "1",
             "--stages", "1", "--min", "1", "--max", "9",
             "--read-ms", "0", "--prep-ms", "0", "--confirm-ms", "0"],
            cwd=temporary_directory,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        writer = None
        reader = None
        writer_a = None
        writer_b = None
        observer_a = None
        observer_b = None
        try:
            for _ in range(50):
                if server.poll() is not None:
                    raise RuntimeError("сервер завершился при запуске")
                try:
                    writer = connect_client(port, "writer", 10)
                    break
                except OSError:
                    time.sleep(0.05)
            assert writer is not None, "сервер не начал принимать соединения"
            reader = connect_client(port, "reader", 11)

            assert send_command(writer, "BEGIN 77 trailing") == "ERR command"
            assert send_command(writer, "BEGIN 77").startswith("OK BEGIN ")
            assert send_command(writer, "STAGE -1") == "ERR stage"
            assert send_command(writer, "STAGE 0 trailing") == "ERR stage"
            assert send_command(reader, "READ 0 trailing") == "ERR index-or-role"

            reader.sendall(b"READ 0\n")
            time.sleep(0.05)
            assert send_command(writer, "ABORT") == "OK ABORT"
            response = receive_line(reader)
            assert response.startswith("DATA 1 0 "), response

            assert send_command(reader, "QUIT") == "BYE"
            assert reader.recv(1) == b""
            assert send_command(writer, "QUIT") == "BYE"
            assert writer.recv(1) == b""

            # The server rejects trailing handshake data.
            malformed = socket.create_connection(("127.0.0.1", port), timeout=3)
            malformed.sendall(b"HELLO reader 99 extra\\n")
            assert receive_line(malformed) == "ERR handshake"
            assert malformed.recv(1) == b""
            malformed.close()

            # Only one writer may own an unfinished version.
            writer_a = connect_client(port, "writer", 12)
            writer_b = connect_client(port, "writer", 13)
            observer_a = connect_client(port, "observer", 20)
            observer_b = connect_client(port, "observer", 21)
            assert send_command(writer_a, "BEGIN 100").startswith("OK BEGIN ")
            assert send_command(writer_b, "BEGIN 101") == "ERR busy"
            status = send_command(observer_a, "STATUS")
            assert "writing=1 writer=12" in status, status
            writer_a.close()
            writer_a = None

            # A disconnected writer must release the lock and preserve v1.
            for _ in range(50):
                status = send_command(observer_b, "STATUS")
                if "writing=0 writer=-1" in status:
                    break
                time.sleep(0.02)
            assert "version=1" in status and "writing=0 writer=-1" in status, status

            assert send_command(writer_b, "BEGIN 102").startswith("OK BEGIN ")
            assert send_command(writer_b, "STAGE 0") == "OK STAGE 0"
            assert send_command(writer_b, "PUBLISH") == "OK PUBLISHED 2"
            assert "version=2" in send_command(observer_a, "STATUS")
            assert send_command(observer_a, "QUIT") == "BYE"
            assert observer_a.recv(1) == b""
            assert send_command(observer_b, "QUIT") == "BYE"
            assert observer_b.recv(1) == b""
            assert send_command(writer_b, "QUIT") == "BYE"
            assert writer_b.recv(1) == b""
            observer_a.close()
            observer_b.close()
            writer_b.close()
        finally:
            if reader is not None:
                reader.close()
            if writer is not None:
                writer.close()
            for connection in (writer_a, writer_b, observer_a, observer_b):
                if connection is not None:
                    connection.close()
            server.terminate()
            try:
                server.wait(timeout=3)
            except subprocess.TimeoutExpired:
                server.kill()
                server.wait(timeout=3)

    print("IDZ4 protocol tests passed.")


if __name__ == "__main__":
    main()
