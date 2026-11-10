#!/usr/bin/env python3
"""Набор проверок для четырёх вариантов ИДЗ 3."""

from __future__ import annotations

import os
import re
import signal
import subprocess
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BIN = ROOT / "bin"
DATA = ROOT / "tests" / "data"
LOGS = ROOT / "logs"


def run(args: list[str], timeout: int = 30) -> str:
    result = subprocess.run(
        args,
        cwd=ROOT,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        timeout=timeout,
    )
    if result.returncode != 0:
        raise AssertionError(f"Команда завершилась с кодом {result.returncode}: {' '.join(map(str, args))}\n{result.stdout[-4000:]}")
    return result.stdout


def summary(output: str) -> tuple[int, int, int, int]:
    match = re.search(r"Final version=(\d+); reads=(\d+); publications=(\d+); stages=(\d+); aborted writes=(\d+)", output)
    if not match:
        raise AssertionError("В выводе нет итоговой статистики")
    return tuple(map(int, match.groups()))  # type: ignore[return-value]


def wait_process(process: subprocess.Popen, timeout: int = 8) -> int:
    try:
        return process.wait(timeout=timeout)
    except subprocess.TimeoutExpired:
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        process.wait(timeout=3)
        raise AssertionError(f"Процесс {process.args} не завершился вовремя")


def start_logged(args: list[str]):
    output = tempfile.TemporaryFile(mode="w+t")
    process = subprocess.Popen(args, cwd=ROOT, stdout=output, stderr=subprocess.STDOUT, start_new_session=True)
    return process, output


def read_output(handle) -> str:
    size = os.fstat(handle.fileno()).st_size
    return os.pread(handle.fileno(), size, 0).decode("utf-8", errors="replace")


def wait_for_text(handle, expected: str, timeout: float = 8.0) -> str:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        output = read_output(handle)
        if expected in output:
            return output
        time.sleep(0.01)
    raise AssertionError(f"Не дождались строки {expected!r}. Последний вывод:\n{read_output(handle)[-4000:]}")


def test_parent_modes() -> None:
    for mode in ("parent_named", "parent_unnamed"):
        out = run([str(BIN / mode), "--config", str(DATA / "basic.conf")])
        version, reads, pubs, stages, aborted = summary(out)
        assert (version, reads, pubs, stages, aborted) == (3, 4, 2, 6, 0), (mode, summary(out))
        assert "Validation passed" in out
        assert "Version 3 confirmed and published" in out
        assert "reading confirmed version 1" in out

        out = run([str(BIN / mode), "--config", str(DATA / "wait.conf")])
        version, reads, pubs, stages, aborted = summary(out)
        assert (version, reads, pubs, stages, aborted) == (7, 9, 6, 30, 0), (mode, summary(out))
        assert "Waiting for publication" in out

        out = run([str(BIN / mode), "--config", str(DATA / "single.conf")])
        assert summary(out) == (3, 2, 2, 14, 0)

    # Проверка больших границ: произведение номера стадии на размер базы не должно переполнять int.
    out = run([str(BIN / "parent_named"), "--config", str(DATA / "large.conf")], timeout=30)
    assert summary(out) == (2, 1, 1, 25000, 0)

    invalid = subprocess.run([str(BIN / "parent_named"), "--config", str(DATA / "invalid.conf")], cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    assert invalid.returncode != 0

    # Одинаковый seed и один писатель дают одинаковый результат в повторных запусках.
    first = run([str(BIN / "parent_named"), "--config", str(DATA / "basic.conf")])
    second = run([str(BIN / "parent_named"), "--config", str(DATA / "basic.conf")])
    def final_db(text: str) -> str:
        match = re.search(r"Final database:\n(.*)", text)
        assert match
        return match.group(1).strip()
    assert final_db(first) == final_db(second)


def test_cli_arguments() -> None:
    out = run([
        str(BIN / "parent_named"), "--config", str(DATA / "basic.conf"),
        "--ops", "1", "--readers", "1", "--writers", "1",
    ])
    assert summary(out) == (2, 1, 1, 3, 0)

    missing_config = subprocess.run(
        [str(BIN / "parent_named"), "--config"], cwd=ROOT,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
    )
    assert missing_config.returncode != 0

    invalid_arguments = (
        [str(BIN / "shm_reader"), "--id", "abc"],
        [str(BIN / "mq_reader"), "--ops", "1abc"],
        [str(BIN / "mq_writer"), "--id", "1x"],
    )
    for args in invalid_arguments:
        result = subprocess.run(args, cwd=ROOT, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, text=True)
        assert result.returncode != 0, (args, result.stdout)


def test_parent_interrupt() -> None:
    for mode in ("parent_named", "parent_unnamed"):
        process, output = start_logged([str(BIN / mode), "--config", str(DATA / "interrupt.conf")])
        time.sleep(0.2)
        os.killpg(process.pid, signal.SIGINT)
        assert wait_process(process) == 0
        text = read_output(output)
        assert "Stop requested from keyboard" in text
        assert "Final version=1" in text and "aborted writes=1" in text
        output.close()


def test_independent_shm() -> None:
    run([str(BIN / "shm_cleanup")])
    run([str(BIN / "shm_init"), "--config", str(DATA / "multi.conf")])
    processes = []
    handles = []
    for tool, ident in (("shm_writer", 1), ("shm_writer", 2), ("shm_reader", 1), ("shm_reader", 2)):
        p, h = start_logged([str(BIN / tool), "--id", str(ident)])
        processes.append(p); handles.append(h)
    for p in processes:
        assert wait_process(p) == 0
    status = run([str(BIN / "shm_status")])
    assert "version=17" in status and "active_workers=0" in status
    assert "reads=16" in status and "publications=16" in status and "stages=112" in status
    all_output = "".join(read_output(h) for h in handles)
    assert "Waiting for publication" in all_output
    for h in handles:
        h.close()
    run([str(BIN / "shm_cleanup")])

    # Отдельная проверка общей остановки нескольких независимо запущенных процессов.
    run([str(BIN / "shm_init"), "--config", str(DATA / "interrupt.conf")])
    workers = []
    for tool in ("shm_writer", "shm_reader"):
        p, h = start_logged([str(BIN / tool), "--id", "1"])
        workers.append((p, h))
    time.sleep(0.2)
    run([str(BIN / "shm_stop")])
    for p, h in workers:
        assert wait_process(p) == 0
        h.close()
    status = run([str(BIN / "shm_status")])
    assert "stop=1" in status and "active_workers=0" in status
    assert "version=1" in status and "aborted=1" in status
    run([str(BIN / "shm_cleanup")])


def test_independent_mq() -> None:
    run([str(BIN / "mq_cleanup")])
    server, server_output = start_logged([str(BIN / "mq_server"), "--config", str(DATA / "multi.conf")])
    time.sleep(0.2)
    clients = []
    handles = []
    for tool, ident, ops in (("mq_writer", 1, 3), ("mq_writer", 2, 3), ("mq_reader", 1, 5), ("mq_reader", 2, 5)):
        p, h = start_logged([str(BIN / tool), "--id", str(ident), "--ops", str(ops)])
        clients.append(p); handles.append(h)
    for p in clients:
        assert wait_process(p) == 0
    run([str(BIN / "mq_stop")])
    assert wait_process(server) == 0
    text = read_output(server_output)
    assert "version=7 reads=10 publications=6 stages=42 aborted=0" in text
    assert "Waiting for publication" in text
    for h in handles + [server_output]:
        h.close()
    run([str(BIN / "mq_cleanup")])

    # READ_OLD: читатель запускается после первой стадии и до публикации версии 2.
    server, server_output = start_logged([
        str(BIN / "mq_server"), "--config", str(DATA / "basic.conf"), "--prep-ms", "500",
    ])
    wait_for_text(server_output, "Сервер базы данных запущен")
    writer, writer_output = start_logged([str(BIN / "mq_writer"), "--id", "1", "--ops", "1"])
    wait_for_text(server_output, "Stage 1/3 completed for version 2")
    reader, reader_output = start_logged([str(BIN / "mq_reader"), "--id", "1", "--ops", "1"])
    assert wait_process(writer) == 0
    assert wait_process(reader) == 0
    run([str(BIN / "mq_stop")])
    assert wait_process(server) == 0
    text = read_output(server_output)
    assert "version=2 reads=1 publications=1 stages=3 aborted=0" in text
    read_position = text.index("Read confirmed version=1")
    publish_position = text.index("Version 2 validated and published")
    assert read_position < publish_position, text
    for h in (writer_output, reader_output, server_output):
        h.close()
    run([str(BIN / "mq_cleanup")])

    # Ctrl+C сервера не должен навсегда оставить клиентов в ожидании ответа.
    server, server_output = start_logged([str(BIN / "mq_server"), "--config", str(DATA / "interrupt.conf")])
    time.sleep(0.2)
    clients = []
    handles = []
    for tool in ("mq_writer", "mq_reader"):
        p, h = start_logged([str(BIN / tool), "--id", "1", "--ops", "0"])
        clients.append(p); handles.append(h)
    time.sleep(0.25)
    os.killpg(server.pid, signal.SIGINT)
    assert wait_process(server) == 0
    for p in clients:
        assert wait_process(p, timeout=6) == 0
    text = read_output(server_output)
    assert "version=1 reads=" in text and "publications=0" in text and "aborted=1" in text
    for h in handles + [server_output]:
        h.close()
    run([str(BIN / "mq_cleanup")])


def main() -> None:
    LOGS.mkdir(exist_ok=True)
    for path in LOGS.glob("*.log"):
        path.unlink()
    test_parent_modes()
    print("[OK] parent_named и parent_unnamed: обычный режим, WAIT_NEW, одна запись, большие границы, seed")
    test_cli_arguments()
    print("[OK] аргументы командной строки: несколько переопределений, обязательный --config, строгая проверка чисел")
    test_parent_interrupt()
    print("[OK] parent_named и parent_unnamed: SIGINT и завершение дочерних процессов")
    test_independent_shm()
    print("[OK] независимые процессы + POSIX shared memory + именованные семафоры")
    test_independent_mq()
    print("[OK] независимые процессы + POSIX message queue + именованные семафоры")
    print("Все проверки завершились успешно.")


if __name__ == "__main__":
    main()
