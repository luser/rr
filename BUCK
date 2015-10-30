# To build the shared library, run `buck build //:rrpreload#default,shared`
# This is lame, sdwilsh should fix it.
cxx_library(
    name = "rrpreload",
    srcs = [
        "src/preload/preload.c",
        "src/preload/traced_syscall.S",
        "src/preload/untraced_syscall.S",
        "src/preload/syscall_hook.S",
    ],
    headers = glob(["src/preload/*.h"]),
    deps = [
      ":headers",
      ":system_libs",
    ],
)

GENERATED_FILES = [
    "AssemblyTemplates.h",
    "CheckSyscallNumbers.h",
    "IsAlwaysEmulatedSyscall.h",
    "SyscallDefsTable.h",
    "SyscallEnumsX64.h",
    "SyscallEnumsX86.h",
    "SyscallEnumsForTestsX64.h",
    "SyscallEnumsForTestsX86.h",
    "SyscallHelperFunctions.h",
    "SyscallnameArch.h",
    "SyscallRecordCase.h",
]

python_library(
    name = 'generate_syscalls_imports',
    srcs = [
        'src/assembly_templates.py',
        'src/syscalls.py'
    ],
)

python_binary(
    name = 'generate_syscalls',
    main = 'src/generate_syscalls.py',
    deps = [':generate_syscalls_imports']
)

for generated in GENERATED_FILES:
    genrule(
        name = 'gen_%s' % generated,
        cmd = '$(exe :generate_syscalls) $OUT',
        out = generated,
    )

cxx_library(
    name='headers',
    exported_headers = {
      'rr/rr.h': 'include/rr/rr.h',
      'preload/preload_interface.h': 'src/preload/preload_interface.h',
    },
)

prebuilt_cxx_library(
    name='system_libs',
    header_only = True,
    exported_linker_flags = [
        "-ldl",
        "-lm",
        "-lpthread",
        "-lrt",
        "-lz",
    ]
)

cxx_binary(
    name="rr",
    compiler_flags = [
        "-std=c++0x",
        "-pthread",
        "-O0",
        "-g3",
        "-Wall",
        "-Werror",
    ],
    preprocessor_flags = [
        "-std=c++0x",
        "-D__USE_LARGEFILE64",
        "-D__STDC_LIMIT_MACROS",
        "-D__STDC_FORMAT_MACROS",
        "-DRR_VERSION=\"3.2.0\"",
        "-I include",
    ],
    srcs = [
        "src/test/cpuid_loop.S",
        "src/AddressSpace.cc",
        "src/AutoRemoteSyscalls.cc",
        "src/Command.cc",
        "src/CompressedReader.cc",
        "src/CompressedWriter.cc",
        "src/CPUIDBugDetector.cc",
        "src/DiversionSession.cc",
        "src/DumpCommand.cc",
        "src/EmuFs.cc",
        "src/Event.cc",
        "src/ExtraRegisters.cc",
        "src/fast_forward.cc",
        "src/FdTable.cc",
        "src/Flags.cc",
        "src/GdbConnection.cc",
        "src/GdbServer.cc",
        "src/HelpCommand.cc",
        "src/kernel_abi.cc",
        "src/kernel_metadata.cc",
        "src/log.cc",
        "src/MagicSaveDataMonitor.cc",
        "src/main.cc",
        "src/Monkeypatcher.cc",
        "src/PerfCounters.cc",
        "src/PsCommand.cc",
        "src/RecordCommand.cc",
        "src/RecordSession.cc",
        "src/record_signal.cc",
        "src/record_syscall.cc",
        "src/Registers.cc",
        "src/ReplayCommand.cc",
        "src/ReplaySession.cc",
        "src/replay_syscall.cc",
        "src/ReplayTimeline.cc",
        "src/Scheduler.cc",
        "src/Session.cc",
        "src/StdioMonitor.cc",
        "src/task.cc",
        "src/TraceFrame.cc",
        "src/TraceStream.cc",
        "src/util.cc",
    ],
    headers = [':gen_%s' % g for g in GENERATED_FILES] + glob(["src/*.h"]),
    deps = [
        ':headers',
        ':system_libs',
    ],
)

# This doesn't work yet.
for target, dest in [('rr', 'bin/rr'), ('rrpreload#default,shared', 'lib/librrpreload.so')]:
    realtarget = ':' + target
    genrule(
        name = 'copy_%s' % target.replace('#', '_'),
        cmd = 'cp $(location %s) $OUT' % realtarget,
        out = dest,
    )
