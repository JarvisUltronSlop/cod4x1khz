"""Windows WDDM GPU engine counters, sampled out of the gameplay processes.

Uses the busiest engine for each PID, like Task Manager. This is engine busy
time, not a percentage of shader throughput or a sum across parallel engines.
"""
import ctypes as ct
from ctypes import wintypes as wt
import re


class CounterUnion(ct.Union):
    _fields_ = [("double", ct.c_double), ("large", ct.c_longlong)]


class CounterValue(ct.Structure):
    _fields_ = [("status", wt.DWORD), ("value", CounterUnion)]


class CounterItem(ct.Structure):
    _fields_ = [("name", wt.LPWSTR), ("formatted", CounterValue)]


class GpuSampler:
    def __init__(self, pids):
        self.pids = set(pids)
        self.query = wt.HANDLE()
        self.counter = wt.HANDLE()
        self.error = None
        self.pdh = ct.WinDLL("pdh")
        self.pdh.PdhOpenQueryW.argtypes = [wt.LPCWSTR, ct.c_size_t, ct.POINTER(wt.HANDLE)]
        self.pdh.PdhAddEnglishCounterW.argtypes = [wt.HANDLE, wt.LPCWSTR, ct.c_size_t, ct.POINTER(wt.HANDLE)]
        self.pdh.PdhCollectQueryData.argtypes = [wt.HANDLE]
        self.pdh.PdhGetFormattedCounterArrayW.argtypes = [
            wt.HANDLE, wt.DWORD, ct.POINTER(wt.DWORD), ct.POINTER(wt.DWORD), ct.c_void_p]
        self.pdh.PdhCloseQuery.argtypes = [wt.HANDLE]
        for name in ("PdhOpenQueryW", "PdhAddEnglishCounterW", "PdhCollectQueryData",
                     "PdhGetFormattedCounterArrayW", "PdhCloseQuery"):
            getattr(self.pdh, name).restype = wt.DWORD
        try:
            self._check(self.pdh.PdhOpenQueryW(None, 0, ct.byref(self.query)))
            self._check(self.pdh.PdhAddEnglishCounterW(
                self.query, r"\GPU Engine(*)\Utilization Percentage", 0, ct.byref(self.counter)))
            # Utilization counters require two collections; prime before sampling.
            self._check(self.pdh.PdhCollectQueryData(self.query))
        except OSError as error:
            self.error = str(error)
            self.close()

    @staticmethod
    def _check(status):
        if status:
            raise OSError(f"GPU PDH counter status 0x{status:08x}")

    def sample(self):
        if self.error:
            return {"available": False, "error": self.error}
        try:
            self._check(self.pdh.PdhCollectQueryData(self.query))
            size, count = wt.DWORD(), wt.DWORD()
            # PDH_FMT_DOUBLE | PDH_FMT_NOCAP100: preserve the reported value.
            format_flags = 0x200 | 0x8000
            status = self.pdh.PdhGetFormattedCounterArrayW(
                self.counter, format_flags, ct.byref(size), ct.byref(count), None)
            if status != 0x800007d2:  # PDH_MORE_DATA
                self._check(status)
            for _ in range(3):
                buffer = ct.create_string_buffer(size.value)
                status = self.pdh.PdhGetFormattedCounterArrayW(
                    self.counter, format_flags, ct.byref(size), ct.byref(count), buffer)
                if status != 0x800007d2:
                    break
            self._check(status)
            items = ct.cast(buffer, ct.POINTER(CounterItem))
            processes = {str(pid): {"busiest_engine_percent": None, "engines": {}} for pid in self.pids}
            for i in range(count.value):
                item = items[i]
                if item.formatted.status not in (0, 1):
                    continue
                match = re.match(r"pid_(\d+)_(.+)", item.name)
                if not match or int(match[1]) not in self.pids:
                    continue
                value = max(0.0, item.formatted.value.double)
                process = processes[match[1]]
                process["engines"][match[2]] = value
                previous = process["busiest_engine_percent"]
                process["busiest_engine_percent"] = value if previous is None else max(previous, value)
            return {"available": True, "processes": processes}
        except OSError as error:
            return {"available": False, "error": str(error)}

    def close(self):
        if self.query:
            self.pdh.PdhCloseQuery(self.query)
            self.query = wt.HANDLE()
