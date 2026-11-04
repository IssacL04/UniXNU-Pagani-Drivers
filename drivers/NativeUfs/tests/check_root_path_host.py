#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Bounded ASan/UBSan checks of production root layout and window reads."""
import argparse
import os
from pathlib import Path
import subprocess

parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--output',required=True,type=Path)
parser.add_argument('--openssl-prefix',type=Path)
parser.add_argument('--root-image',type=Path,help='Optional existing image: read only its first 4096 bytes')
parser.add_argument('--gpt-dir',type=Path,help='Optional PKX110 GPT dumps (sda..sdf_主GPT.bin) for PartitionTable')
args=parser.parse_args()
source=Path(__file__).resolve().parent
out=args.output.resolve();out.mkdir(parents=True,exist_ok=True)
openssl=args.openssl_prefix or Path(subprocess.check_output(['brew','--prefix','openssl@3'],text=True).strip())
for name in ('RootLayout','RootWindowRead','RootDiagnostics','RootPublication','PartitionTable'):
    binary=out/name
    command=['clang++','-std=c++17','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-g',str(source/(name+'.cpp')),'-o',str(binary)]
    if name=='RootLayout':command+=['-I',str(openssl/'include'),'-L',str(openssl/'lib'),'-lcrypto']
    subprocess.run(command,check=True,timeout=60)
    command=[str(binary)]
    if name=='RootLayout' and args.root_image:command.append(str(args.root_image.resolve()))
    if name=='PartitionTable' and args.gpt_dir:command.append(str(args.gpt_dir.resolve()))
    with (out/(name+'.log')).open('w') as log:subprocess.run(command,check=True,timeout=20,stdout=log,stderr=subprocess.STDOUT)
    print('PASS:',name)
# The real command path as the kernel builds it (read-only but for the diagnostic log), and as a read-only image without that switch.
kernel=['-DNATIVE_UFS_SM8750=1','-DNATIVE_UFS_READ_ONLY=1']
for name,source_name,flags in (('RootCommands','RootCommands',kernel+['-DNATIVE_UFS_LOG_WRITES=1']),
                               ('RootCommandsReadOnly','RootCommands',kernel),
                               ('LogWindow','LogWindow',kernel+['-DNATIVE_UFS_LOG_WRITES=1']),
                               ('LogWindowReadOnly','LogWindow',kernel),
                               ('LogWindowPlain','LogWindow',['-DNATIVE_UFS_SM8750=1'])):
    binary=out/name
    subprocess.run(['clang++','-std=c++17','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-g',*flags,str(source/(source_name+'.cpp')),'-o',str(binary)],check=True,timeout=60)
    with (out/(name+'.log')).open('w') as log:subprocess.run([str(binary)],check=True,timeout=20,stdout=log,stderr=subprocess.STDOUT)
    print('PASS:',name)
# LogInterface.h on the same path, and the actual writer over it (BootProgress/Test/ufslog-e2e.c): that object is C, the rest C++.
writer=out/'ufslog-e2e.o'
subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-g','-c',str(source.parent.parent/'BootProgress/Test/ufslog-e2e.c'),'-o',str(writer)],check=True,timeout=60)
binary=out/'LogInterface'
subprocess.run(['clang++','-std=c++17','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-g',*kernel,'-DNATIVE_UFS_LOG_WRITES=1',str(source/'LogInterface.cpp'),str(writer),'-o',str(binary)],check=True,timeout=60)
for arguments in ([],['--off']):
    with (out/'LogInterface.log').open('a') as log:subprocess.run([str(binary),*arguments],check=True,timeout=60,stdout=log,stderr=subprocess.STDOUT,env=dict(os.environ,ASAN_OPTIONS='detect_leaks=0'))
print('PASS: LogInterface')
with (out/'media-diagnostics.txt').open('w') as log:
    subprocess.run([str(out/'RootDiagnostics'),'--media'],check=True,timeout=20,stdout=log,stderr=subprocess.STDOUT)
print('PASS: maximum-width diagnostics plus eight media entries prepared for actual renderer')
