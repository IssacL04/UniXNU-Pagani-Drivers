#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Why the daemon lives where it does: walk the Sandbox platform policy of a built kernel for the exec the kernel redirects.

  check-exec-policy.py [KERNEL.macho]

xpcproxy execs a launchd job's program, so the check that decides the daemon's start is the platform policy's process-exec* with xpcproxy
as the caller. This reads the compiled policy out of Sandbox.kext's __const in the kernel (nothing of it is stored here) and evaluates
that operation for a path: a node is a filter with a "match" and an "unmatch" successor. What is known of the caller decides the path,
entitlement and "container-required" filters; a filter this script does not interpret is followed both ways, so the answer is the set of
verdicts any reading could reach. A proof therefore needs a single verdict:

  /private/var/ngpu/bin/paganidiag        deny    (the 2026-10-31 A package: the exec never happened, hence no "diag:" line)
  /private/var/factory_mount/paganidiag   allow   (PAGANI_DIAG_DAEMON)
  /usr/libexec/DumpPanic                  allow   (control: the job's own program, seen running in the 2026-10-30 10:54 run)
  /private/var/staged_system_apps/...     deny    (control: why the staged apps run only through the /Applications overlay)

process-exec-update-label (the exec's label update) and file-map-executable allow the daemon too, and sysctl-write, the daemon's
channel, has no rule at all (the operation's entry is the policy's default allow node). The compiled format as read here: a node is 8 bytes (0, filter, argument, match, unmatch; a terminal has bit 0 set in its first byte, and the second
byte holds its action, 0 allow, bit 0 deny); a path filter's argument points, in units of 8 bytes past the nodes, at a 16-bit length and
a pattern tree: a byte 0x40..0x7f is a literal run of (byte - 0x3f) characters, 0x00 the end of the string, 0x0f or 0x81..0xff opens a
group of alternatives (0x81..0xff also gives its length minus one), 0x80 0x0a ends an alternative that has siblings after it and 0x0f
0x0a the last one; matching a path means reaching such an end. A pattern with other tokens is compared by its literal runs alone.
The header and the walk are checked against facts of the format before anything is concluded. Exit 0: as above; 1: a difference;
a kernel without this policy (another release) says SKIP."""
import json
import re
import struct
import subprocess
import sys
from pathlib import Path

DEFAULT_KERNEL = Path('/Users/xiaochoumao/unixnu-13t/UniXNU/out-gpu/oneplus13t/ipados-17.7.11-21H461/oneplus13t/kernel/kernel.macho')
XPCPROXY = Path('/Users/xiaochoumao/unixnu-13t/build-userspace/a830-inputs/osmnt-21H461/usr/libexec/xpcproxy')
OPERATIONS_START = b'default\0appleevent-send\0'
HEADER = re.compile(rb'\x00\x00(..)\xbf\x07\x00\x00\x00\x00\x08\x00\x00\x00\x3f\x00', re.S)
PATH_FILTER, ENTITLEMENT_FILTER, PRESENT_FILTER = 1, 87, 30
ALLOW, DENY = 'allow', 'deny'


class Skip(Exception):
    pass


def operations(data):
    at = data.find(OPERATIONS_START)
    if at < 0:
        raise Skip('no Sandbox operation names in this kernel')
    names = []
    while True:
        end = data.index(b'\0', at)
        name = data[at:end]
        if not re.fullmatch(rb'[a-z0-9*\-]+', name):
            return names
        names.append(name.decode())
        at = end + 1


class Policy:
    def __init__(self, data):
        self.data = data
        self.names = operations(data)
        headers = [m for m in HEADER.finditer(data, data.find(OPERATIONS_START) - 0x180000, data.find(OPERATIONS_START))]
        if len(headers) != 1:
            raise Skip(f'{len(headers)} policy headers found in this kernel')
        start = headers[0].start()
        self.count = struct.unpack_from('<H', data, start + 2)[0]
        self.table = start + 0x2c
        self.nodes = start + 0x1b0
        self.strings = self.nodes + self.count * 8
        self.entries = struct.unpack_from('<191H', data, self.table)

    def node(self, index):
        return struct.unpack_from('<BBHHH', self.data, self.nodes + index * 8)

    def entry(self, operation):
        return self.entries[self.names.index(operation)]

    def string(self, argument):
        at = self.strings + argument * 8
        length = struct.unpack_from('<H', self.data, at)[0]
        return self.data[at + 2:at + 2 + length]

    def text(self, argument):
        return self.string(argument).rstrip(b'\0').decode('latin-1')


class Unreadable(Exception):
    pass


def leaves(pattern):
    """Every accepted (literal, must end there) of a pattern tree; Unreadable when it has tokens outside the tree form."""
    out = []

    def alternatives(at, end, prefix, eos):
        """The alternatives of a group whose bytes end at `end` (None: until an alternative closes it). Returns where it ended."""
        while end is None or at < end:
            at, closed = alternative(at, prefix, eos)
            if closed and end is None:
                return at
        return at

    def alternative(at, prefix, eos):
        """One alternative from `at`. Returns (the byte after it, whether it was marked as the last one of its group)."""
        while True:
            if at >= len(pattern):
                raise Unreadable
            byte = pattern[at]
            if 0x40 <= byte <= 0x7f:
                length = byte - 0x3f
                prefix += pattern[at + 1:at + 1 + length]
                at += 1 + length
            elif byte == 0:
                eos = True
                at += 1
            elif byte in (0x80, 0x0f) and at + 1 < len(pattern) and pattern[at + 1] == 0x0a:
                out.append((bytes(prefix), eos))
                return at + 2, byte == 0x0f
            elif byte == 0x0f:
                return alternatives(at + 1, None, prefix, eos), True
            elif byte > 0x80:
                end = at + 2 + (byte & 0x7f)
                return alternatives(at + 1, end, prefix, eos), False
            else:
                raise Unreadable

    alternative(0, b'', False)
    return out


def path_matches(pattern, path):
    """True or False, or None when the pattern can't be told apart for this path."""
    try:
        return any(path == literal if eos else path.startswith(literal) for literal, eos in leaves(pattern))
    except Unreadable:
        runs = []
        at = 0
        while at < len(pattern):
            if 0x40 <= pattern[at] <= 0x7f:
                length = pattern[at] - 0x3f
                runs.append(pattern[at + 1:at + 1 + length])
                at += 1 + length
            else:
                at += 1
        longest = max(runs, key=len, default=b'')
        return None if len(longest) < 4 or longest in path else False


def verdicts(policy, operation, path, entitlements):
    """The verdicts a walk of the operation can reach for a caller with these entitlements."""
    seen, found = set(), set()
    stack = [policy.entry(operation)]
    while stack:
        index = stack.pop()
        if index in seen:
            continue
        seen.add(index)
        kind, value, argument, match, unmatch = policy.node(index)
        if kind & 1:
            found.add(DENY if value & 1 else ALLOW)
        elif value == PATH_FILTER:
            result = path_matches(policy.string(argument), path.encode())
            stack += [match, unmatch] if result is None else [match if result else unmatch]
        elif value in (ENTITLEMENT_FILTER, PRESENT_FILTER):
            stack.append(match if policy.text(argument) in entitlements else unmatch)
        else:
            stack += [match, unmatch]
    return found


def signed_entitlements(path):
    import plistlib
    data = subprocess.run(['codesign', '-d', '--entitlements', ':-', str(path)], capture_output=True, check=True).stdout
    return set(plistlib.loads(data)) if data else set()


EXPECTED = {
    '/private/var/ngpu/bin/paganidiag': {DENY},
    '/private/var/factory_mount/paganidiag': {ALLOW},
    '/usr/libexec/DumpPanic': {ALLOW},
    '/private/var/containers/Bundle/Application/X/App.app/App': {ALLOW},
    '/private/var/staged_system_apps/Weather.app/Weather': {DENY},
}


def main():
    kernel = Path(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT_KERNEL
    if not kernel.is_file() or not XPCPROXY.is_file():
        print(f'SKIP: no {kernel if not kernel.is_file() else XPCPROXY}')
        return
    try:
        policy = Policy(kernel.read_bytes())
    except Skip as why:
        print(f'SKIP: {why}')
        return
    problems = []
    last = policy.node(policy.count - 1)
    if last[0] & 1 != 1 or policy.entry('default') != policy.count - 1 or last[1] & 1:
        problems.append('the default operation is not the last node, a plain allow: the format was not understood')
    if policy.entry('sysctl-write') != policy.entry('default'):
        problems.append('sysctl-write has rules')
    entitlements = signed_entitlements(XPCPROXY)
    if 'com.apple.private.xpc.is.xpcproxy' not in entitlements:
        problems.append(f'{XPCPROXY} is not xpcproxy')
    results = {}
    for path, want in EXPECTED.items():
        got = verdicts(policy, 'process-exec*', path, entitlements)
        results[path] = sorted(got)
        if got != want:
            problems.append(f'{path}: {sorted(got)}, expected {sorted(want)}')
    daemon = '/private/var/factory_mount/paganidiag'
    for operation in ('process-exec-update-label', 'file-map-executable'):
        got = verdicts(policy, operation, daemon, entitlements)
        results[operation] = sorted(got)
        if got != {ALLOW}:
            problems.append(f'{operation} of the daemon: {sorted(got)}')
    # The caller matters: with no-container (the running daemon starting its probes) nothing below /private/var is refused.
    if verdicts(policy, 'process-exec*', '/private/var/ngpu/bin/paganidiag', {'com.apple.private.security.no-container'}) != {ALLOW}:
        problems.append('the no-container caller is refused too: the entitlement filter was not understood')
    print(json.dumps(results, indent=1))
    if problems:
        sys.exit('FAIL: ' + '; '.join(problems))
    print('PASS: process-exec* refuses xpcproxy /private/var/ngpu/bin; exec, label update and mapping allow /private/var/factory_mount; '
          'sysctl-write has no rule')


if __name__ == '__main__':
    main()
