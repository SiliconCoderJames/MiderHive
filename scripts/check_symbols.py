#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Can this executable be debugged at source level? Ask DbgHelp.

"Build X is debuggable" should be a checked statement, not an assumption: a
Release build has no PDB at all, and a PDB that belongs to another build is
silently rejected by debuggers. This loads the image from disk through the same
symbol engine every Windows debugger uses (no process is started) and reports
what it resolved:

  symbol type       PDB / exports / none
  pdb unmatched     True if the PDB does not belong to this exact build
  breakpoint test   the source file and line DbgHelp maps a function to

Exit code: 0 = matching PDB with line numbers; 1 = not debuggable at source
level; 2 = could not run.

Usage (repository root or anywhere):
  python scripts/check_symbols.py dist/miderhive.exe [--fragment digestRunOnce]

DbgHelp is taken from the Windows SDK's Debugging Tools when they are installed
(C:\\Program Files (x86)\\Windows Kits\\10\\Debuggers\\x64); otherwise the in-box
one is tried. Reads only the executable and the PDB next to it.
"""
import ctypes
import os
import sys
from ctypes import (POINTER, Structure, WINFUNCTYPE, byref, c_int, c_ulong, c_ulonglong,
                    c_void_p, c_wchar, c_wchar_p)

SDK_DBGHELP_DIR = r"C:\Program Files (x86)\Windows Kits\10\Debuggers\x64"
SYMOPT_UNDNAME, SYMOPT_LOAD_LINES, SYMOPT_EXACT_SYMBOLS = 0x2, 0x10, 0x4
SYM_TYPE = {0: "none", 1: "COFF", 2: "CodeView", 3: "PDB", 4: "exports", 5: "deferred",
            6: ".sym", 7: "DIA", 8: "virtual"}
MAX_NAME = 400


class MODULEINFOW64(Structure):
    _fields_ = [("SizeOfStruct", c_ulong), ("BaseOfImage", c_ulonglong), ("ImageSize", c_ulong),
                ("TimeDateStamp", c_ulong), ("CheckSum", c_ulong), ("NumSyms", c_ulong),
                ("SymType", c_int), ("ModuleName", c_wchar * 32), ("ImageName", c_wchar * 256),
                ("LoadedImageName", c_wchar * 256), ("LoadedPdbName", c_wchar * 256),
                ("CVSig", c_ulong), ("CVData", c_wchar * (260 * 3)), ("PdbSig", c_ulong),
                ("PdbSig70", c_ulong * 4), ("PdbAge", c_ulong), ("PdbUnmatched", c_int),
                ("DbgUnmatched", c_int), ("LineNumbers", c_int), ("GlobalSymbols", c_int),
                ("TypeInfo", c_int), ("SourceIndexed", c_int), ("Publics", c_int),
                ("MachineType", c_int), ("Reserved", c_int)]


class LINEW64(Structure):
    _fields_ = [("SizeOfStruct", c_ulong), ("Key", c_void_p), ("LineNumber", c_ulong),
                ("FileName", c_wchar_p), ("Address", c_ulonglong)]


class SYMBOLINFOW(Structure):
    _fields_ = [("SizeOfStruct", c_ulong), ("TypeIndex", c_ulong), ("Reserved", c_ulonglong * 2),
                ("Index", c_ulong), ("Size", c_ulong), ("ModBase", c_ulonglong),
                ("Flags", c_ulong), ("Value", c_ulonglong), ("Address", c_ulonglong),
                ("Register", c_ulong), ("Scope", c_ulong), ("Tag", c_ulong),
                ("NameLen", c_ulong), ("MaxNameLen", c_ulong), ("Name", c_wchar * MAX_NAME)]


def load_dbghelp():
    if os.path.isdir(SDK_DBGHELP_DIR):
        os.add_dll_directory(SDK_DBGHELP_DIR)
        try:
            return ctypes.WinDLL(os.path.join(SDK_DBGHELP_DIR, "dbghelp.dll"))
        except OSError:
            pass
    return ctypes.WinDLL("dbghelp.dll")  # the in-box copy


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    fragment = "main"
    if "--fragment" in sys.argv:
        fragment = sys.argv[sys.argv.index("--fragment") + 1]
        args = [a for a in args if a != fragment]
    if len(args) != 1:
        print(__doc__.split("Usage", 1)[1].split("DbgHelp is taken", 1)[0].strip())
        return 2
    exe = os.path.abspath(args[0])
    if not os.path.isfile(exe):
        print("not found: %s" % exe)
        return 2
    folder = os.path.dirname(exe)
    try:
        dh = load_dbghelp()
    except OSError as e:
        print("DbgHelp could not be loaded (%s). Install the Windows SDK feature "
              "'Debugging Tools for Windows', or check the symbols in a debugger." % e)
        return 2

    dh.SymSetOptions.argtypes, dh.SymSetOptions.restype = [c_ulong], c_ulong
    dh.SymInitializeW.argtypes, dh.SymInitializeW.restype = [c_void_p, c_wchar_p, c_int], c_int
    dh.SymLoadModuleExW.argtypes = [c_void_p, c_void_p, c_wchar_p, c_wchar_p, c_ulonglong,
                                    c_ulong, c_void_p, c_ulong]
    dh.SymLoadModuleExW.restype = c_ulonglong
    dh.SymGetModuleInfoW64.argtypes = [c_void_p, c_ulonglong, POINTER(MODULEINFOW64)]
    dh.SymGetModuleInfoW64.restype = c_int
    callback_type = WINFUNCTYPE(c_int, POINTER(SYMBOLINFOW), c_ulong, c_void_p)
    dh.SymEnumSymbolsW.argtypes = [c_void_p, c_ulonglong, c_wchar_p, callback_type, c_void_p]
    dh.SymEnumSymbolsW.restype = c_int
    dh.SymGetLineFromAddrW64.argtypes = [c_void_p, c_ulonglong, POINTER(c_ulong),
                                         POINTER(LINEW64)]
    dh.SymGetLineFromAddrW64.restype = c_int
    dh.SymCleanup.argtypes = [c_void_p]

    # EXACT_SYMBOLS: a PDB that does not belong to this exact build is rejected,
    # so "loaded" below really means "matches".
    dh.SymSetOptions(SYMOPT_UNDNAME | SYMOPT_LOAD_LINES | SYMOPT_EXACT_SYMBOLS)
    process = c_void_p(0x5A5A5A5A)  # any unique non-handle value: no real process is involved
    if not dh.SymInitializeW(process, folder, 0):
        print("SymInitialize failed: Win32 error %d" % ctypes.GetLastError())
        return 2
    try:
        base = dh.SymLoadModuleExW(process, None, exe, None, 0, 0, None, 0)
        if not base:
            print("SymLoadModuleEx failed: Win32 error %d" % ctypes.GetLastError())
            return 2
        info = MODULEINFOW64()
        info.SizeOfStruct = ctypes.sizeof(MODULEINFOW64)
        if not dh.SymGetModuleInfoW64(process, base, byref(info)):
            print("SymGetModuleInfo failed: Win32 error %d" % ctypes.GetLastError())
            return 2
        kind = SYM_TYPE.get(info.SymType, str(info.SymType))
        print("image        : %s" % exe)
        print("symbol type  : %s" % kind)
        if info.LoadedPdbName:
            print("pdb          : %s" % info.LoadedPdbName)
            print("pdb unmatched: %s" % bool(info.PdbUnmatched))
            print("line numbers : %s" % bool(info.LineNumbers))
        if info.SymType != 3:
            print("RESULT: NOT debuggable at source level (no matching PDB). "
                  "Build with RelWithDebInfo, or deploy the PDB next to the exe.")
            return 1

        found = []

        @callback_type
        def on_symbol(sym, _size, _ctx):
            s = sym.contents
            name = s.Name[:s.NameLen]
            # Skip the internals of std:: templates: they resolve to <functional>,
            # which proves nothing about this code base.
            if fragment in name and "std::" not in name and len(found) < 2:
                line = LINEW64()
                line.SizeOfStruct = ctypes.sizeof(LINEW64)
                displacement = c_ulong(0)
                if dh.SymGetLineFromAddrW64(process, s.Address, byref(displacement), byref(line)):
                    found.append((name[:70], line.FileName, line.LineNumber))
            return 1

        dh.SymEnumSymbolsW(process, base, None, on_symbol, None)
        if not found:
            print("RESULT: symbols load, but no function whose name contains %r was found "
                  "(try --fragment)" % fragment)
            return 1
        print("  breakpoint test (functions whose name contains %r):" % fragment)
        for name, file_name, line_no in found:
            print("    %s" % name)
            print("      -> %s:%d" % (file_name, line_no))
        print("RESULT: debuggable at source level (matching PDB, line numbers resolve)")
        return 0
    finally:
        dh.SymCleanup(process)


if __name__ == "__main__":
    sys.exit(main())
