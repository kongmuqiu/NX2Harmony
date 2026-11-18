# Load-aware build scheduling for this PC (Windows host; also works inside Linux containers).
#
# How many compile / rewrite jobs to run, chosen from what the machine has free right now instead of a fixed number:
#   cpu  - logical processors minus the busy share measured over a short sample (GetSystemTimes on Windows,
#          /proc/stat on Linux), minus a reserve so the desktop stays responsive;
#   mem  - free physical memory / the peak one job needs (clang -O2 on the largest Vulkan backend file: ~0.6 GB).
# Jobs also start below normal priority (Windows BELOW_NORMAL_PRIORITY_CLASS, Linux nice 10): the scheduler then serves
# interactive programs first, and on hybrid CPUs (Intel Thread Director) background work tends to the efficiency cores.
# MNG_JOBS=<n> overrides the choice.
import ctypes
import os
import subprocess
import sys
import time

RESERVE_CPUS = 2          # logical processors left for the desktop
JOB_MEM_BYTES = 600 << 20  # one clang -O2 job at peak (vk_draw.c is the largest unit)


def _cpu_busy_windows(sample):
    class FT(ctypes.Structure):
        _fields_ = [('lo', ctypes.c_uint32), ('hi', ctypes.c_uint32)]

    def times():
        idle, kern, user = FT(), FT(), FT()
        ctypes.windll.kernel32.GetSystemTimes(ctypes.byref(idle), ctypes.byref(kern), ctypes.byref(user))
        v = lambda f: (f.hi << 32) | f.lo
        return v(idle), v(kern) + v(user)   # kernel time includes idle time

    i0, t0 = times(); time.sleep(sample); i1, t1 = times()
    total = t1 - t0
    return 0.0 if total <= 0 else max(0.0, min(1.0, 1.0 - (i1 - i0) / total))


def _cpu_busy_linux(sample):
    def times():
        f = open('/proc/stat').readline().split()[1:]
        v = list(map(int, f))
        return v[3] + v[4], sum(v)   # idle + iowait, total
    i0, t0 = times(); time.sleep(sample); i1, t1 = times()
    total = t1 - t0
    return 0.0 if total <= 0 else max(0.0, min(1.0, 1.0 - (i1 - i0) / total))


def _mem_free():
    if sys.platform == 'win32':
        class MS(ctypes.Structure):
            _fields_ = [('len', ctypes.c_uint32), ('load', ctypes.c_uint32), ('total', ctypes.c_uint64), ('avail', ctypes.c_uint64),
                        ('tpage', ctypes.c_uint64), ('apage', ctypes.c_uint64), ('tvirt', ctypes.c_uint64), ('avirt', ctypes.c_uint64),
                        ('ext', ctypes.c_uint64)]
        m = MS(); m.len = ctypes.sizeof(MS)
        ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(m))
        return m.avail
    for line in open('/proc/meminfo'):
        if line.startswith('MemAvailable:'):
            return int(line.split()[1]) << 10
    return 0


def pick_jobs(job_mem=JOB_MEM_BYTES, sample=0.5, cap=None):
    """(jobs, detail) for the machine as it is now."""
    if os.environ.get('MNG_JOBS'):
        n = max(1, int(os.environ['MNG_JOBS']))
        return n, f'MNG_JOBS={n}'
    cpus = os.cpu_count() or 1
    busy = _cpu_busy_windows(sample) if sys.platform == 'win32' else _cpu_busy_linux(sample)
    free_cpus = int(cpus * (1.0 - busy)) - RESERVE_CPUS
    free_mem = _mem_free()
    by_mem = int(free_mem // job_mem) if free_mem else cpus
    n = max(1, min(free_cpus, by_mem, cap or cpus))
    return n, f'{n} jobs: {cpus} logical CPUs {busy * 100:.0f}% busy, {free_mem / (1 << 30):.1f} GB free'


def low_priority():
    """subprocess keyword arguments that start a child below normal priority."""
    if sys.platform == 'win32':
        return {'creationflags': subprocess.BELOW_NORMAL_PRIORITY_CLASS}
    return {'preexec_fn': lambda: os.nice(10)}


if __name__ == '__main__':
    n, detail = pick_jobs()
    print(n if sys.argv[1:] == ['--jobs'] else detail)
