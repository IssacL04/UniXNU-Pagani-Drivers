"""Compile the actual service against the host test double (Test/mock, Test/host.h) and run Test/keybag.cpp: the volatile keybag.

RFC 3394 vectors 4.1-4.6 through the driver's wrap over the libkern AES calls (backed by a FIPS-197 reference AES), every single-bit
tampering refused; the IOConnectCallMethod contracts of the 21H461 libaks client (wrap 10, unwrap 11, configuration 31, lock 8/32)
with the buffers securityd hands over (40 bytes out for a 32-byte key, 32 back); round trips for classes 1-4 and 6-11 over the bag
aliases 0/-3/-6 and every key length 16..512, inline and through descriptors; class 12 (akpu) and unknown classes/bags, wrong sizes,
short outputs and foreign blobs refused with the codes securityd expects; the no-AES, oversize-context, self-test and random-generator
guards; zeroization at teardown; pagani-keybag=0 and =2; concurrent first use. AddressSanitizer and UBSan are on.
Usage: python3 -B test_volatile_keybag.py   (needs a host C++ compiler: c++)
"""
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_closed_boundary import run  # noqa: E402

if __name__ == '__main__':
    print(run('keybag'))
