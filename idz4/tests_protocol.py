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
            assert send_command(writer, "QUIT") == "BYE"
        finally:
            if reader is not None:
                reader.close()
            if writer is not None:
                writer.close()
            server.terminate()
            try:
                server.wait(timeout=3)
            except subprocess.TimeoutExpired:
                server.kill()
                server.wait(timeout=3)

    print("IDZ4 protocol tests passed.")


if __name__ == "__main__":
    main()
