"""Read an MSI's embedded cabinets without invoking the Windows Installer service.

Used only to unpack the hash-pinned Python Tcl/Tk component when making setup.
No product is installed or registered on the build machine.
"""
import ctypes
from ctypes import wintypes as w
from pathlib import Path
import shutil
import subprocess
import tempfile


def extract(msi, destination):
    api = ctypes.WinDLL('msi')
    handle = w.UINT()
    api.MsiOpenDatabaseW.argtypes = [w.LPCWSTR, w.LPCWSTR, ctypes.POINTER(w.UINT)]
    api.MsiDatabaseOpenViewW.argtypes = [w.UINT, w.LPCWSTR, ctypes.POINTER(w.UINT)]
    api.MsiRecordGetStringW.argtypes = [w.UINT, w.UINT, w.LPWSTR, ctypes.POINTER(w.DWORD)]
    api.MsiRecordReadStream.argtypes = [w.UINT, w.UINT, ctypes.c_void_p, ctypes.POINTER(w.DWORD)]

    def check(code):
        if code:
            raise OSError(code, 'Could not read the pinned Python Tcl/Tk MSI')

    check(api.MsiOpenDatabaseW(str(msi), None, ctypes.byref(handle)))

    def rows(query, fields):
        view = w.UINT()
        check(api.MsiDatabaseOpenViewW(handle, query, ctypes.byref(view)))
        try:
            check(api.MsiViewExecute(view, 0))
            while True:
                record = w.UINT()
                code = api.MsiViewFetch(view, ctypes.byref(record))
                if code == 259:
                    break
                check(code)
                try:
                    result = []
                    for index in range(1, fields + 1):
                        value = ctypes.create_unicode_buffer(32768)
                        count = w.DWORD(len(value))
                        check(api.MsiRecordGetStringW(record, index, value, ctypes.byref(count)))
                        result.append(value.value)
                    yield result, record
                finally:
                    api.MsiCloseHandle(record)
        finally:
            api.MsiCloseHandle(view)

    try:
        directories = {r[0]: r[1:] for r, _ in rows('SELECT `Directory`, `Directory_Parent`, `DefaultDir` FROM `Directory`', 3)}
        components = {r[0]: r[1] for r, _ in rows('SELECT `Component`, `Directory_` FROM `Component`', 2)}

        def folder(ident):
            parent, name = directories[ident]
            if not parent:
                return Path()
            name = name.split(':')[0].split('|')[-1]
            return folder(parent) / ('' if name == '.' else name)

        with tempfile.TemporaryDirectory(prefix='engage-tcl-') as temp:
            temp = Path(temp)
            for values, record in rows('SELECT `Name`, `Data` FROM `_Streams`', 1):
                name = values[0]
                if not name.endswith('.cab'):
                    continue
                cabinet = temp / Path(name).name
                with cabinet.open('wb') as output:
                    while True:
                        buf = ctypes.create_string_buffer(65536)
                        count = w.DWORD(len(buf))
                        check(api.MsiRecordReadStream(record, 2, buf, ctypes.byref(count)))
                        output.write(buf.raw[:count.value])
                        if not count.value:
                            break
                subprocess.run(['expand.exe', '-F:*', str(cabinet), str(temp)], check=True,
                               stdout=subprocess.DEVNULL)
            for values, _ in rows('SELECT `File`, `Component_`, `FileName` FROM `File`', 3):
                ident, component, name = values
                target = (destination / folder(components[component]) / name.split('|')[-1]).resolve()
                if not target.is_relative_to(destination.resolve()):
                    raise ValueError('Unsafe path in Tcl/Tk MSI')
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(temp / ident, target)
    finally:
        api.MsiCloseHandle(handle)
