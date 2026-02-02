#!/usr/bin/env python3
# -*- coding: utf-8 -*-
#
# base.py
#
# Copyright (C) 2024 Ray Lee <hburaylee@gmail.com>
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 2 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.

import subprocess
import sys
import os
import socket
import time
import shutil

class Colors:
    GREEN = '\033[92m'
    RED = '\033[91m'
    ENDC = '\033[0m'

def traverse_kernels(base_dir):
    kernels_dic = {}

    for root, dirs, files in os.walk(base_dir):
        if root != base_dir:
            continue
        for dir_name in dirs:
            kernels_dic[dir_name] = []
            dir_path = os.path.join(root, dir_name)
            for file_name in os.listdir(dir_path):
                file_path = os.path.join(dir_path, file_name)
                if file_name.startswith("System.map") or file_name.startswith("vmlinuz"):
                    kernels_dic[dir_name].append(file_path)
    return kernels_dic

def kvmdmesg_test(sysmap_path):
    kvmdmesg_path = "../kvm-dmesg"
    if not os.path.exists(sysmap_path):
        print(f"Error: {sysmap_path} does not exist.")
        return False

    if not os.path.exists(kvmdmesg_path):
        print(f"Error: kvm-dmesg does not exist.")
        return False

    command = [
        kvmdmesg_path,
        sysmap_path,
        '/tmp/qmp.sock'
    ]

    try:
        print(f"Running command: {' '.join(command)}")
        process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE)

        log_output = ""
        for _ in range(30):
            line_bytes = process.stdout.readline()
            if not line_bytes:
                break
            line = line_bytes.decode('utf-8')
            log_output += line
            print(line, end="")
            if " Linux version " in line:
                print("\n" + Colors.GREEN + "Test ok!" + Colors.ENDC + "\n")
                return True

        print("\n" + Colors.RED + "Test failed." + Colors.ENDC + "\n")

        process.terminate()
        return False

    except subprocess.CalledProcessError as e:
        print(f"Error occurred while starting kvm-dmesg: {e}")
        return False

def find_qemu():
    qemu_paths_search = [
            "/bin/qemu-system-x86_64",
            "/usr/bin/qemu-system-x86_64",
            "/usr/local/bin/qemu-system-x86_64",
            "/usr/libexec/qemu-kvm"
            ]
    qemu_path = None
    for p in qemu_paths_search:
        if os.path.exists(p):
            qemu_path = p
            break
    if qemu_path:
        return qemu_path

    qemu_path = shutil.which('qemu-system-x86_64')
    if qemu_path:
        return qemu_path
    qemu_path = shutil.which('qemu-kvm')
    return qemu_path if qemu_path else None

def qemu_run(vmlinux_path):
    if not os.path.exists(vmlinux_path):
        print(f"Error: {vmlinux_path} does not exist.")
        return False

    qemu_path = find_qemu()
    if not qemu_path:
        print("QEMU not found.")
        return False

    command = [
        qemu_path,
        '-kernel', vmlinux_path,
        '-nographic',
        '-no-reboot',
        '-append', 'console=ttyS0',
        '-m', '4G',
        '-smp', '1',
        '-qmp', 'unix:/tmp/qmp.sock,server,nowait',
        '-monitor', 'unix:/tmp/mon.sock,server,nowait'
    ]

    if os.path.exists('/dev/kvm') and os.environ.get('CI') != 'true':
        command.extend(['-accel', 'kvm', '-cpu', 'host'])

    try:
        print(f"Running command: {' '.join(command)}")
        process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE)

        kernel_log_output = ""
        for _ in range(30):
            line_bytes = process.stdout.readline()
            if not line_bytes:
                break
            line = line_bytes.decode('utf-8')
            kernel_log_output += line
            print(line, end="")
            if " Linux version " in line:
                print("\nKernel booted successfully!")
                return True

        print("\n" + Colors.RED + "Kernel did not boot successfully." + Colors.ENDC)
        process.terminate()
        return False

    except subprocess.CalledProcessError as e:
        print(f"Error occurred while starting QEMU: {e}")
        return False

def qemu_shutdown():
    try:
        sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        sock.connect('/tmp/mon.sock')

        shutdown_command = "quit\n"
        sock.sendall(shutdown_command.encode())

        time.sleep(1)
        sock.close()
    except Exception as e:
        print(f"Connect monitor error: {e}")

def test_all(kernels_dic):
    nr_tests = len(kernels_dic)
    nr_passed = 0
    for key, files in kernels_dic.items():
        print(f"=== {key} ===")
        sysmap_path = ''
        vmlinux_path = ''
        for f in files:
            if "System.map" in f:
                sysmap_path = f
            if "vmlinuz" in f:
                vmlinux_path = f
        if len(sysmap_path) and len(vmlinux_path):
            if qemu_run(vmlinux_path):
                time.sleep(10)
                if (kvmdmesg_test(sysmap_path)):
                    nr_passed += 1
                qemu_shutdown()
    current_time = time.localtime()
    formatted_time = time.strftime('%Y-%m-%d %H:%M:%S', current_time)
    if nr_passed == nr_tests:
        print("\n" + Colors.GREEN + formatted_time + f" Tests {nr_passed}/{nr_tests} passed!" + Colors.ENDC)
        sys.exit(0)
    elif nr_passed < nr_tests:
        nr_failed = nr_tests - nr_passed
        print("\n" + Colors.RED + formatted_time + f" Tests {nr_failed}/{nr_tests} failed." + Colors.ENDC)


if __name__ == "__main__":
    kernels = traverse_kernels("%s/kvm-dmesg-ci/kernels" % os.getcwd())
    test_all(kernels)
    sys.exit(1)
