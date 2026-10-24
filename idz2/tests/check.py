from pathlib import Path
import re
import signal
import subprocess
import sys
import time

root = Path(__file__).resolve().parents[1]
exe = root / "idz2"
if not exe.exists():
    print("Сначала выполните make")
    sys.exit(1)

cases = {"old": (4, 2), "wait": (12, 8), "multi": (9, 12), "single": (3, 3)}
for name, (reads, writes) in cases.items():
    data = (root / "data" / f"{name}.txt").read_text()
    for attempt in range(3):
        p = subprocess.run([str(exe)], input=data, capture_output=True, text=True, timeout=15)
        out = p.stdout
        assert p.returncode == 0 and "Simulation finished" in out, f"{name}: ошибка завершения"
        assert f"Reads: {reads}\nPublications: {writes}" in out, f"{name}: неверное число операций"
        assert f"Final version: {writes + 1}" in out, f"{name}: неверная версия"
        assert out.count("Started preparing version") == writes, f"{name}: неполный запуск писателей"
        assert out.count(" published") == writes, f"{name}: неверное число публикаций"
        if name == "wait":
            assert "being prepared, reading confirmed version" not in out, "WAIT_NEW: чтение во время подготовки"
        for items in re.findall(r"Database: \[([^\]]+)\]", out):
            arr = list(map(int, items.split()))
            assert arr == sorted(arr) and all(x > 0 for x in arr), f"{name}: неверное содержимое базы"
    print(f"{name}: OK")

data = (root / "data" / "wait.txt").read_text().splitlines()
data[8] = "0"
data[13] = "10"
data[14] = "10"
data[15] = "10"
p = subprocess.Popen([str(exe)], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
p.stdin.write("\n".join(data) + "\n")
p.stdin.flush()
time.sleep(0.2)
p.send_signal(signal.SIGINT)
out, err = p.communicate(timeout=15)
assert p.returncode == 0 and "Stopped by user" in out, "SIGINT: неверное завершение"
print("SIGINT: OK")
print("Все тесты пройдены")
