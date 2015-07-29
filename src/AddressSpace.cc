/* -*- Mode: C++; tab-width: 8; c-basic-offset: 2; indent-tabs-mode: nil; -*- */

//#define DEBUGTAG "AddressSpace"

#include "AddressSpace.h"

#include <limits.h>
#include <linux/kdev_t.h>
#include <sys/stat.h>
#include <unistd.h>

#include <limits>

#include "rr/rr.h"

#include "preload/preload_interface.h"

#include "AutoRemoteSyscalls.h"
#include "log.h"
#include "RecordSession.h"
#include "Session.h"
#include "task.h"

using namespace rr;
using namespace std;

/*static*/ ino_t MappableResource::nr_anonymous_maps;

/*static*/ const uint8_t AddressSpace::breakpoint_insn;

void HasTaskSet::insert_task(Task* t) {
  LOG(debug) << "adding " << t->tid << " to task set " << this;
  tasks.insert(t);
}

void HasTaskSet::erase_task(Task* t) {
  LOG(debug) << "removing " << t->tid << " from task group " << this;
  tasks.erase(t);
}

ostream& operator<<(ostream& o, const KernelMapping& m) {
  o << static_cast<const MemoryRange&>(m) << " " << HEX(m.prot())
    << " f:" << HEX(m.flags());
  return o;
}

static PseudoDevice pseudodevice_for_name(const string& name) {
  if ("[heap]" == name) {
    return PSEUDODEVICE_HEAP;
  }
  if ("[stack" == name.substr(0, 6)) {
    return PSEUDODEVICE_STACK;
  }
  if ("[vdso]" == name) {
    return PSEUDODEVICE_VDSO;
  }
  if ("" == name || "/dev/zero (deleted)" == name) {
    return PSEUDODEVICE_ANONYMOUS;
  }
  if ("/SYSV" == name.substr(0, 5)) {
    return PSEUDODEVICE_SYSV_SHM;
  }
  return PSEUDODEVICE_NONE;
}

/*static*/ MappableResource MappableResource::shared_mmap_file(
    const KernelMapping& km) {
  return MappableResource(km.device(), km.inode(),
                          pseudodevice_for_name(km.fsname()) ==
                                  PSEUDODEVICE_SYSV_SHM
                              ? PSEUDODEVICE_SYSV_SHM
                              : PSEUDODEVICE_SHARED_MMAP_FILE);
}

/*static*/ MappableResource MappableResource::syscallbuf(pid_t tid, int fd) {
  struct stat st;
  if (fstat(fd, &st)) {
    FATAL() << "Failed to fstat(" << fd << ")";
  }
  return MappableResource(st.st_dev, st.st_ino, PSEUDODEVICE_NONE);
}

/**
 * Advance *str to skip leading blank characters.
 */
static const char* trim_leading_blanks(const char* str) {
  const char* trimmed = str;
  while (isblank(*trimmed)) {
    ++trimmed;
  }
  return trimmed;
}

/**
 * The following helper is used to iterate over a tracee's memory
 * map.
 */
class KernelMapIterator {
public:
  KernelMapIterator(Task* t) : t(t) {
    char maps_path[PATH_MAX];
    sprintf(maps_path, "/proc/%d/maps", t->tid);
    ASSERT(t, (maps_file = fopen(maps_path, "r"))) << "Failed to open "
                                                   << maps_path;
  }
  ~KernelMapIterator() { fclose(maps_file); }
  bool next(KernelMapping* result, string* raw_line = nullptr);

private:
  Task* t;
  FILE* maps_file;
};

bool KernelMapIterator::next(KernelMapping* result, string* raw_line) {
  char line[PATH_MAX * 2];
  if (!fgets(line, sizeof(line), maps_file)) {
    return false;
  }

  uint64_t start, end, offset, inode;
  int dev_major, dev_minor;
  char flags[32];
  int chars_scanned;
  int nparsed = sscanf(line, "%" SCNx64 "-%" SCNx64 " %31s %" SCNx64
                             " %x:%x %" SCNu64 " %n",
                       &start, &end, flags, &offset, &dev_major, &dev_minor,
                       &inode, &chars_scanned);
  ASSERT(t, 8 /*number of info fields*/ == nparsed ||
                7 /*num fields if name is blank*/ == nparsed);

  // trim trailing newline, if any
  int last_char = strlen(line) - 1;
  if (line[last_char] == '\n') {
    line[last_char] = 0;
  }
  if (raw_line) {
    *raw_line = line;
  }

  const char* name = trim_leading_blanks(line + chars_scanned);
#if defined(__i386__)
  if (start > numeric_limits<uint32_t>::max() ||
      end > numeric_limits<uint32_t>::max() ||
      strcmp(name, "[vsyscall]") == 0) {
    // We manually read the exe link here because
    // this helper is used to set
    // |t->vm()->exe_image()|, so we can't rely on
    // that being correct yet.
    char proc_exe[PATH_MAX];
    char exe[PATH_MAX];
    snprintf(proc_exe, sizeof(proc_exe), "/proc/%d/exe", t->tid);
    readlink(proc_exe, exe, sizeof(exe));
    FATAL() << "Sorry, tracee " << t->tid << " has x86-64 image " << exe
            << " and that's not supported with a 32-bit rr.";
  }
#endif
  int prot = (strchr(flags, 'r') ? PROT_READ : 0) |
             (strchr(flags, 'w') ? PROT_WRITE : 0) |
             (strchr(flags, 'x') ? PROT_EXEC : 0);
  int f = (strchr(flags, 'p') ? MAP_PRIVATE : 0) |
          (strchr(flags, 's') ? MAP_SHARED : 0);

  *result = KernelMapping(start, end, name, MKDEV(dev_major, dev_minor), inode,
                          prot, f, offset);
  return true;
}

/**
 * Cat the /proc/[t->tid]/maps file to stdout, line by line.
 */
static void print_process_mmap(Task* t) {
  KernelMapIterator it(t);
  KernelMapping km;
  string line;
  while (it.next(&km, &line)) {
    cerr << line << '\n';
  }
}

AddressSpace::~AddressSpace() { session_->on_destroy(this); }

void AddressSpace::after_clone() { allocate_watchpoints(); }

static remote_ptr<void> find_rr_vdso(Task* t, size_t* len) {
  KernelMapIterator it(t);
  KernelMapping km;
  while (it.next(&km)) {
    if (km.fsname() == "[vdso]") {
      *len = km.size();
      ASSERT(t, uint32_t(*len) == *len) << "VDSO more than 4GB???";
      return km.start();
    }
  }
  ASSERT(t, false) << "rr VDSO not found?";
  return nullptr;
}

static uint32_t find_offset_of_syscall_instruction_in(SupportedArch arch,
                                                      uint8_t* vdso_data,
                                                      size_t vdso_len) {
  auto instruction = syscall_instruction(arch);
  for (uint32_t i = 1; i < vdso_len - instruction.size(); ++i) {
    if (memcmp(vdso_data + i, instruction.data(), instruction.size()) == 0) {
      return i;
    }
  }
  return 0;
}

uint32_t AddressSpace::offset_to_syscall_in_vdso[SupportedArch_MAX + 1];

remote_code_ptr AddressSpace::find_syscall_instruction(Task* t) {
  SupportedArch arch = t->arch();
  if (!offset_to_syscall_in_vdso[arch]) {
    auto vdso_data = t->read_mem(vdso().start().cast<uint8_t>(), vdso().size());
    offset_to_syscall_in_vdso[arch] = find_offset_of_syscall_instruction_in(
        arch, vdso_data.data(), vdso_data.size());
    ASSERT(t, offset_to_syscall_in_vdso[arch])
        << "No syscall instruction found in VDSO";
  }
  return remote_code_ptr((vdso().start().cast<uint8_t>() +
                          offset_to_syscall_in_vdso[arch]).as_int());
}

static void write_rr_page(Task* t, ScopedFd& fd) {
  switch (t->arch()) {
    case x86: {
      static const uint8_t x86_data[32] = {
        0x90, 0x90, // padding
        // rr_page_untraced_syscall_ip:
        0xcd, 0x80, // int 0x80
        // rr_page_ip_in_untraced_syscall:
        0xc3, // ret
        0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
        0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, // padding
        // rr_page_traced_syscall_ip:
        0xcd, 0x80, // int 0x80
        // rr_page_ip_in_traced_syscall:
        0xc3, // ret
        0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90
      };
      ASSERT(t, sizeof(x86_data) == write(fd, x86_data, sizeof(x86_data)));
      break;
    }
    case x86_64:
      // See Task::did_waitpid for an explanation of why we have to
      // modify R11 and RCX here.
      static const uint8_t x86_64_data[32] = {
        0x90, 0x90, // padding
        // rr_page_untraced_syscall_ip:
        0x0f, 0x05, // syscall
        // rr_page_ip_in_untraced_syscall:
        0x49, 0x81, 0xe3, 0xff,
        0xfe, 0xff, 0xff, // and $0xfffffffffffffeff,%r11
        0x48, 0xc7, 0xc1, 0xff,
        0xff, 0xff, 0xff, // mov $-1,%rcx
        0xc3,             // ret
        0x90, 0x90, 0x90, // padding
        // rr_page_traced_syscall_ip:
        0x0f, 0x05, // syscall
        // rr_page_ip_in_traced_syscall:
        0xc3, // ret
        0x90, 0x90, 0x90, 0x90,
        0x90, 0x90, 0x90
      };
      ASSERT(t, sizeof(x86_64_data) ==
                    write(fd, x86_64_data, sizeof(x86_64_data)));
      break;
  }
}

void AddressSpace::map_rr_page(Task* t) {
  // We initialize the rr page by creating a temporary file with the data
  // and mapping it PROT_READ | PROT_EXEC. There are simpler ways to do it,
  // but this is the only way allowed by PaX.
  char path[] = "/tmp/rr-page-XXXXXX";
  ScopedFd fd(mkstemp(path));
  ASSERT(t, fd.is_open());
  // Write two copies of the page so we get both unpriviledged and privileged
  // entry points. Privileged entry points are in the second copy.
  write_rr_page(t, fd);
  write_rr_page(t, fd);

  int prot = PROT_EXEC | PROT_READ;
  int flags = MAP_PRIVATE | MAP_FIXED;

  Task::FStatResult fstat;

  {
    AutoRemoteSyscalls remote(t);
    SupportedArch arch = t->arch();

    AutoRestoreMem child_path(remote, reinterpret_cast<uint8_t*>(path),
                              sizeof(path));
    // skip leading '/' since we want the path to be relative to the root fd
    int child_fd =
        remote.syscall(syscall_number_for_openat(arch), RR_RESERVED_ROOT_DIR_FD,
                       child_path.get() + 1, O_RDONLY);
    ASSERT(t, child_fd >= 0);

    auto result = remote.mmap_syscall(rr_page_start(), rr_page_size(), prot,
                                      flags, child_fd, 0);
    ASSERT(t, result == rr_page_start());

    fstat = t->fstat(child_fd);

    remote.syscall(syscall_number_for_close(arch), child_fd);
  }

  unlink(path);

  map(rr_page_start(), rr_page_size(), prot, flags, 0,
      MappableResource(fstat.st.st_dev, fstat.st.st_ino, PSEUDODEVICE_NONE),
      fstat.file_name);

  untraced_syscall_ip_ = rr_page_untraced_syscall_ip(t->arch());
  traced_syscall_ip_ = rr_page_traced_syscall_ip(t->arch());
  privileged_untraced_syscall_ip_ =
      rr_page_privileged_untraced_syscall_ip(t->arch());
  privileged_traced_syscall_ip_ =
      rr_page_privileged_traced_syscall_ip(t->arch());
}

template <typename Arch> static vector<uint8_t> read_auxv_arch(Task* t) {
  auto stack_ptr = t->regs().sp().cast<typename Arch::unsigned_word>();

  auto argc = t->read_mem(stack_ptr);
  stack_ptr += argc + 1;

  // Check final NULL in argv
  auto null_ptr = t->read_mem(stack_ptr);
  assert(null_ptr == 0);
  stack_ptr++;

  // Should now point to envp
  while (0 != t->read_mem(stack_ptr)) {
    stack_ptr++;
  }
  stack_ptr++;
  // should now point to ELF Auxiliary Table

  vector<uint8_t> result;
  while (true) {
    auto pair_vec = t->read_mem(stack_ptr, 2);
    stack_ptr += 2;
    typename Arch::unsigned_word pair[2] = { pair_vec[0], pair_vec[1] };
    result.resize(result.size() + sizeof(pair));
    memcpy(result.data() + result.size() - sizeof(pair), pair, sizeof(pair));
    if (pair[0] == 0) {
      break;
    }
  }
  return result;
}

static vector<uint8_t> read_auxv(Task* t) {
  RR_ARCH_FUNCTION(read_auxv_arch, t->arch(), t);
}

void AddressSpace::save_auxv(Task* t) { saved_auxv_ = read_auxv(t); }

void AddressSpace::post_exec_syscall(Task* t) {
  // First locate a syscall instruction we can use for remote syscalls.
  traced_syscall_ip_ = find_syscall_instruction(t);
  privileged_traced_syscall_ip_ = nullptr;
  untraced_syscall_ip_ = nullptr;
  privileged_untraced_syscall_ip_ = nullptr;
  // Now remote syscalls work, we can open_mem_fd.
  t->open_mem_fd();
  // Now we can set up the "rr page" at its fixed address. This gives
  // us traced and untraced syscall instructions at known, fixed addresses.
  map_rr_page(t);
  monkeypatcher().patch_after_exec(t);
}

void AddressSpace::brk(remote_ptr<void> addr) {
  LOG(debug) << "brk(" << addr << ")";

  assert(heap.start() <= addr);

  remote_ptr<void> vm_addr = ceil_page_size(addr);
  if (heap.end() < vm_addr) {
    map(heap.end(), vm_addr - heap.end(), heap.prot(), heap.flags(),
        heap.file_offset_bytes(), MappableResource::heap(), "[heap]");
  } else {
    unmap(vm_addr, heap.end() - vm_addr);
  }
  update_heap(heap.start(), vm_addr);
}

void AddressSpace::dump() const {
  fprintf(stderr, "  (heap: %p-%p)\n", (void*)heap.start().as_int(),
          (void*)heap.end().as_int());
  for (auto it = mem.begin(); it != mem.end(); ++it) {
    const KernelMapping& m = it->second.map;
    fprintf(stderr, "%s\n", m.str().c_str());
  }
}

TrapType AddressSpace::get_breakpoint_type_for_retired_insn(
    remote_code_ptr ip) {
  remote_code_ptr addr = ip.decrement_by_bkpt_insn_length(SupportedArch::x86);
  return get_breakpoint_type_at_addr(addr);
}

TrapType AddressSpace::get_breakpoint_type_at_addr(remote_code_ptr addr) {
  auto it = breakpoints.find(addr);
  return it == breakpoints.end() ? TRAP_NONE : it->second.type();
}

void AddressSpace::replace_breakpoints_with_original_values(
    uint8_t* dest, size_t length, remote_ptr<uint8_t> addr) {
  for (auto& it : breakpoints) {
    remote_ptr<uint8_t> bkpt_location = it.first.to_data_ptr<uint8_t>();
    remote_ptr<uint8_t> start = max(addr, bkpt_location);
    remote_ptr<uint8_t> end =
        min(addr + length, bkpt_location + it.second.data_length());
    if (start < end) {
      memcpy(dest + (start - addr),
             it.second.original_data() + (start - bkpt_location), end - start);
    }
  }
}

static void remove_range(set<MemoryRange>& ranges, const MemoryRange& range) {
  auto start = ranges.lower_bound(range);
  auto end = start;
  auto prev_end = start;
  while (end != ranges.end() && end->start() < range.end()) {
    prev_end = end;
    ++end;
  }
  if (start == end) {
    return;
  }
  MemoryRange start_range = *start;
  MemoryRange end_range = *prev_end;
  ranges.erase(start, end);
  if (start_range.start() < range.start()) {
    ranges.insert(MemoryRange(start_range.start(), range.start()));
  }
  if (range.end() < end_range.end()) {
    ranges.insert(MemoryRange(range.end(), end_range.end()));
  }
}

static void add_range(set<MemoryRange>& ranges, const MemoryRange& range) {
  // Remove overlapping ranges
  remove_range(ranges, range);
  ranges.insert(range);
  // We could coalesce adjacent ranges, but there's probably no need.
}

KernelMapping AddressSpace::map(remote_ptr<void> addr, size_t num_bytes,
                                int prot, int flags, off64_t offset_bytes,
                                const MappableResource& res,
                                const string& fsname,
                                const KernelMapping* recorded_map) {
  LOG(debug) << "mmap(" << addr << ", " << num_bytes << ", " << HEX(prot)
             << ", " << HEX(flags) << ", " << HEX(offset_bytes);
  num_bytes = ceil_page_size(num_bytes);
  KernelMapping m(addr, addr + num_bytes, fsname, res.device, res.inode, prot,
                  flags, offset_bytes);
  if (!num_bytes) {
    return m;
  }

  remove_range(dont_fork, MemoryRange(addr, num_bytes));

  bool insert_guard_page = false;
  if (has_mapping(m.end()) && (mapping_of(m.end()).flags() & MAP_GROWSDOWN)) {
    // When inserting a mapping immediately before a grow-down VMA,
    // the kernel unmaps an extra page to form a guard page. We need to
    // emulate that.
    insert_guard_page = true;
  }

  if (mem.end() != mem.find(m)) {
    // The mmap() man page doesn't specifically describe
    // what should happen if an existing map is
    // "overwritten" by a new map (of the same resource).
    // In testing, the behavior seems to be as if the
    // overlapping region is unmapped and then remapped
    // per the arguments to the second call.
    unmap_internal(addr, num_bytes + (insert_guard_page ? page_size() : 0));
  }

  if (flags & MAP_GROWSDOWN) {
    // The first page is made into a guard page by the kernel
    m = KernelMapping(addr + page_size(), addr + num_bytes, fsname, res.device,
                      res.inode, prot, flags, offset_bytes + page_size());
  }
  map_and_coalesce(m, recorded_map ? *recorded_map : m, res.psdev);

  if ((prot & PROT_EXEC) &&
      (fsname.find(SYSCALLBUF_LIB_FILENAME) != string::npos ||
       fsname.find(SYSCALLBUF_LIB_FILENAME_32) != string::npos)) {
    syscallbuf_lib_start_ = addr;
    syscallbuf_lib_end_ = addr + num_bytes;
  }

  // During an emulated exec, we explicitly map in a (copy of) the VDSO
  // at the recorded address.
  if (res.psdev == PSEUDODEVICE_VDSO) {
    vdso_start_addr = addr;
  }

  return m;
}

template <typename Arch> void AddressSpace::at_preload_init_arch(Task* t) {
  auto params = t->read_mem(
      remote_ptr<rrcall_init_preload_params<Arch> >(t->regs().arg1()));

  ASSERT(t, !t->session().as_record() ||
                t->session().as_record()->use_syscall_buffer() ==
                    params.syscallbuf_enabled)
      << "Tracee thinks syscallbuf is "
      << (params.syscallbuf_enabled ? "en" : "dis")
      << "abled, but tracer thinks "
      << (t->session().as_record()->use_syscall_buffer() ? "en" : "dis")
      << "abled";

  if (!params.syscallbuf_enabled) {
    return;
  }

  monkeypatch_state.patch_at_preload_init(t);
}

void AddressSpace::at_preload_init(Task* t) {
  ASSERT(t, syscallbuf_lib_start_)
      << "should have found preload library already";
  RR_ARCH_FUNCTION(at_preload_init_arch, t->arch(), t);
}

const AddressSpace::Mapping& AddressSpace::mapping_of(
    remote_ptr<void> addr) const {
  MemoryRange range(floor_page_size(addr), page_size());
  auto it = mem.find(range);
  assert(it != mem.end());
  assert(it->second.map.contains(range));
  return it->second;
}

bool AddressSpace::has_mapping(remote_ptr<void> addr) const {
  if (addr + page_size() < addr) {
    // Assume the last byte in the address space is never mapped; avoid overflow
    return false;
  }
  MemoryRange m(floor_page_size(addr), page_size());
  auto it = mem.find(m);
  return it != mem.end() && it->first.contains(m);
}

void AddressSpace::protect(remote_ptr<void> addr, size_t num_bytes, int prot) {
  LOG(debug) << "mprotect(" << addr << ", " << num_bytes << ", " << HEX(prot)
             << ")";

  MemoryRange last_overlap;
  auto protector =
      [this, prot, &last_overlap](const Mapping& mm, const MemoryRange& rem) {
    LOG(debug) << "  protecting (" << rem << ") ...";

    Mapping m = move(mm);
    mem.erase(m.map);

    // PROT_GROWSDOWN means that if this is a grows-down segment
    // (which for us means "stack") then the change should be
    // extended to the start of the segment.
    // We don't try to handle the analogous PROT_GROWSUP, because we
    // don't understand the idea of a grows-up segment.
    remote_ptr<void> new_start;
    if ((m.map.start() < rem.start()) && (prot & PROT_GROWSDOWN) &&
        (m.flags() & MAP_GROWSDOWN)) {
      new_start = m.map.start();
      LOG(debug) << "  PROT_GROWSDOWN: expanded region down to " << new_start;
    } else {
      new_start = rem.start();
    }

    LOG(debug) << "  erased (" << m.map << ")";

    // If the first segment we protect underflows the
    // region, remap the underflow region with previous
    // prot.
    if (m.map.start() < new_start) {
      Mapping underflow(
          m.map.subrange(m.map.start(), rem.start()),
          m.recorded_map.subrange(m.recorded_map.start(), rem.start()),
          m.pseudodevice());
      mem[underflow.map] = underflow;
    }
    // Remap the overlapping region with the new prot.
    remote_ptr<void> new_end = min(rem.end(), m.map.end());

    int new_prot = prot & (PROT_READ | PROT_WRITE | PROT_EXEC);
    Mapping overlap(
        m.map.subrange(new_start, new_end).set_prot(new_prot),
        m.recorded_map.subrange(new_start, new_end).set_prot(new_prot),
        m.pseudodevice());
    mem[overlap.map] = overlap;
    last_overlap = overlap.map;

    // If the last segment we protect overflows the
    // region, remap the overflow region with previous
    // prot.
    if (rem.end() < m.map.end()) {
      Mapping overflow(m.map.subrange(rem.end(), m.map.end()),
                       m.recorded_map.subrange(rem.end(), m.map.end()),
                       m.pseudodevice());
      mem[overflow.map] = overflow;
    }
  };
  for_each_in_range(addr, num_bytes, protector, ITERATE_CONTIGUOUS);
  // All mappings that we altered which might need coalescing
  // are adjacent to |last_overlap|.
  coalesce_around(mem.find(last_overlap));
}

void AddressSpace::remap(remote_ptr<void> old_addr, size_t old_num_bytes,
                         remote_ptr<void> new_addr, size_t new_num_bytes) {
  LOG(debug) << "mremap(" << old_addr << ", " << old_num_bytes << ", "
             << new_addr << ", " << new_num_bytes << ")";

  auto mr = mapping_of(old_addr);
  const KernelMapping& m = mr.map;

  old_num_bytes = ceil_page_size(old_num_bytes);
  unmap_internal(old_addr, old_num_bytes);
  if (0 == new_num_bytes) {
    return;
  }

  auto it = dont_fork.lower_bound(MemoryRange(old_addr, old_num_bytes));
  if (it != dont_fork.end() && it->start() < old_addr + old_num_bytes) {
    // mremap fails if some but not all pages are marked DONTFORK
    assert(*it == MemoryRange(old_addr, old_num_bytes));
    remove_range(dont_fork, MemoryRange(old_addr, old_num_bytes));
    add_range(dont_fork, MemoryRange(new_addr, new_num_bytes));
  } else {
    remove_range(dont_fork, MemoryRange(old_addr, old_num_bytes));
    remove_range(dont_fork, MemoryRange(new_addr, new_num_bytes));
  }

  remote_ptr<void> new_end = new_addr + new_num_bytes;
  map_and_coalesce(m.set_range(new_addr, new_end),
                   mr.recorded_map.set_range(new_addr, new_end),
                   mr.pseudodevice());
}

void AddressSpace::remove_breakpoint(remote_code_ptr addr, TrapType type) {
  auto it = breakpoints.find(addr);
  if (it == breakpoints.end() || it->second.unref(type) > 0) {
    return;
  }
  destroy_breakpoint(it);
}

bool AddressSpace::add_breakpoint(remote_code_ptr addr, TrapType type) {
  auto it = breakpoints.find(addr);
  if (it == breakpoints.end()) {
    uint8_t overwritten_data;
    // Grab a random task from the VM so we can use its
    // read/write_mem() helpers.
    Task* t = *task_set().begin();
    if (sizeof(overwritten_data) !=
        t->read_bytes_fallible(addr.to_data_ptr<uint8_t>(),
                               sizeof(overwritten_data), &overwritten_data)) {
      return false;
    }
    t->write_mem(addr.to_data_ptr<uint8_t>(), breakpoint_insn);

    auto it_and_is_new = breakpoints.insert(make_pair(addr, Breakpoint()));
    assert(it_and_is_new.second);
    it_and_is_new.first->second.overwritten_data = overwritten_data;
    it = it_and_is_new.first;
  }
  it->second.ref(type);
  return true;
}

void AddressSpace::remove_all_breakpoints() {
  while (!breakpoints.empty()) {
    destroy_breakpoint(breakpoints.begin());
  }
}

int AddressSpace::access_bits_of(WatchType type) {
  switch (type) {
    case WATCH_EXEC:
      return EXEC_BIT;
    case WATCH_WRITE:
      return WRITE_BIT;
    case WATCH_READWRITE:
      return READ_BIT | WRITE_BIT;
    default:
      FATAL() << "Unknown watchpoint type " << type;
      return 0; // not reached
  }
}

void AddressSpace::remove_watchpoint(remote_ptr<void> addr, size_t num_bytes,
                                     WatchType type) {
  auto it = watchpoints.find(MemoryRange(addr, num_bytes));
  if (it != watchpoints.end() &&
      0 == it->second.unwatch(access_bits_of(type))) {
    watchpoints.erase(it);
  }
  allocate_watchpoints();
}

bool AddressSpace::add_watchpoint(remote_ptr<void> addr, size_t num_bytes,
                                  WatchType type) {
  MemoryRange key(addr, num_bytes);
  auto it = watchpoints.find(key);
  if (it == watchpoints.end()) {
    auto it_and_is_new =
        watchpoints.insert(make_pair(key, Watchpoint(num_bytes)));
    assert(it_and_is_new.second);
    it = it_and_is_new.first;
    update_watchpoint_value(it->first, it->second);
  }
  it->second.watch(access_bits_of(type));
  return allocate_watchpoints();
}

void AddressSpace::save_watchpoints() {
  saved_watchpoints.push_back(watchpoints);
}

bool AddressSpace::restore_watchpoints() {
  assert(!saved_watchpoints.empty());
  watchpoints = saved_watchpoints[saved_watchpoints.size() - 1];
  saved_watchpoints.pop_back();
  return allocate_watchpoints();
}

bool AddressSpace::update_watchpoint_value(const MemoryRange& range,
                                           Watchpoint& watchpoint) {
  Task* t = *task_set().begin();
  bool valid = true;
  vector<uint8_t> value_bytes = watchpoint.value_bytes;
  for (size_t i = 0; i < value_bytes.size(); ++i) {
    value_bytes[i] = 0xFF;
  }
  remote_ptr<void> addr = range.start();
  size_t num_bytes = range.size();
  while (num_bytes > 0) {
    ssize_t bytes_read = t->read_bytes_fallible(
        addr, num_bytes, value_bytes.data() + (addr - range.start()));
    if (bytes_read <= 0) {
      valid = false;
      // advance to next page and try to read more. We want to know
      // when the valid part of a partially invalid watchpoint changes.
      bytes_read =
          min<size_t>(num_bytes, (floor_page_size(addr) + page_size()) - addr);
    }
    addr += bytes_read;
    num_bytes -= bytes_read;
  }

  bool changed = valid != watchpoint.valid ||
                 memcmp(value_bytes.data(), watchpoint.value_bytes.data(),
                        value_bytes.size()) != 0;
  watchpoint.valid = valid;
  watchpoint.value_bytes = value_bytes;
  return changed;
}

void AddressSpace::update_watchpoint_values(remote_ptr<void> start,
                                            remote_ptr<void> end) {
  MemoryRange r(start, end);
  for (auto& it : watchpoints) {
    if (it.first.intersects(r) &&
        update_watchpoint_value(it.first, it.second)) {
      it.second.changed = true;
      // We do nothing to track kernel reads of read-write watchpoints...
    }
  }
}

static int DR_WATCHPOINT(int n) { return 1 << n; }

static bool watchpoint_triggered(uintptr_t debug_status,
                                 const vector<int8_t>& regs) {
  for (auto reg : regs) {
    if (debug_status & DR_WATCHPOINT(reg)) {
      return true;
    }
  }
  return false;
}

bool AddressSpace::notify_watchpoint_fired(uintptr_t debug_status) {
  bool triggered = false;
  for (auto& it : watchpoints) {
    if (((it.second.watched_bits() & WRITE_BIT) &&
         update_watchpoint_value(it.first, it.second)) ||
        ((it.second.watched_bits() & (READ_BIT | EXEC_BIT)) &&
         watchpoint_triggered(debug_status,
                              it.second.debug_regs_for_exec_read))) {
      it.second.changed = true;
      triggered = true;
    }
  }
  return triggered;
}

void AddressSpace::notify_written(remote_ptr<void> addr, size_t num_bytes) {
  update_watchpoint_values(addr, addr + num_bytes);
  session()->accumulate_bytes_written(num_bytes);
}

void AddressSpace::remove_all_watchpoints() {
  watchpoints.clear();
  allocate_watchpoints();
}

void AddressSpace::unmap(remote_ptr<void> addr, ssize_t num_bytes) {
  LOG(debug) << "munmap(" << addr << ", " << num_bytes << ")";
  num_bytes = ceil_page_size(num_bytes);
  if (!num_bytes) {
    return;
  }

  remove_range(dont_fork, MemoryRange(addr, num_bytes));

  return unmap_internal(addr, num_bytes);
}

void AddressSpace::unmap_internal(remote_ptr<void> addr, ssize_t num_bytes) {
  LOG(debug) << "munmap(" << addr << ", " << num_bytes << ")";

  auto unmapper = [this](const Mapping& mm, const MemoryRange& rem) {
    LOG(debug) << "  unmapping (" << rem << ") ...";

    Mapping m = move(mm);
    mem.erase(m.map);
    LOG(debug) << "  erased (" << m.map << ") ...";

    // If the first segment we unmap underflows the unmap
    // region, remap the underflow region.
    if (m.map.start() < rem.start()) {
      // When splitting a stack mapping, the bottom part of the split is no
      // longer treated as stack by the kernel.
      PseudoDevice psdev = m.pseudodevice() == PSEUDODEVICE_STACK
                               ? PSEUDODEVICE_ANONYMOUS
                               : m.pseudodevice();
      Mapping underflow(m.map.subrange(m.map.start(), rem.start()),
                        m.recorded_map.subrange(m.map.start(), rem.start()),
                        psdev);
      mem[underflow.map] = underflow;
    }
    // If the last segment we unmap overflows the unmap
    // region, remap the overflow region.
    if (rem.end() < m.map.end()) {
      Mapping overflow(m.map.subrange(rem.end(), m.map.end()),
                       m.recorded_map.subrange(rem.end(), m.map.end()),
                       m.pseudodevice());
      mem[overflow.map] = overflow;
    }
  };
  for_each_in_range(addr, num_bytes, unmapper);
  update_watchpoint_values(addr, addr + num_bytes);
}

void AddressSpace::advise(remote_ptr<void> addr, ssize_t num_bytes,
                          int advice) {
  LOG(debug) << "madvise(" << addr << ", " << num_bytes << ", " << advice
             << ")";
  num_bytes = ceil_page_size(num_bytes);

  switch (advice) {
    case MADV_DONTFORK:
      add_range(dont_fork, MemoryRange(addr, num_bytes));
      break;
    case MADV_DOFORK:
      remove_range(dont_fork, MemoryRange(addr, num_bytes));
      break;
    default:
      break;
  }
}

void AddressSpace::did_fork_into(Task* t) {
  for (auto& range : dont_fork) {
    // During recording we execute MADV_DONTFORK so the forked child will
    // have had its dontfork areas unmapped by the kernel already
    if (!t->session().is_recording()) {
      AutoRemoteSyscalls remote(t);
      remote.syscall(syscall_number_for_munmap(remote.arch()), range.start(),
                     range.size());
    }
    t->vm()->unmap(range.start(), range.size());
  }
}

static bool is_equivalent(const KernelMapping& km1, PseudoDevice pd1,
                          const KernelMapping& km2, PseudoDevice pd2) {
  if (pd1 != pd2) {
    return false;
  }
  if (pd1 == PSEUDODEVICE_ANONYMOUS) {
    return true;
  }
  if (pd1 != PSEUDODEVICE_SYSV_SHM) {
    if (MAJOR(km1.device()) != MAJOR(km2.device())) {
      return false;
    }
    // Allow device minor numbers to vary if the major device is
    // 0. This was observed to be happening on
    // "3.13.0-24-generic #46-Ubuntu SMP" in KVM with btrfs.
    if (MAJOR(km1.device()) != 0 &&
        MINOR(km1.device()) != MINOR(km2.device())) {
      return false;
    }
  }
  return km1.inode() == km2.inode();
}

/**
 * Return true iff |left| and |right| are located adjacently in memory
 * with the same metadata, and map adjacent locations of the same
 * underlying (real) device.
 */
static bool is_adjacent_mapping(const KernelMapping& mleft,
                                PseudoDevice left_psdev,
                                const KernelMapping& mright,
                                PseudoDevice right_psdev,
                                int32_t flags_to_check = 0xFFFFFFFF) {
  if (mleft.end() != mright.start()) {
    LOG(debug) << "    (not adjacent in memory)";
    return false;
  }
  if (((mleft.flags() ^ mright.flags()) & flags_to_check) ||
      mleft.prot() != mright.prot()) {
    LOG(debug) << "    (flags or prot differ)";
    return false;
  }
  if (mright.fsname().substr(0, strlen(PREFIX_FOR_EMPTY_MMAPED_REGIONS)) ==
      PREFIX_FOR_EMPTY_MMAPED_REGIONS) {
    return true;
  }
  if (!is_equivalent(mleft, left_psdev, mright, right_psdev)) {
    LOG(debug) << "    (not the same resource)";
    return false;
  }
  if (mleft.is_real_device() &&
      mleft.file_offset_bytes() + off64_t(mleft.size()) !=
          mright.file_offset_bytes()) {
    LOG(debug) << "    (" << mleft.file_offset_bytes() << " + " << mleft.size()
               << " != " << mright.file_offset_bytes()
               << ": offsets into real device aren't adjacent)";
    return false;
  }
  if (left_psdev == PSEUDODEVICE_SYSV_SHM) {
    LOG(debug) << "    (SysV shm not coalescable)";
    return false;
  }
  LOG(debug) << "    adjacent!";
  return true;
}

/**
 * If (*left_m, left_r), (right_m, right_r) are adjacent (see
 * |is_adjacent_mapping()|), write a merged segment descriptor to
 * |*left_m| and return true.  Otherwise return false.
 */
static bool try_merge_adjacent(KernelMapping* left_m, PseudoDevice left_psdev,
                               const KernelMapping& right_m,
                               PseudoDevice right_psdev) {
  if (is_adjacent_mapping(*left_m, left_psdev, right_m, right_psdev,
                          KernelMapping::checkable_flags_mask)) {
    *left_m = KernelMapping(left_m->start(), right_m.end(), left_m->fsname(),
                            left_m->device(), left_m->inode(), right_m.prot(),
                            right_m.flags(), left_m->file_offset_bytes());
    return true;
  }
  return false;
}

struct VerifyAddressSpace {
  typedef AddressSpace::MemoryMap::const_iterator const_iterator;

  VerifyAddressSpace(const AddressSpace* as)
      : as(as), it(as->mem.begin()), phase(NO_PHASE) {}

  /**
   * |km| and |m| are the same mapping of the same resource, or
   * don't return.
   */
  void assert_segments_match(Task* t);

  /* Current kernel Mapping we're merging and trying to
   * match. */
  KernelMapping km;
  /* Current cached Mapping we've merged and are trying to
   * match. */
  KernelMapping m;
  /* The resource that |km| and |m| map. */
  PseudoDevice psdev;
  const AddressSpace* as;
  /* Iterator over mappings in |as|. */
  const_iterator it;
  /* Which mapping-checking phase we're in.  See below. */
  enum { NO_PHASE, MERGING_CACHED, INITING_KERNEL, MERGING_KERNEL } phase;
};

void VerifyAddressSpace::assert_segments_match(Task* t) {
  assert(MERGING_KERNEL == phase);
  bool same_mapping = (m.start() == km.start() && m.end() == km.end() &&
                       m.prot() == km.prot() && m.flags() == km.flags());
  // When we stripped most identifying info from |r| with
  // |to_kernel()|, we also lost its "is stack" flag.  So to check if
  // it's a grows-down stack segment, we see if it has the special
  // linux name for the process stack segment, "[stack]".
  if (!same_mapping && km.start() < m.start() && "[stack]" == m.fsname()) {
    // TODO: the stack can grow down arbitrarily, and rr needs to be
    // aware of the updated mapping in case the user tries to map or
    // unmap pages near the stack.  But keeping track of expanded
    // stack in general is somewhat difficult, because the stack grows
    // without rr being notified.  So we just add a special early-exit
    // case for the assert for now.
    // We do fix up our current mapping to match the kernel as closely
    // as possible. Then, if the grow-down VMA is split somehow, we know
    // about the split parts.
    t->vm()->fix_stack_segment_start(m, km.start());
    return;
  }
  if (!same_mapping) {
    LOG(error) << "cached mmap:";
    as->dump();
    LOG(error) << "/proc/" << t->tid << "/mmaps:";
    print_process_mmap(t);

    ASSERT(t, same_mapping) << "\nCached mapping " << m << " should be " << km;
  }
}

void AddressSpace::fix_stack_segment_start(const MemoryRange& mapping,
                                           remote_ptr<void> new_start) {
  auto it = mem.find(mapping);
  it->first.update_start(new_start);
  it->second.map.update_start(new_start);
  it->second.recorded_map.update_start(new_start);
}

/**
 * Iterate over the segments that are parsed from
 * |/proc/[t->tid]/maps| and ensure that they match up with the cached
 * segments for |t|.
 *
 * This implementation does the following
 *  1. Merge as many adjacent cached mappings as it can.
 *  2. Merge as many adjacent /proc/maps mappings as it can.
 *  3. Ensure that the two merged mappings are the same.
 *  4. Move on to the next mapping region, goto 1.
 *
 * The kernel and rr have (only very slightly! argh) different
 * heuristics for merging adjacent memory mappings.  That means we
 * can't simply iterate through /proc/maps and assert that a cached
 * mapping corresponds to it, though we sure would like to.  Instead,
 * we reduce the rr mappings to the lowest common denonminator that
 * can be parsed from /proc/maps, and assume that adjacent mappings
 * should be merged if they're equal per common lax criteria (i.e.,
 * not honoring either rr or kernel criteria).  That means that the
 * mapped segments that this helper compares may look nothing like the
 * segments you would see in a /proc/maps dump or |as->dump()|.
 */

KernelMapping AddressSpace::vdso() const {
  assert(!vdso_start_addr.is_null());
  return mapping_of(vdso_start_addr).map;
}

static PseudoDevice to_kernel(PseudoDevice psdev) {
  switch (psdev) {
    case PSEUDODEVICE_STACK:
    case PSEUDODEVICE_SCRATCH:
    case PSEUDODEVICE_ANONYMOUS:
      return PSEUDODEVICE_ANONYMOUS;
    case PSEUDODEVICE_SYSV_SHM:
      return PSEUDODEVICE_SYSV_SHM;
    default:
      return PSEUDODEVICE_NONE;
  }
}

/**
 * Iterate over /proc/maps segments for a task and verify that the
 * task's cached mapping matches the kernel's (given a lenient fuzz
 * factor).
 */
void AddressSpace::verify(Task* t) const {
  assert(task_set().end() != task_set().find(t));

  VerifyAddressSpace vas(this);
  KernelMapIterator it(t);
  KernelMapping km;
  while (it.next(&km)) {
    while (true) {
      LOG(debug) << "examining /proc/maps segment " << km;

      // Merge adjacent cached mappings.
      if (vas.NO_PHASE == vas.phase) {
        assert(vas.it != mem.end());

        vas.phase = vas.MERGING_CACHED;
        // Start of next segment range to match.
        vas.m = vas.it->second.map.to_kernel();
        vas.psdev = to_kernel(vas.it->second.pseudodevice());
        do {
          ++vas.it;
        } while (vas.it != mem.end() &&
                 try_merge_adjacent(&vas.m, vas.psdev,
                                    vas.it->second.map.to_kernel(),
                                    to_kernel(vas.it->second.pseudodevice())));
        vas.phase = vas.INITING_KERNEL;
      }

      LOG(debug) << "  merged cached seg: " << vas.m;

      // Merge adjacent kernel mappings.
      assert(km.flags() == (km.flags() & KernelMapping::checkable_flags_mask));
      PseudoDevice kpsdev = to_kernel(pseudodevice_for_name(km.fsname()));

      if (vas.INITING_KERNEL == vas.phase) {
        assert(is_equivalent(km, kpsdev, vas.m, vas.psdev)
               // XXX not-so-pretty hack.  If the mapped file
               // lives in our replayer's emulated fs, then it
               // will have a real system device/inode
               // descriptor.  We /could/ initialize the
               // MappableResource with that descriptor, but
               // we rely on quick access to the recorded
               // (i.e. emulated in replay) device/inode for
               // gc.  So this suffices for now.
               ||
               string::npos != km.fsname().find(SHMEM_FS "/rr-emufs") ||
               string::npos != km.fsname().find(SHMEM_FS2 "/rr-emufs"));
        vas.km = km;
        vas.psdev = kpsdev;
        vas.phase = vas.MERGING_KERNEL;
        break;
      }
      if (vas.MERGING_KERNEL == vas.phase &&
          try_merge_adjacent(&vas.km, vas.psdev, km, kpsdev)) {
        break;
      }

      // Merged as much as we can ... now the mappings must be
      // equal.
      vas.assert_segments_match(t);

      vas.phase = vas.NO_PHASE;
    }
  }

  assert(vas.MERGING_KERNEL == vas.phase);
  vas.assert_segments_match(t);
}

AddressSpace::AddressSpace(Task* t, const string& exe, uint32_t exec_count)
    : exe(exe),
      leader_tid_(t->rec_tid),
      leader_serial(t->tuid().serial()),
      exec_count(exec_count),
      is_clone(false),
      session_(&t->session()),
      child_mem_fd(-1),
      first_run_event_(0) {
  // TODO: this is a workaround of
  // https://github.com/mozilla/rr/issues/1113 .
  if (session_->can_validate()) {
    populate_address_space(t);
    assert(!vdso_start_addr.is_null());
  } else {
    // Find the location of the VDSO in the just-spawned process. This will
    // match the VDSO in rr itself since we haven't execed yet. So, speed
    // things up by search rr's own VDSO for a syscall instruction.
    size_t rr_vdso_len;
    remote_ptr<void> rr_vdso = find_rr_vdso(t, &rr_vdso_len);
    // Here we rely on the VDSO location in the spawned tracee being the same
    // as in rr itself.
    uint8_t* local_vdso = reinterpret_cast<uint8_t*>(rr_vdso.as_int());
    auto offset = find_offset_of_syscall_instruction_in(
        NativeArch::arch(), local_vdso, rr_vdso_len);
    offset_to_syscall_in_vdso[NativeArch::arch()] = offset;
    // Setup traced_syscall_ip_ now because we need to do AutoRemoteSyscalls
    // (for open_mem_fd) before the first exec.
    traced_syscall_ip_ = remote_code_ptr(rr_vdso.as_int() + offset);
  }
}

AddressSpace::AddressSpace(Session* session, const AddressSpace& o,
                           pid_t leader_tid, uint32_t leader_serial,
                           uint32_t exec_count)
    : exe(o.exe),
      leader_tid_(leader_tid),
      leader_serial(leader_serial),
      exec_count(exec_count),
      heap(o.heap),
      is_clone(true),
      mem(o.mem),
      session_(session),
      vdso_start_addr(o.vdso_start_addr),
      monkeypatch_state(o.monkeypatch_state),
      traced_syscall_ip_(o.traced_syscall_ip_),
      untraced_syscall_ip_(o.untraced_syscall_ip_),
      privileged_traced_syscall_ip_(o.privileged_traced_syscall_ip_),
      privileged_untraced_syscall_ip_(o.privileged_untraced_syscall_ip_),
      syscallbuf_lib_start_(o.syscallbuf_lib_start_),
      syscallbuf_lib_end_(o.syscallbuf_lib_end_),
      saved_auxv_(o.saved_auxv_),
      first_run_event_(0) {
  for (auto& it : o.breakpoints) {
    breakpoints.insert(make_pair(it.first, it.second));
  }
  for (auto& it : o.watchpoints) {
    watchpoints.insert(make_pair(it.first, it.second));
  }
  if (session != o.session()) {
    // Cloning into a new session means we're checkpointing.
    first_run_event_ = o.first_run_event_;
  }
  // cloned tasks will automatically get cloned debug registers and
  // cloned address-space memory, so we don't need to do any more work here.
}

static bool try_split_unaligned_range(MemoryRange& range, size_t bytes,
                                      vector<MemoryRange>& result) {
  if ((range.start().as_int() & (bytes - 1)) || range.size() < bytes) {
    return false;
  }
  result.push_back(MemoryRange(range.start(), bytes));
  range = MemoryRange(range.start() + bytes, range.end());
  return true;
}

static vector<MemoryRange> split_range(const MemoryRange& range) {
  vector<MemoryRange> result;
  MemoryRange r = range;
  while (r.size() > 0) {
    if ((sizeof(void*) < 8 || !try_split_unaligned_range(r, 8, result)) &&
        !try_split_unaligned_range(r, 4, result) &&
        !try_split_unaligned_range(r, 2, result)) {
      bool ret = try_split_unaligned_range(r, 1, result);
      assert(ret);
    }
  }
  return result;
}

static void configure_watch_registers(vector<WatchConfig>& regs,
                                      const MemoryRange& range, WatchType type,
                                      vector<int8_t>* assigned_regs) {
  auto split_ranges = split_range(range);

  if (type == WATCH_WRITE && range.size() > 1) {
    // We can suppress spurious write-watchpoint triggerings by checking
    // whether memory values have changed. So we can sometimes conserve
    // debug registers by upgrading an unaligned range to an aligned range
    // of a larger size.
    uintptr_t align;
    if (range.size() <= 2) {
      align = 2;
    } else if (range.size() <= 4 || sizeof(void*) <= 4) {
      align = 4;
    } else {
      align = 8;
    }
    remote_ptr<void> aligned_start(range.start().as_int() & ~(align - 1));
    remote_ptr<void> aligned_end((range.end().as_int() + (align - 1)) &
                                 ~(align - 1));
    auto split = split_range(MemoryRange(aligned_start, aligned_end));
    // If the aligned range doesn't reduce register usage, use the original
    // split to avoid spurious triggerings
    if (split.size() < split_ranges.size()) {
      split_ranges = split;
    }
  }

  for (auto& r : split_ranges) {
    if (assigned_regs) {
      assigned_regs->push_back(regs.size());
    }
    regs.push_back(WatchConfig(r.start(), r.size(), type));
  }
}

vector<WatchConfig> AddressSpace::get_watch_configs(
    WillSetTaskState will_set_task_state) {
  vector<WatchConfig> result;
  for (auto& kv : watchpoints) {
    vector<int8_t>* assigned_regs = nullptr;
    if (will_set_task_state == SETTING_TASK_STATE) {
      kv.second.debug_regs_for_exec_read.clear();
      assigned_regs = &kv.second.debug_regs_for_exec_read;
    }
    const MemoryRange& r = kv.first;
    int watching = kv.second.watched_bits();
    if (EXEC_BIT & watching) {
      configure_watch_registers(result, r, WATCH_EXEC, assigned_regs);
    }
    if (READ_BIT & watching) {
      configure_watch_registers(result, r, WATCH_READWRITE, assigned_regs);
    } else if (WRITE_BIT & watching) {
      configure_watch_registers(result, r, WATCH_WRITE, nullptr);
    }
  }
  return result;
}

vector<WatchConfig> AddressSpace::get_watchpoints_internal(
    WatchpointFilter filter) {
  vector<WatchConfig> result;
  for (auto& kv : watchpoints) {
    if (filter == CHANGED_WATCHPOINTS) {
      if (!kv.second.changed) {
        continue;
      }
      kv.second.changed = false;
    }
    const MemoryRange& r = kv.first;
    int watching = kv.second.watched_bits();
    if (EXEC_BIT & watching) {
      result.push_back(WatchConfig(r.start(), r.size(), WATCH_EXEC));
    }
    if (READ_BIT & watching) {
      result.push_back(WatchConfig(r.start(), r.size(), WATCH_READWRITE));
    } else if (WRITE_BIT & watching) {
      result.push_back(WatchConfig(r.start(), r.size(), WATCH_WRITE));
    }
  }
  return result;
}

vector<WatchConfig> AddressSpace::consume_watchpoint_changes() {
  return get_watchpoints_internal(CHANGED_WATCHPOINTS);
}

vector<WatchConfig> AddressSpace::all_watchpoints() {
  return get_watchpoints_internal(ALL_WATCHPOINTS);
}

bool AddressSpace::allocate_watchpoints() {
  Task::DebugRegs regs = get_watch_configs(SETTING_TASK_STATE);

  if (regs.size() <= 0x7f) {
    bool ok = true;
    for (auto t : task_set()) {
      if (!t->set_debug_regs(regs)) {
        ok = false;
      }
    }
    if (ok) {
      return true;
    }
  }

  regs.clear();
  for (auto t2 : task_set()) {
    t2->set_debug_regs(regs);
  }
  for (auto kv : watchpoints) {
    kv.second.debug_regs_for_exec_read.clear();
  }
  return false;
}

void AddressSpace::coalesce_around(MemoryMap::iterator it) {
  auto first_kv = it;
  while (mem.begin() != first_kv) {
    auto next = first_kv;
    --first_kv;
    if (!is_adjacent_mapping(first_kv->second.map, first_kv->second.psdev,
                             next->second.map, next->second.psdev)) {
      first_kv = next;
      break;
    }
  }
  auto last_kv = it;
  while (true) {
    auto prev = last_kv;
    ++last_kv;
    if (mem.end() == last_kv ||
        !is_adjacent_mapping(prev->second.map, prev->second.psdev,
                             last_kv->second.map, last_kv->second.psdev)) {
      last_kv = prev;
      break;
    }
  }
  assert(last_kv != mem.end());
  if (first_kv == last_kv) {
    LOG(debug) << "  no mappings to coalesce";
    return;
  }

  Mapping new_m(first_kv->second.map.extend(last_kv->first.end()),
                first_kv->second.recorded_map.extend(last_kv->first.end()),
                first_kv->second.pseudodevice());
  LOG(debug) << "  coalescing " << new_m.map;

  mem.erase(first_kv, ++last_kv);

  auto ins = mem.insert(MemoryMap::value_type(new_m.map, new_m));
  assert(ins.second); // key didn't already exist
}

void AddressSpace::destroy_breakpoint(BreakpointMap::const_iterator it) {
  Task* t = *task_set().begin();
  t->write_mem(it->first.to_data_ptr<uint8_t>(), it->second.overwritten_data);
  breakpoints.erase(it);
}

void AddressSpace::for_each_in_range(
    remote_ptr<void> addr, ssize_t num_bytes,
    function<void(const Mapping& m, const MemoryRange& rem)> f, int how) {
  remote_ptr<void> region_start = floor_page_size(addr);
  remote_ptr<void> last_unmapped_end = region_start;
  remote_ptr<void> region_end = ceil_page_size(addr + num_bytes);
  while (last_unmapped_end < region_end) {
    // Invariant: |rem| is always exactly the region of
    // memory remaining to be examined for pages to be
    // unmapped.
    MemoryRange rem(last_unmapped_end, region_end);

    // The next page to iterate may not be contiguous with
    // the last one seen.
    auto it = mem.lower_bound(rem);
    if (mem.end() == it) {
      LOG(debug) << "  not found, done.";
      return;
    }

    // Don't make a reference here. |f| is allowed to erase Mappings.
    MemoryRange range = it->first;
    if (rem.end() <= range.start()) {
      LOG(debug) << "  mapping at " << range.start() << " out of range, done.";
      return;
    }
    if (ITERATE_CONTIGUOUS == how &&
        !(range.start() < region_start || rem.start() == range.start())) {
      LOG(debug) << "  discontiguous mapping at " << range.start() << ", done.";
      return;
    }

    f(it->second, rem);

    // Maintain the loop invariant.
    last_unmapped_end = range.end();
  }
}

void AddressSpace::map_and_coalesce(const KernelMapping& m,
                                    const KernelMapping& recorded_map,
                                    PseudoDevice psdev) {
  LOG(debug) << "  mapping " << m;

  auto ins =
      mem.insert(MemoryMap::value_type(m, Mapping(m, recorded_map, psdev)));
  coalesce_around(ins.first);

  update_watchpoint_values(m.start(), m.end());
}

void AddressSpace::populate_address_space(Task* t) {
  KernelMapIterator it(t);
  KernelMapping km;
  while (it.next(&km)) {
    if (!heap.start() && exe == km.fsname() && !(km.prot() & PROT_EXEC) &&
        (km.prot() & (PROT_READ | PROT_WRITE))) {
      update_heap(km.end(), km.end());
      LOG(debug) << "  guessing heap starts at " << heap.start()
                 << " (end of text segment)";
    }

    // This segment is adjacent to our previous guess at the start of
    // the dynamic heap, but it's still not an explicit heap segment.
    // Or, in corner cases, the segment is the final mapping of the data
    // segment of the exe image, but is not adjacent to the prior mapped
    // segment of the exe.  (This is seen with x86-64 bash on Fedora
    // Core 20.)  Update the guess.
    if (!(km.prot() & PROT_EXEC) &&
        (heap.end() == km.start() || exe == km.fsname())) {
      assert(heap.start() == heap.end() || exe == km.fsname());
      update_heap(km.end(), km.end());
      LOG(debug) << "  updating start-of-heap guess to " << heap.start()
                 << " (end of mapped-data segment)";
    }

    PseudoDevice psdev = pseudodevice_for_name(km.fsname());
    if (psdev == PSEUDODEVICE_HEAP) {
      if (!heap.start()) {
        // No guess for the heap start. Assume it's just the [heap] segment.
        update_heap(km.start(), km.end());
      } else {
        update_heap(heap.start(), km.end());
      }
    }

    int flags = km.flags();
    if (psdev == PSEUDODEVICE_STACK) {
      flags |= MAP_GROWSDOWN;
    }
    map(km.start(), km.size(), km.prot(), flags, km.file_offset_bytes(),
        MappableResource(km.device(), km.inode(), psdev), km.fsname());
  }
}
