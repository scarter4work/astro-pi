#!/usr/bin/env python3
"""
pi_ipc.py - Send commands to running PixInsight instance via IPC

Reverse-engineered PixInsight IPC protocol using System V shared memory.

Protocol format (null-separated key=value pairs):
  n=<seq>     - Sequence number (-1 = new request)
  c=<cmd>     - Command (execute, open, etc.)
  i=<uuid>    - Request UUID
  t=<path>    - Target file path

Usage:
    ./pi_ipc.py execute /path/to/script.js
    ./pi_ipc.py open /path/to/image.fits
"""

import argparse
import ctypes
import ctypes.util
import os
import sys
import uuid


def find_pixinsight_shm_key():
    """Find the PixInsight shared memory key from /tmp files"""
    for filename in os.listdir('/tmp'):
        if 'qipc_sharedmemory_PIXINSIGHTINSTANCE' in filename:
            # Extract the hash from the filename
            # The key is derived from this hash
            # Qt uses a hash of the key name to generate the System V key

            # We need to find the actual shmid from ipcs
            import subprocess
            result = subprocess.run(['ipcs', '-m'], capture_output=True, text=True)
            for line in result.stdout.split('\n'):
                parts = line.split()
                if len(parts) >= 6 and parts[2] == os.environ.get('USER', 'root'):
                    try:
                        key = int(parts[0], 16)
                        return key
                    except:
                        continue
    return None


def attach_shm(key):
    """Attach to shared memory segment"""
    libc = ctypes.CDLL(ctypes.util.find_library('c'))

    # Get shmid
    shmid = libc.shmget(key, 0, 0)
    if shmid == -1:
        return None, None, None

    # Attach read-write
    libc.shmat.restype = ctypes.c_void_p
    addr = libc.shmat(shmid, None, 0)
    if addr == ctypes.c_void_p(-1).value:
        return None, None, None

    return libc, shmid, addr


def read_current_message(addr):
    """Read the current message from shared memory"""
    data = ctypes.string_at(addr, 2048)

    # Find message start (after header)
    msg_start = 0xc0  # Based on our analysis
    msg_data = data[msg_start:]

    # Parse key-value pairs
    result = {}
    parts = msg_data.split(b'\x00')
    for part in parts:
        if b'=' in part:
            try:
                key, value = part.decode('utf-8').split('=', 1)
                result[key] = value
            except:
                pass
    return result


def write_message(addr, command, target_path):
    """Write a command message to shared memory"""
    request_id = str(uuid.uuid4())

    # Build message
    parts = [
        f"n=-1",
        f"c={command}",
        f"i={request_id}",
        f"t={target_path}",
    ]

    # Join with null bytes, add trailing null
    message = '\x00'.join(parts) + '\x00'
    message_bytes = message.encode('utf-8')

    # Write to shared memory at offset 0xc0
    msg_start = 0xc0

    # Clear the message area first
    clear_bytes = b'\x00' * 1024
    ctypes.memmove(addr + msg_start, clear_bytes, len(clear_bytes))

    # Write new message
    ctypes.memmove(addr + msg_start, message_bytes, len(message_bytes))

    return request_id


def signal_semaphore():
    """Signal the System V semaphore to notify PixInsight"""
    # Find semaphore from /tmp
    for filename in os.listdir('/tmp'):
        if 'qipc_systemsem_PIXINSIGHTINSTANCE' in filename:
            # Qt uses semaphores to signal new messages
            # We need to find and signal it
            import subprocess
            result = subprocess.run(['ipcs', '-s'], capture_output=True, text=True)
            for line in result.stdout.split('\n'):
                parts = line.split()
                if len(parts) >= 5 and parts[2] == os.environ.get('USER', 'root'):
                    try:
                        semid = int(parts[1])
                        # Signal the semaphore using semop
                        libc = ctypes.CDLL(ctypes.util.find_library('c'))

                        # struct sembuf { short sem_num; short sem_op; short sem_flg; }
                        class sembuf(ctypes.Structure):
                            _fields_ = [
                                ('sem_num', ctypes.c_short),
                                ('sem_op', ctypes.c_short),
                                ('sem_flg', ctypes.c_short),
                            ]

                        # Release (increment) semaphore
                        sop = sembuf(0, 1, 0)
                        result = libc.semop(semid, ctypes.byref(sop), 1)
                        return result == 0
                    except Exception as e:
                        print(f"Semaphore error: {e}")
                        continue
    return False


def main():
    parser = argparse.ArgumentParser(description='Send commands to PixInsight via IPC')
    parser.add_argument('command', choices=['execute', 'open', 'status'],
                       help='Command to send')
    parser.add_argument('path', nargs='?', help='File path (for execute/open)')
    parser.add_argument('--key', type=lambda x: int(x, 0),
                       help='Shared memory key (hex, e.g., 0x51292ac2)')
    args = parser.parse_args()

    # Find or use provided key
    shm_key = args.key
    if not shm_key:
        shm_key = find_pixinsight_shm_key()
        if not shm_key:
            print("Error: Could not find PixInsight shared memory")
            print("Is PixInsight running?")
            sys.exit(1)

    print(f"Using shared memory key: {hex(shm_key)}")

    # Attach to shared memory
    libc, shmid, addr = attach_shm(shm_key)
    if addr is None:
        print("Error: Could not attach to shared memory")
        sys.exit(1)

    print(f"Attached to shmid: {shmid}")

    if args.command == 'status':
        # Just read current state
        msg = read_current_message(addr)
        print("Current message:")
        for k, v in msg.items():
            print(f"  {k} = {v}")
    else:
        # Send command
        if not args.path:
            print(f"Error: {args.command} requires a file path")
            sys.exit(1)

        path = os.path.abspath(args.path)
        print(f"Sending: {args.command} {path}")

        request_id = write_message(addr, args.command, path)
        print(f"Request ID: {request_id}")

        # Signal the semaphore
        if signal_semaphore():
            print("Signaled semaphore")
        else:
            print("Warning: Could not signal semaphore")
            print("PixInsight may not receive the command")

    # Detach
    libc.shmdt(addr)
    print("Done!")


if __name__ == '__main__':
    main()
