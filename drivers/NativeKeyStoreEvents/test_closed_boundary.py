"""Compile the actual service against a narrow host lifecycle test double (Test/mock, Test/host.h) and run Test/boundary.cpp.

Checks the open/answer boundaries, not real IOKit registration. Without sheng-aks every open is refused (closed boundary); with it
the user client answers the handshake/bookkeeping selectors, get_system (14), lock state (7) and device state (17, 35; decoded below
the way libaks decodes it, inline and through a prepared descriptor), the volatile keybag's selectors (8, 10, 11, 31, 32) unless
pagani-keybag=0, and rejects every other selector. test_volatile_keybag.py covers the keybag itself.
Usage: python3 -B test_closed_boundary.py   (needs a host C++ compiler: c++)
"""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parent
FLAGS = ['-std=c++17', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
         '-pthread', '-I', str(root / 'Test/mock'), '-I', str(root / 'Test')]


def run(test):
    with tempfile.TemporaryDirectory() as directory:
        binary = Path(directory) / test
        subprocess.run(['c++', *FLAGS, str(root / 'Test' / f'{test}.cpp'), '-o', str(binary)], check=True)
        return subprocess.run([str(binary)], check=True, capture_output=True, text=True).stdout.strip()


if __name__ == '__main__':
    print(run('boundary'))
