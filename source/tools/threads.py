"""The scheduler's view of the game's threads: base priority, current (boosted) priority, state, what each waits for,
processor affinity, and how much processor time and how many context switches each got during the sample.
It only asks Windows (NtQuerySystemInformation, NtQueryInformationThread): nothing is asked of the game's own
threads, so it answers at once even when the game is stuck.
usage: python threads.py [pid] [--seconds 1]     (default pid: out/build/release/test.pid)"""
import ctypes, os, struct, sys, time
from ctypes import wintypes as wt

rel = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'out', 'build', 'release')
args = [a for a in sys.argv[1:] if not a.startswith('--')]
secs = float(sys.argv[sys.argv.index('--seconds') + 1]) if '--seconds' in sys.argv else 1.0
if '--seconds' in sys.argv: args = [a for a in args if a != sys.argv[sys.argv.index('--seconds') + 1]]
pid = int(args[0]) if args else int(open(os.path.join(rel, 'test.pid')).read().strip())
ntdll = ctypes.WinDLL('ntdll')
k32 = ctypes.WinDLL('kernel32', use_last_error=True)
k32.OpenThread.restype = wt.HANDLE

STATE = ['initialized', 'ready', 'running', 'standby', 'terminated', 'waiting', 'transition', 'deferred ready', 'gate wait']
WAIT = {0: 'Executive', 1: 'FreePage', 2: 'PageIn', 3: 'PoolAllocation', 4: 'DelayExecution', 5: 'Suspended', 6: 'UserRequest',
        7: 'WrExecutive', 8: 'WrFreePage', 9: 'WrPageIn', 10: 'WrPoolAllocation', 11: 'WrDelayExecution', 12: 'WrSuspended',
        13: 'WrUserRequest', 15: 'WrQueue', 16: 'WrLpcReceive', 17: 'WrLpcReply', 18: 'WrVirtualMemory', 19: 'WrPageOut',
        20: 'WrRendezvous', 27: 'WrResource', 28: 'WrPushLock', 29: 'WrMutex', 30: 'WrQuantumEnd', 31: 'WrDispatchInt',
        32: 'WrPreempted', 33: 'WrYieldExecution', 34: 'WrFastMutex', 35: 'WrGuardedMutex', 36: 'WrRundown',
        37: 'WrAlertByThreadId', 38: 'WrDeferredPreempt'}

def snapshot():
    size = wt.ULONG(1 << 20)
    while True:
        buf = ctypes.create_string_buffer(size.value)
        need = wt.ULONG(0)
        st = ntdll.NtQuerySystemInformation(5, buf, size, ctypes.byref(need))
        if st == 0: break
        size = wt.ULONG(max(need.value, size.value) + (1 << 16))
    raw = buf.raw
    off = 0
    while True:
        nxt, count = struct.unpack_from('<II', raw, off)
        if struct.unpack_from('<Q', raw, off + 80)[0] == pid:
            out = {}
            for i in range(count):
                t = off + 256 + i * 80
                kt, ut = struct.unpack_from('<qq', raw, t)
                tid = struct.unpack_from('<Q', raw, t + 48)[0]
                pri, base, cs, state, reason = struct.unpack_from('<iiIII', raw, t + 56)
                out[tid] = (kt / 1e4, ut / 1e4, pri, base, cs, state, reason)
            return out
        if not nxt: return None
        off += nxt

def affinity(tid):
    h = k32.OpenThread(0x0800, False, tid)  # query limited information
    if not h: return None
    info = ctypes.create_string_buffer(48)  # THREAD_BASIC_INFORMATION: AffinityMask at 32
    ok = ntdll.NtQueryInformationThread(wt.HANDLE(h), 0, info, 48, None) == 0
    k32.CloseHandle(wt.HANDLE(h))
    return struct.unpack_from('<Q', info.raw, 32)[0] if ok else None

a = snapshot()
if a is None: sys.exit('process %d is not running' % pid)
time.sleep(secs)
b = snapshot() or a
cpus = os.cpu_count() or 64
full = (1 << cpus) - 1
print('process %d, %d threads, sample %.1f s' % (pid, len(b), secs))
print('   tid  base  now  state           waits for          processors  cpu ms in sample  switches  total cpu s')
for tid, (kt, ut, pri, base, cs, state, reason) in sorted(b.items(), key=lambda x: -(x[1][0] + x[1][1])):
    old = a.get(tid)
    used = (kt + ut) - (old[0] + old[1]) if old else 0
    sw = cs - old[4] if old else 0
    aff = affinity(tid)
    where = 'all' if aff is None or aff & full == full else ','.join(str(i) for i in range(cpus) if aff >> i & 1)
    print('%6d  %4d %4d  %-14s  %-17s  %-10s  %16.0f  %8d  %11.1f' % (
        tid, base, pri, STATE[state] if state < len(STATE) else state,
        WAIT.get(reason, reason) if state == 5 else '', where, used, sw, (kt + ut) / 1000))
