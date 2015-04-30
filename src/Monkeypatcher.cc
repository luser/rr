/* -*- Mode: C++; tab-width: 8; c-basic-offset: 2; indent-tabs-mode: nil; -*- */

//#define DEBUGTAG "Monkeypatcher"

#include "Monkeypatcher.h"

#include "kernel_abi.h"
#include "log.h"
#include "ReplaySession.h"
#include "task.h"

using namespace rr;
using namespace std;

#include "AssemblyTemplates.h"

/**
 * RecordSession sets up an LD_PRELOAD environment variable with an entry
 * SYSCALLBUF_LIB_FILENAME_PADDED which is big enough to hold either the
 * 32-bit or 64-bit preload library file names. Immediately after exec we
 * enter this function, which patches the environment variable value with
 * the correct library name for the task's architecture.
 *
 * It's possible for this to fail if a tracee alters the LD_PRELOAD value
 * and then does an exec. That's just too bad. If we ever have to handle that,
 * we should modify the environment passed to the exec call.
 */
template <typename Arch> static void setup_preload_library_path(Task* t) {
  static_assert(sizeof(SYSCALLBUF_LIB_FILENAME_PADDED) ==
                    sizeof(SYSCALLBUF_LIB_FILENAME_32),
                "filename length mismatch");

  const char* lib_name =
      sizeof(typename Arch::unsigned_word) < sizeof(uintptr_t)
          ? SYSCALLBUF_LIB_FILENAME_32
          : SYSCALLBUF_LIB_FILENAME_PADDED;

  auto p = t->regs().sp().cast<typename Arch::unsigned_word>();
  auto argc = t->read_mem(p);
  p += 1 + argc + 1; // skip argc, argc parameters, and trailing NULL
  while (true) {
    auto envp = t->read_mem(p);
    if (!envp) {
      ASSERT(t, false) << "LD_PRELOAD not found";
      return;
    }
    string env = t->read_c_str(envp);
    if (env.find("LD_PRELOAD=") != 0) {
      ++p;
      continue;
    }
    size_t lib_pos = env.find(SYSCALLBUF_LIB_FILENAME_BASE);
    if (lib_pos == string::npos) {
      ASSERT(t, false) << SYSCALLBUF_LIB_FILENAME_BASE
          " not found in LD_PRELOAD";
      return;
    }
    size_t next_colon = env.find(':', lib_pos);
    if (next_colon != string::npos) {
      while (env[next_colon + 1] == ':' || env[next_colon + 1] == 0) {
        ++next_colon;
      }
      if (next_colon < lib_pos + sizeof(SYSCALLBUF_LIB_FILENAME_PADDED) - 1) {
        ASSERT(t, false) << "Insufficient space for " << lib_name
                         << " in LD_PRELOAD before next ':'";
        return;
      }
    }
    if (env.length() < lib_pos + sizeof(SYSCALLBUF_LIB_FILENAME_PADDED) - 1) {
      ASSERT(t, false) << "Insufficient space for " << lib_name
                       << " in LD_PRELOAD before end of string";
      return;
    }
    remote_ptr<void> dest = envp + lib_pos;
    t->write_mem(dest.cast<char>(), lib_name,
                 sizeof(SYSCALLBUF_LIB_FILENAME_PADDED) - 1);
    return;
  }
}

void Monkeypatcher::init_dynamic_syscall_patching(
    Task* t, int syscall_patch_hook_count,
    remote_ptr<struct syscall_patch_hook> syscall_patch_hooks) {
  if (syscall_patch_hook_count) {
    syscall_hooks = t->read_mem(syscall_patch_hooks, syscall_patch_hook_count);
  }
}

template <typename Arch>
static bool patch_syscall_with_hook_arch(Task* t,
                                         const syscall_patch_hook& hook);

static bool patch_syscall_with_hook_x86ish(Task* t,
                                           const syscall_patch_hook& hook) {
  // We can use the same patch code on x86 and x86-64.
  uint8_t patch[X86CallMonkeypatch::size];
  // We're patching in a relative jump, so we need to compute the offset from
  // the end of the jump to our actual destination.
  remote_ptr<uint8_t> patch_start = t->regs().ip();
  remote_ptr<uint8_t> patch_end = patch_start + sizeof(patch);
  intptr_t offset = hook.hook_address - patch_end.as_int();
  int32_t offset32 = (int32_t)offset;
  if (offset32 != offset) {
    LOG(debug) << "syscall can't be patched due to jump out of range from "
               << HEX(patch_end.as_int()) << " to " << HEX(hook.hook_address);
    return false;
  }
  X86CallMonkeypatch::substitute(patch, offset32);
  t->write_bytes(patch_start, patch);

  // pad with NOPs to the next instruction
  static const uint8_t NOP = 0x90;
  assert(syscall_instruction_length(x86) == syscall_instruction_length(x86_64));
  uint8_t nops[syscall_instruction_length(x86) + hook.next_instruction_length -
               sizeof(patch)];
  memset(nops, NOP, sizeof(nops));
  t->write_mem(patch_start + sizeof(patch), nops, sizeof(nops));
  return true;
}

template <>
bool patch_syscall_with_hook_arch<X86Arch>(Task* t,
                                           const syscall_patch_hook& hook) {
  return patch_syscall_with_hook_x86ish(t, hook);
}

template <>
bool patch_syscall_with_hook_arch<X64Arch>(Task* t,
                                           const syscall_patch_hook& hook) {
  return patch_syscall_with_hook_x86ish(t, hook);
}

static bool patch_syscall_with_hook(Task* t, const syscall_patch_hook& hook) {
  RR_ARCH_FUNCTION(patch_syscall_with_hook_arch, t->arch(), t, hook);
}

// TODO de-dup
static void advance_syscall(Task* t) {
  do {
    t->cont_syscall();
  } while (t->is_ptrace_seccomp_event() ||
           ReplaySession::is_ignored_signal(t->pending_sig()));
  assert(t->ptrace_event() == 0);
}

bool Monkeypatcher::try_patch_syscall(Task* t) {
  if (!t->vm()->syscallbuf_enabled()) {
    return false;
  }
  if (t->is_in_traced_syscall()) {
    // Never try to patch the traced-syscall in our preload library!
    return false;
  }
  if (tried_to_patch_syscall_addresses.count(t->ip().as_int())) {
    return false;
  }
  // We could examine the current syscall number and if it's not one that
  // we support syscall buffering for, refuse to patch the syscall instruction.
  // This would, on the face of it, reduce overhead since patching the
  // instruction just means a useless trip through the syscall buffering logic.
  // However, it actually wouldn't help much since we'd still to a switch
  // on the syscall number in this function instead, and due to context
  // switching costs any overhead saved would be insignificant.
  // Also, implementing that would require keeping a buffered-syscalls
  // list in sync with the preload code, which is unnecessary complexity.

  tried_to_patch_syscall_addresses.insert(t->ip().as_int());

  syscall_patch_hook dummy;
  auto next_instruction =
      t->read_mem(t->ip(), sizeof(dummy.next_instruction_bytes));
  for (auto& hook : syscall_hooks) {
    if (memcmp(next_instruction.data(), hook.next_instruction_bytes,
               hook.next_instruction_length) == 0) {
      // Get out of executing the current syscall before we patch it.

      Registers r = t->regs();
      intptr_t syscallno = r.original_syscallno();
      r.set_original_syscallno(syscall_number_for_gettid(t->arch()));
      t->set_regs(r);
      // This exits the hijacked SYS_gettid.  Now the tracee is
      // ready to do our bidding.
      advance_syscall(t);

      // Restore these regs to what they would have been just before
      // the tracee trapped at the syscall.
      r.set_original_syscallno(-1);
      r.set_syscallno(syscallno);
      r.set_ip(r.ip() - syscall_instruction_length(t->arch()));
      t->set_regs(r);

      patch_syscall_with_hook(t, hook);

      // Return to caller, which resume normal execution.
      return true;
    }
  }
  return false;
}

template <typename Arch> struct VdsoSymbols {
  vector<typename Arch::ElfSym> symbols;
  vector<char> strtab;
};

template <typename Arch> static VdsoSymbols<Arch> read_vdso_symbols(Task* t) {
  auto vdso_start = t->vm()->vdso().start;
  auto elfheader = t->read_mem(vdso_start.cast<typename Arch::ElfEhdr>());
  assert(elfheader.e_ident[EI_CLASS] == Arch::elfclass);
  assert(elfheader.e_ident[EI_DATA] == Arch::elfendian);
  assert(elfheader.e_machine == Arch::elfmachine);
  assert(elfheader.e_shentsize == sizeof(typename Arch::ElfShdr));

  auto sections_start = vdso_start + elfheader.e_shoff;
  typename Arch::ElfShdr sections[elfheader.e_shnum];
  t->read_bytes_helper(sections_start, sizeof(sections), sections);

  typename Arch::ElfShdr* dynsym = nullptr;
  typename Arch::ElfShdr* dynstr = nullptr;

  for (size_t i = 0; i < elfheader.e_shnum; ++i) {
    auto header = &sections[i];
    if (header->sh_type == SHT_DYNSYM) {
      assert(!dynsym && "multiple .dynsym sections?!");
      dynsym = header;
      continue;
    }
    if (header->sh_type == SHT_STRTAB && (header->sh_flags & SHF_ALLOC) &&
        i != elfheader.e_shstrndx) {
      assert(!dynstr && "multiple .dynstr sections?!");
      dynstr = header;
    }
  }

  if (!dynsym || !dynstr) {
    assert(0 && "Unable to locate vdso information");
  }

  assert(dynsym->sh_entsize == sizeof(typename Arch::ElfSym));
  remote_ptr<void> symbols_start = vdso_start + dynsym->sh_offset;
  size_t nsymbols = dynsym->sh_size / dynsym->sh_entsize;
  remote_ptr<void> strtab_start = vdso_start + dynstr->sh_offset;
  VdsoSymbols<Arch> result;
  result.symbols =
      t->read_mem(symbols_start.cast<typename Arch::ElfSym>(), nsymbols);
  result.strtab = t->read_mem(strtab_start.cast<char>(), dynstr->sh_size);
  return result;
}

/**
 * Return true iff |addr| points to a known |__kernel_vsyscall()|
 * implementation.
 */
static bool is_kernel_vsyscall(Task* t, remote_ptr<void> addr) {
  uint8_t impl[X86VsyscallImplementation::size];
  t->read_bytes(addr, impl);
  return X86VsyscallImplementation::match(impl);
}

/**
 * Return the address of a recognized |__kernel_vsyscall()|
 * implementation in |t|'s address space.
 */
static remote_ptr<void> locate_and_verify_kernel_vsyscall(Task* t) {
  auto syms = read_vdso_symbols<X86Arch>(t);

  remote_ptr<void> kernel_vsyscall = nullptr;
  // It is unlikely but possible that multiple, versioned __kernel_vsyscall
  // symbols will exist.  But we can't rely on setting |kernel_vsyscall| to
  // catch that case, because only one of the versioned symbols will
  // actually match what we expect to see, and the matching one might be
  // the last one.  Therefore, we have this separate flag to alert us to
  // this possbility.
  bool seen_kernel_vsyscall = false;

  for (auto& sym : syms.symbols) {
    const char* name = &syms.strtab[sym.st_name];
    if (strcmp(name, "__kernel_vsyscall") == 0) {
      assert(!seen_kernel_vsyscall);
      seen_kernel_vsyscall = true;
      // The ELF information in the VDSO assumes that the VDSO
      // is always loaded at a particular address.  The kernel,
      // however, subjects the VDSO to ASLR, which means that
      // we have to adjust the offsets properly.
      auto vdso_start = t->vm()->vdso().start;
      remote_ptr<void> candidate = sym.st_value;
      // The symbol values can be absolute or relative addresses.
      // The first part of the assertion is for absolute
      // addresses, and the second part is for relative.
      assert((candidate.as_int() & ~uintptr_t(0xfff)) == 0xffffe000 ||
             (candidate.as_int() & ~uintptr_t(0xfff)) == 0);
      uintptr_t candidate_offset = candidate.as_int() & uintptr_t(0xfff);
      candidate = vdso_start + candidate_offset;

      if (is_kernel_vsyscall(t, candidate)) {
        kernel_vsyscall = candidate;
      }
    }
  }

  return kernel_vsyscall;
}

template <typename Arch>
static void patch_at_preload_init_arch(Task* t, Monkeypatcher& patcher);

template <typename Arch> static void patch_after_exec_arch(Task* t);

template <> void patch_after_exec_arch<X86Arch>(Task* t) {
  setup_preload_library_path<X86Arch>(t);
}

// Monkeypatch x86 vsyscall hook only after the preload library
// has initialized. The vsyscall hook expects to be able to use the syscallbuf.
// Before the preload library has initialized, the regular vsyscall code
// will trigger ptrace traps and be handled correctly by rr.
template <>
void patch_at_preload_init_arch<X86Arch>(Task* t, Monkeypatcher& patcher) {
  auto params = t->read_mem(
      remote_ptr<rrcall_init_preload_params<X86Arch> >(t->regs().arg1()));
  if (!params.syscallbuf_enabled) {
    return;
  }

  auto kernel_vsyscall = locate_and_verify_kernel_vsyscall(t);
  if (!kernel_vsyscall) {
    FATAL() << "Failed to monkeypatch vdso: your __kernel_vsyscall() wasn't "
               "recognized.\n"
               "    Syscall buffering is now effectively disabled.  If you're "
               "OK with\n"
               "    running rr without syscallbuf, then run the recorder "
               "passing the\n"
               "    --no-syscall-buffer arg.\n"
               "    If you're *not* OK with that, file an issue.";
  }

  // Luckily, linux is happy for us to scribble directly over
  // the vdso mapping's bytes without mprotecting the region, so
  // we don't need to prepare remote syscalls here.
  remote_ptr<void> syscall_hook_trampoline = params.syscall_hook_trampoline;

  uint8_t patch[X86VsyscallMonkeypatch::size];
  // We're patching in a relative jump, so we need to compute the offset from
  // the end of the jump to our actual destination.
  X86VsyscallMonkeypatch::substitute(
      patch, syscall_hook_trampoline.as_int() -
                 (kernel_vsyscall + sizeof(patch)).as_int());

  t->write_bytes(kernel_vsyscall, patch);
  LOG(debug) << "monkeypatched __kernel_vsyscall to jump to "
             << HEX(syscall_hook_trampoline.as_int());

  patcher.init_dynamic_syscall_patching(t, params.syscall_patch_hook_count,
                                        params.syscall_patch_hooks);
}

// x86-64 doesn't have a convenient vsyscall-esque function in the VDSO;
// syscalls happen directly with the |syscall| instruction and manual
// syscall restarting if necessary.  Its VDSO is filled with overhead
// critical functions related to getting the time and current CPU.  We
// need to ensure that these syscalls get redirected into actual
// trap-into-the-kernel syscalls so rr can intercept them.

struct named_syscall {
  const char* name;
  int syscall_number;
};

#define S(n)                                                                   \
  { #n, X64Arch::n }
static const named_syscall syscalls_to_monkeypatch[] = {
  S(clock_gettime), S(gettimeofday), S(time),
  // getcpu isn't supported by rr, so any changes to this monkeypatching
  // scheme for efficiency's sake will have to ensure that getcpu gets
  // converted to an actual syscall so rr will complain appropriately.
  S(getcpu),
};
#undef S

// Monkeypatch x86-64 vdso syscalls immediately after exec. The vdso syscalls
// will cause replay to fail if called by the dynamic loader or some library's
// static constructors, so we can't wait for our preload library to be
// initialized. Fortunately we're just replacing the vdso code with real
// syscalls so there is no dependency on the preload library at all.
template <> void patch_after_exec_arch<X64Arch>(Task* t) {
  setup_preload_library_path<X64Arch>(t);

  auto vdso_start = t->vm()->vdso().start;

  auto syms = read_vdso_symbols<X64Arch>(t);

  for (auto& sym : syms.symbols) {
    const char* symname = &syms.strtab[sym.st_name];
    for (size_t j = 0; j < array_length(syscalls_to_monkeypatch); ++j) {
      if (strcmp(symname, syscalls_to_monkeypatch[j].name) == 0) {
        // Absolutely-addressed symbols in the VDSO claim to start here.
        static const uint64_t vdso_static_base = 0xffffffffff700000LL;
        static const uintptr_t vdso_max_size = 0xffffLL;
        uintptr_t sym_address = uintptr_t(sym.st_value);
        // The symbol values can be absolute or relative addresses.
        // The first part of the assertion is for absolute
        // addresses, and the second part is for relative.
        assert((sym_address & ~vdso_max_size) == vdso_static_base ||
               (sym_address & ~vdso_max_size) == 0);
        uintptr_t sym_offset = sym_address & vdso_max_size;
        uintptr_t absolute_address = vdso_start.as_int() + sym_offset;

        uint8_t patch[X64VsyscallMonkeypatch::size];
        uint32_t syscall_number = syscalls_to_monkeypatch[j].syscall_number;
        X64VsyscallMonkeypatch::substitute(patch, syscall_number);

        t->write_bytes(absolute_address, patch);
        LOG(debug) << "monkeypatched " << symname << " to syscall "
                   << syscalls_to_monkeypatch[j].syscall_number;
      }
    }
  }
}

template <>
void patch_at_preload_init_arch<X64Arch>(Task* t, Monkeypatcher& patcher) {
  auto params = t->read_mem(
      remote_ptr<rrcall_init_preload_params<X64Arch> >(t->regs().arg1()));
  if (!params.syscallbuf_enabled) {
    return;
  }

  patcher.init_dynamic_syscall_patching(t, params.syscall_patch_hook_count,
                                        params.syscall_patch_hooks);
}

void Monkeypatcher::patch_after_exec(Task* t) {
  ASSERT(t, 1 == t->vm()->task_set().size())
      << "Can't have multiple threads immediately after exec!";

  RR_ARCH_FUNCTION(patch_after_exec_arch, t->arch(), t);
}

void Monkeypatcher::patch_at_preload_init(Task* t) {
  ASSERT(t, 1 == t->vm()->task_set().size())
      << "TODO: monkeypatch multithreaded process";

  // NB: the tracee can't be interrupted with a signal while
  // we're processing the rrcall, because it's masked off all
  // signals.
  RR_ARCH_FUNCTION(patch_at_preload_init_arch, t->arch(), t, *this);
}
