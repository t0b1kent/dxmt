// SPDX-License-Identifier: LGPL-2.1-or-later
// Added 2026 by the MacRunner project; see README-MACRUNNER.md
//
// GAPS CPU stage (01.11.2026): an in-process PC sampler for the unix side of the process. Off unless
// MACRUNNER_PC_SAMPLER=<Hz> (1..1000). A plain pthread, unknown to Wine (it never calls into Wine, takes no Wine lock and
// receives no signal). Every second it lists the task's threads (Mach task_threads + THREAD_EXTENDED_INFO), picks the
// MACRUNNER_PC_SAMPLER_THREADS (default 4) busiest ones of the last second and, Hz times per second, reads each one's
// run state and PC/LR (thread_get_state: the kernel stops the thread for the copy, a few microseconds; no signal, no
// handler, nothing runs in the target; the period is jittered 0.5..1.5 x so it does not alias with frame-periodic work).
// Every PC is put in one class by address:
//   jit         executable memory that is neither a PE image nor a dyld image: the translator's code cache (game code)
//   translator  xtajit64.dll / xtajit.dll (PE) and libarm64ecfex.so / libwow64fex.so / libmacrunner-hwtso.dylib
//   wine_pe     any other PE image (Wine's ARM64EC/ARM64X modules: ntdll.dll, kernelbase.dll, ucrtbase.dll, ...)
//   layer_pe    d3d12.dll, dxgi.dll, d3d11.dll, d3d10core.dll, d3d9.dll, winemetal.dll (PE side of the graphics layer)
//   layer_unix  winemetal.so (unix side of the layer, airconv)
//   wine_unix   Wine's unix libraries (*.so other than the above) and the main executable (wine / preloader)
//   metal       Metal, AGX*, IOGPU, IOSurface, MetalFX, GPUCompiler, MetalPerformanceShaders
//   kernel      libsystem_kernel.dylib (system call stubs: a running sample here is in the kernel)
//   system      every other dyld image (libsystem_*, libobjc, libc++, CoreFoundation, libdispatch, ...)
//   unknown     none of the above (not executable, unreadable)
// PE images are found by scanning the address space for 64 KB-aligned readable regions that start with an MZ/PE header
// (mach_vm_read_overwrite, never a direct load), names from the export directory; rescanned every 5 s. Samples of a
// thread that is not running (TH_STATE_RUNNING) are counted apart ("wait"). For running samples in metal/kernel/system
// the LR class is counted too ("via"), to see who called the leaf (game code through jit, Wine, the layer).
// Every MACRUNNER_PC_SAMPLER_WINDOW seconds (default 10) one line per sampled thread to stderr:
//   pc_sampler window=W tid=<thread_id> name=<pthread name> cpu_ms_per_s=C samples=N running=R
//              run_<class>=n ... wait=n via_<class>=n ... top=<module>:<n>,...
// plus "pc_sampler_scan" (PE images found) and "pc_sampler_cost" (time the sampler itself spent per second).
// MACRUNNER_PC_SAMPLER_SYSCALLS=1 (default 0, GAPS 01.11): for a sample whose PC is in libsystem_kernel right after `svc #0x80`
// (the word at PC-4 is 0xd4001001: the thread is in, or blocked in, a system call) the number in x16 is counted (positive = BSD
// syscall, negative = Mach trap) together with the module of the caller of the wrapper (LR), separately for running and waiting
// samples: " sys_run=<num>@<module>:<n>,..." and " sys_wait=...". A kernel-class sample not right after svc counts as "user".
// MACRUNNER_PC_SAMPLER_STACK=1 (default 0, with _SYSCALLS, GAPS 01.11 12:xx): for those syscall samples the frame-pointer chain is
// followed (x29 -> [fp] / [fp+8], up to 8 frames, reads through mach_vm_read_overwrite) to the first return address outside
// libsystem_kernel / libsystem_platform / libsystem_pthread: "who took the lock / made the call", named <module>+0x<offset>(<nearest
// exported symbol>) for dyld images or <module>+0x<offset> for PE images, counted per syscall number: " sys_owner=<num>|<owner>:<n>,..." (running and waiting
// together) — e.g. __ulock_wait2 via os_unfair_lock: which library's lock.
// MACRUNNER_PC_SAMPLER_PECHAIN=1 (default 0, with _STACK, GAPS 01.11 14:xx): the same walk goes on across the system-call dispatcher
// into the PE side. Wine's arm64 __wine_syscall_dispatcher does not touch x29, so the first unix function's frame record holds the
// PE x29 and the walk continues on the PE stack (one downward jump of the frame pointer is allowed there: the unix side runs on the
// kernel stack). Counted per (syscall number, running/waiting, unix owner, PE chain): " sys_pe=<num>|<r|w>|<owner>|<pe0><<pe1><...<tail>:<n>,..." with the
// first PE return addresses inside Wine's PE modules (up to 4, <module>+0x<rva>) and the first frame that is NOT a Wine PE module
// (the tail: d3d12.dll+..., winemetal.dll+..., the game: <exe name or pe@base>+0x<rva> = the x64 return address of the call into
// Wine, as the ARM64EC entry keeps it in the frame record; [jit...] / translator; or "-"). Stands 01.11: x64 probe -> tail =
// cpu-probe main+0xcd7 / +0xc27 / +0xb8a (WaitForSingleObject / SetEvent / SwitchToThread call sites); layer SRW wait -> tail =
// d3d12.dll+0xa8e8 (dxmt::mutex::lock). Frames past the first x64 one follow RBP and are not a call chain.
// That separates a wait of the layer's own lock from one of the game's / Wine's.
// Ground truth on a stand with known split first; the sampler then reports, it does not decide.
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <mach/thread_info.h>
#include <mach/thread_status.h>
#include <pthread.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>

enum { C_JIT, C_TRANSLATOR, C_WINE_PE, C_LAYER_PE, C_LAYER_UNIX, C_WINE_UNIX, C_METAL, C_KERNEL, C_SYSTEM, C_UNKNOWN, C_COUNT };
static const char *const class_names[C_COUNT] = {"jit", "translator", "wine_pe", "layer_pe", "layer_unix", "wine_unix",
                                                 "metal", "kernel", "system", "unknown"};

#define MAX_IMAGES 2048
#define MAX_MODULES 512
#define MAX_PICK 16
#define ADDR_MASK ((1ull << 47) - 1)

struct image { uint64_t base, end; int cls, module; };
static struct image pe_images[MAX_IMAGES];
static int pe_count;
static struct { uint64_t base; int cls, module; } dyld_cache[256];
static int dyld_count;
static char module_names[MAX_MODULES][48];
static int module_count;

static int
module_id(const char *name) {
  for (int i = 0; i < module_count; ++i)
    if (!strcmp(module_names[i], name)) return i;
  if (module_count == MAX_MODULES) return MAX_MODULES - 1;
  snprintf(module_names[module_count], sizeof(module_names[0]), "%s", name);
  return module_count++;
}

static int
lower_eq(const char *a, const char *b) {
  for (; *a && *b; ++a, ++b) {
    char x = *a >= 'A' && *a <= 'Z' ? *a + 32 : *a, y = *b >= 'A' && *b <= 'Z' ? *b + 32 : *b;
    if (x != y) return 0;
  }
  return !*a && !*b;
}

static int
pe_class(const char *name) {
  static const char *const layer[] = {"d3d12.dll", "dxgi.dll", "d3d11.dll", "d3d10core.dll", "d3d9.dll", "winemetal.dll", 0};
  static const char *const translator[] = {"xtajit64.dll", "xtajit.dll", "libarm64ecfex.dll", "libwow64fex.dll", 0};
  for (int i = 0; layer[i]; ++i) if (lower_eq(name, layer[i])) return C_LAYER_PE;
  for (int i = 0; translator[i]; ++i) if (lower_eq(name, translator[i])) return C_TRANSLATOR;
  return C_WINE_PE;
}

static int
read_mem(uint64_t addr, void *out, size_t size) {
  mach_vm_size_t got = 0;
  return mach_vm_read_overwrite(mach_task_self(), addr, size, (mach_vm_address_t)out, &got) == KERN_SUCCESS && got == size;
}

static int
image_compare(const void *a, const void *b) {
  const struct image *x = a, *y = b;
  return x->base < y->base ? -1 : x->base > y->base;
}

static void
scan_pe_images(void) {
  struct image found[MAX_IMAGES];
  int n = 0;
  mach_vm_address_t addr = 0x10000;
  for (;;) {
    mach_vm_size_t size = 0;
    vm_region_basic_info_data_64_t info;
    mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t object = MACH_PORT_NULL;
    if (mach_vm_region(mach_task_self(), &addr, &size, VM_REGION_BASIC_INFO_64, (vm_region_info_t)&info, &count, &object) != KERN_SUCCESS)
      break;
    if (object != MACH_PORT_NULL) mach_port_deallocate(mach_task_self(), object);
    if ((info.protection & VM_PROT_READ) && !(addr & 0xffff) && n < MAX_IMAGES) {
      unsigned char head[0x400];
      if (read_mem(addr, head, sizeof(head)) && head[0] == 'M' && head[1] == 'Z') {
        uint32_t lfanew;
        memcpy(&lfanew, head + 0x3c, 4);
        if (lfanew >= 0x40 && lfanew + 0x18 + 0x70 + 8 <= sizeof(head) && !memcmp(head + lfanew, "PE\0\0", 4)) {
          const unsigned char *opt = head + lfanew + 0x18;
          uint16_t magic; uint32_t image_size, export_rva = 0;
          memcpy(&magic, opt, 2);
          memcpy(&image_size, opt + 56, 4);
          if (magic == 0x20b) memcpy(&export_rva, opt + 112, 4);
          else if (magic == 0x10b) memcpy(&export_rva, opt + 96, 4);
          char name[48];
          snprintf(name, sizeof(name), "pe@%llx", (unsigned long long)addr);
          uint32_t name_rva;
          if (export_rva && export_rva < image_size && read_mem(addr + export_rva + 12, &name_rva, 4) && name_rva && name_rva < image_size) {
            char text[48] = {0};
            if (read_mem(addr + name_rva, text, sizeof(text) - 1) || read_mem(addr + name_rva, text, 16)) {
              text[sizeof(text) - 1] = 0;
              int ok = text[0] != 0;
              for (int i = 0; text[i]; ++i) if (text[i] < 0x20 || text[i] > 0x7e) { ok = 0; break; }
              if (ok) snprintf(name, sizeof(name), "%s", text);
            }
          }
          if ((magic == 0x20b || magic == 0x10b) && image_size && image_size < (1u << 30)) {
            found[n].base = addr;
            found[n].end = addr + image_size;
            found[n].cls = pe_class(name);
            found[n].module = module_id(name);
            ++n;
          }
        }
      }
    }
    if (addr + size <= addr) break;
    addr += size;
    if (addr >= (1ull << 47)) break;
  }
  qsort(found, n, sizeof(found[0]), image_compare);
  memcpy(pe_images, found, sizeof(found[0]) * n);
  pe_count = n;
}

static int
dyld_class(const char *path, int *module) {
  const char *base = strrchr(path, '/');
  base = base ? base + 1 : path;
  *module = module_id(base);
  if (!strcmp(base, "winemetal.so")) return C_LAYER_UNIX;
  if (!strcmp(base, "libarm64ecfex.so") || !strcmp(base, "libwow64fex.so") || !strcmp(base, "xtajit64.so") ||
      !strcmp(base, "xtajit.so") || !strcmp(base, "libmacrunner-hwtso.dylib"))
    return C_TRANSLATOR;
  if (strstr(path, "Metal") || strstr(path, "AGX") || strstr(path, "IOGPU") || strstr(path, "IOSurface") ||
      strstr(path, "GPUCompiler"))
    return C_METAL;
  if (!strcmp(base, "libsystem_kernel.dylib")) return C_KERNEL;
  const size_t len = strlen(base);
  if (len > 3 && !strcmp(base + len - 3, ".so")) return C_WINE_UNIX;
  if (strstr(base, "wine") || strstr(path, "/lib/wine/")) return C_WINE_UNIX;
  return C_SYSTEM;
}

static int
classify(uint64_t pc, int *module) {
  int lo = 0, hi = pe_count - 1;
  while (lo <= hi) {
    const int mid = (lo + hi) / 2;
    if (pc < pe_images[mid].base) hi = mid - 1;
    else if (pc >= pe_images[mid].end) lo = mid + 1;
    else { *module = pe_images[mid].module; return pe_images[mid].cls; }
  }
  Dl_info info;
  if (dladdr((const void *)pc, &info) && info.dli_fname) {
    const uint64_t fbase = (uint64_t)info.dli_fbase;
    for (int i = 0; i < dyld_count; ++i)
      if (dyld_cache[i].base == fbase) { *module = dyld_cache[i].module; return dyld_cache[i].cls; }
    int m;
    const int cls = dyld_class(info.dli_fname, &m);
    if (dyld_count < 256) { dyld_cache[dyld_count].base = fbase; dyld_cache[dyld_count].cls = cls; dyld_cache[dyld_count].module = m; ++dyld_count; }
    *module = m;
    return cls;
  }
  mach_vm_address_t addr = pc;
  mach_vm_size_t size = 0;
  vm_region_basic_info_data_64_t region;
  mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
  mach_port_t object = MACH_PORT_NULL;
  if (mach_vm_region(mach_task_self(), &addr, &size, VM_REGION_BASIC_INFO_64, (vm_region_info_t)&region, &count, &object) == KERN_SUCCESS) {
    if (object != MACH_PORT_NULL) mach_port_deallocate(mach_task_self(), object);
    if (addr <= pc && (region.protection & VM_PROT_EXECUTE)) {
      // Small executable regions are named by base (the translator's dispatcher / thunks live in small code buffers of
      // their own; translated blocks in the large code cache): "[jit]" for regions of 4 MB and more.
      if (size < (4u << 20)) {
        char name[48];
        snprintf(name, sizeof(name), "[jit@%llx+%lluk]", (unsigned long long)addr, (unsigned long long)(size >> 10));
        *module = module_id(name);
      } else *module = module_id("[jit]");
      return C_JIT;
    }
  }
  *module = module_id("[unknown]");
  return C_UNKNOWN;
}

struct pick {
  thread_act_t port;
  uint64_t tid, cpu_ns_start, cpu_ns_last;
  char name[64];
  uint64_t samples, running, wait, run[C_COUNT], via[C_COUNT], modules[MAX_MODULES];
};

static struct pick picks[MAX_PICK];
static int pick_count;

static uint64_t
now_ns(void) {
  return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
}

static uint64_t
self_cpu_ns(void) {
  return clock_gettime_nsec_np(CLOCK_THREAD_CPUTIME_ID);
}

struct thread_row { thread_act_t port; uint64_t tid, cpu; char name[64]; };
static struct { uint64_t tid, cpu; } prev_cpu[1024];
static int prev_count;

static void report(unsigned window, double seconds);

static void
refresh_picks(unsigned want, uint64_t forced_tid, unsigned window, double window_seconds, int report_now) {
  if (report_now) report(window, window_seconds);
  for (int i = 0; i < pick_count; ++i) mach_port_deallocate(mach_task_self(), picks[i].port);
  pick_count = 0;
  thread_act_array_t list = NULL;
  mach_msg_type_number_t count = 0;
  if (task_threads(mach_task_self(), &list, &count) != KERN_SUCCESS) return;
  thread_act_t self = mach_thread_self();
  static struct thread_row rows[1024];
  int n = 0;
  for (mach_msg_type_number_t i = 0; i < count; ++i) {
    thread_extended_info_data_t ext;
    thread_identifier_info_data_t ident;
    mach_msg_type_number_t c1 = THREAD_EXTENDED_INFO_COUNT, c2 = THREAD_IDENTIFIER_INFO_COUNT;
    if (list[i] != self && n < 1024 &&
        thread_info(list[i], THREAD_EXTENDED_INFO, (thread_info_t)&ext, &c1) == KERN_SUCCESS &&
        thread_info(list[i], THREAD_IDENTIFIER_INFO, (thread_info_t)&ident, &c2) == KERN_SUCCESS) {
      rows[n].port = list[i];
      rows[n].tid = ident.thread_id;
      rows[n].cpu = ext.pth_user_time + ext.pth_system_time;
      snprintf(rows[n].name, sizeof(rows[n].name), "%s", ext.pth_name);
      ++n;
    } else {
      mach_port_deallocate(mach_task_self(), list[i]);
    }
  }
  vm_deallocate(mach_task_self(), (vm_address_t)list, count * sizeof(list[0]));
  mach_port_deallocate(mach_task_self(), self);
  // CPU of the last second per thread
  static uint64_t delta[1024];
  for (int i = 0; i < n; ++i) {
    delta[i] = 0;
    for (int j = 0; j < prev_count; ++j)
      if (prev_cpu[j].tid == rows[i].tid) { delta[i] = rows[i].cpu - prev_cpu[j].cpu; break; }
  }
  for (int i = 0; i < n && i < 1024; ++i) { prev_cpu[i].tid = rows[i].tid; prev_cpu[i].cpu = rows[i].cpu; }
  prev_count = n < 1024 ? n : 1024;
  // pick the busiest (and the forced one)
  int chosen[MAX_PICK], c = 0;
  if (forced_tid)
    for (int i = 0; i < n && c < MAX_PICK; ++i) if (rows[i].tid == forced_tid) chosen[c++] = i;
  while (c < (int)want && c < MAX_PICK) {
    int best = -1;
    for (int i = 0; i < n; ++i) {
      int taken = 0;
      for (int k = 0; k < c; ++k) if (chosen[k] == i) taken = 1;
      if (!taken && (best < 0 || delta[i] > delta[best])) best = i;
    }
    if (best < 0 || !delta[best]) break;
    chosen[c++] = best;
  }
  for (int i = 0; i < n; ++i) {
    int keep = -1;
    for (int k = 0; k < c; ++k) if (chosen[k] == i) keep = k;
    if (keep < 0) { mach_port_deallocate(mach_task_self(), rows[i].port); continue; }
  }
  for (int k = 0; k < c; ++k) {
    struct pick *p = &picks[pick_count++];
    const struct thread_row *r = &rows[chosen[k]];
    p->port = r->port;
    p->tid = r->tid;
    p->cpu_ns_last = r->cpu;
    snprintf(p->name, sizeof(p->name), "%s", r->name);
  }
}

// Accumulation per thread id across picks of one window
#define MAX_ACC 64
#define MAX_SYS 48
struct sysent { int64_t num; int module; uint64_t count; };
#define MAX_OWN 64
#define MAX_PECH 96
struct pechent { int64_t num; int owner, chain, running; uint64_t count; };
struct acc {
  uint64_t tid, cpu_ns, samples, running, wait, run[C_COUNT], via[C_COUNT], modules[MAX_MODULES];
  struct sysent sys_run[MAX_SYS], sys_wait[MAX_SYS], sys_own[MAX_OWN];
  struct pechent sys_pe[MAX_PECH];
  uint64_t sys_pe_overflow;
  uint64_t sys_user_run, sys_user_wait, sys_overflow;
  uint64_t cpu_first, cpu_last, t_first, t_last;   // thread CPU and sampler time (uptime ns) at the first / last sample
  char name[64];
};
static struct acc accs[MAX_ACC];
static int sys_enabled, stack_enabled, pechain_enabled;
#define MAX_CHAIN_NAMES 1024
static char chain_names[MAX_CHAIN_NAMES][224];
static int chain_count;
static int
chain_id(const char *name) {
  for (int i = 0; i < chain_count; ++i) if (!strcmp(chain_names[i], name)) return i;
  if (chain_count == MAX_CHAIN_NAMES) return MAX_CHAIN_NAMES - 1;
  snprintf(chain_names[chain_count], sizeof(chain_names[0]), "%s", name);
  return chain_count++;
}
#define MAX_OWNER_NAMES 1024
static char owner_names[MAX_OWNER_NAMES][96];
static int owner_count;
static int
owner_id(const char *name) {
  for (int i = 0; i < owner_count; ++i) if (!strcmp(owner_names[i], name)) return i;
  if (owner_count == MAX_OWNER_NAMES) return MAX_OWNER_NAMES - 1;
  snprintf(owner_names[owner_count], sizeof(owner_names[0]), "%s", name);
  return owner_count++;
}
static int acc_count;
static uint64_t sampler_cpu_ns_window, sampler_samples_window;

static struct acc *
acc_for(uint64_t tid, const char *name, uint64_t cpu, uint64_t now) {
  for (int i = 0; i < acc_count; ++i)
    if (accs[i].tid == tid) { accs[i].cpu_last = cpu; accs[i].t_last = now; return &accs[i]; }
  if (acc_count == MAX_ACC) return NULL;
  struct acc *a = &accs[acc_count++];
  memset(a, 0, sizeof(*a));
  a->tid = tid;
  a->cpu_first = a->cpu_last = cpu;
  a->t_first = a->t_last = now;
  snprintf(a->name, sizeof(a->name), "%s", name[0] ? name : "-");
  return a;
}

static void
own_count(struct acc *a, int64_t num, int owner) {
  for (int i = 0; i < MAX_OWN; ++i) {
    if (a->sys_own[i].count && a->sys_own[i].num == num && a->sys_own[i].module == owner) { ++a->sys_own[i].count; return; }
    if (!a->sys_own[i].count) { a->sys_own[i].num = num; a->sys_own[i].module = owner; a->sys_own[i].count = 1; return; }
  }
}

static int
is_libsystem_lock_frame(uint64_t addr) {
  Dl_info info;
  if (!dladdr((const void *)addr, &info) || !info.dli_fname) return 0;
  const char *base = strrchr(info.dli_fname, '/');
  base = base ? base + 1 : info.dli_fname;
  return !strcmp(base, "libsystem_kernel.dylib") || !strcmp(base, "libsystem_platform.dylib") || !strcmp(base, "libsystem_pthread.dylib");
}

static int
pe_image_of(uint64_t addr) {
  int lo = 0, hi = pe_count - 1;
  while (lo <= hi) {
    const int mid = (lo + hi) / 2;
    if (addr < pe_images[mid].base) hi = mid - 1;
    else if (addr >= pe_images[mid].end) lo = mid + 1;
    else return mid;
  }
  return -1;
}

// PE chain after the unix owner (frames[first..n)): up to 4 return addresses in Wine PE modules, then the tail (first other frame).
static int
pe_chain_of(const uint64_t *frames, int first, int n) {
  char text[224];
  int k = 0, shown = 0, seen_pe = 0;
  text[0] = 0;
  for (int i = first; i < n && k < (int)sizeof(text) - 64; ++i) {
    const int img = pe_image_of(frames[i]);
    if (img < 0 && !seen_pe) continue;   // still on the unix side (dispatcher, unix helpers)
    seen_pe = 1;
    // a Wine PE module: class wine_pe and a .dll (an image without export name, pe@..., or an .exe is the program: tail)
    const char *mn = img >= 0 ? module_names[pe_images[img].module] : "";
    const size_t ml = strlen(mn);
    if (img >= 0 && pe_images[img].cls == C_WINE_PE && ml > 4 && lower_eq(mn + ml - 4, ".dll")) {
      if (shown < 4)
        k += snprintf(text + k, sizeof(text) - k, "%s%s+0x%llx", shown ? "<" : "", module_names[pe_images[img].module],
                      (unsigned long long)(frames[i] - pe_images[img].base));
      ++shown;
      continue;
    }
    int module;
    classify(frames[i], &module);
    if (img >= 0)
      k += snprintf(text + k, sizeof(text) - k, "%s%s%s+0x%llx", shown > 4 ? "<.." : "", shown ? "<" : "", module_names[pe_images[img].module],
                    (unsigned long long)(frames[i] - pe_images[img].base));
    else
      k += snprintf(text + k, sizeof(text) - k, "%s%s%s", shown > 4 ? "<.." : "", shown ? "<" : "", module_names[module]);
    return chain_id(text);
  }
  if (!seen_pe) return chain_id("[no PE frame]");
  if (k < (int)sizeof(text) - 8) snprintf(text + k, sizeof(text) - k, "%s<-", shown > 4 ? "<.." : "");
  return chain_id(text);
}

static int
owner_of(uint64_t lr, uint64_t fp, int *chain) {
  uint64_t frames[25];
  int n = 0, jumps = 0;
  frames[n++] = lr;
  const int depth_max = chain ? 24 : 8;
  for (int depth = 0; depth < depth_max && fp && !(fp & 7); ++depth) {
    uint64_t rec[2];
    if (!read_mem(fp, rec, sizeof(rec))) break;
    const uint64_t ret = rec[1] & ADDR_MASK;
    if (!ret) break;
    frames[n++] = ret;
    // the chain must go up the stack; one downward jump is the switch from the unix (kernel) stack to the PE stack
    if (rec[0] <= fp && (!chain || jumps++ || pe_image_of(ret) >= 0)) break;
    fp = rec[0] & ADDR_MASK;
  }
  if (chain) *chain = -1;
  for (int i = 0; i < n; ++i) {
    if (is_libsystem_lock_frame(frames[i])) continue;
    char name[96];
    for (int k = 0; k < pe_count; ++k)
      if (frames[i] >= pe_images[k].base && frames[i] < pe_images[k].end) {
        snprintf(name, sizeof(name), "%s+0x%llx", module_names[pe_images[k].module], (unsigned long long)(frames[i] - pe_images[k].base));
        if (chain) *chain = chain_id("[owner is PE]");
        return owner_id(name);
      }
    Dl_info info;
    if (dladdr((const void *)frames[i], &info) && info.dli_fname) {
      const char *base = strrchr(info.dli_fname, '/');
      base = base ? base + 1 : info.dli_fname;
      // module offset as well: dladdr names the nearest EXPORTED symbol, static functions (Wine's msync_wait_single ...) are
      // resolved offline from the module's local symbol table (nm).
      snprintf(name, sizeof(name), "%s+0x%llx(%s)", base, (unsigned long long)(frames[i] - (uint64_t)info.dli_fbase),
               info.dli_sname ? info.dli_sname : "?");
    } else snprintf(name, sizeof(name), "?@0x%llx", (unsigned long long)frames[i]);
    if (chain) *chain = pe_chain_of(frames, i + 1, n);
    return owner_id(name);
  }
  return owner_id("[only libsystem frames]");
}

static void
pe_count_add(struct acc *a, int64_t num, int owner, int chain, int running) {
  for (int i = 0; i < MAX_PECH; ++i) {
    struct pechent *e = &a->sys_pe[i];
    if (e->count && e->num == num && e->owner == owner && e->chain == chain && e->running == running) { ++e->count; return; }
    if (!e->count) { e->num = num; e->owner = owner; e->chain = chain; e->running = running; e->count = 1; return; }
  }
  ++a->sys_pe_overflow;
}

static void
sys_count(struct acc *a, int waiting, int64_t num, int module) {
  struct sysent *t = waiting ? a->sys_wait : a->sys_run;
  for (int i = 0; i < MAX_SYS; ++i) {
    if (t[i].count && t[i].num == num && t[i].module == module) { ++t[i].count; return; }
    if (!t[i].count) { t[i].num = num; t[i].module = module; t[i].count = 1; return; }
  }
  ++a->sys_overflow;
}

static int
sys_print(char *line, int k, int size, const char *label, struct sysent *t) {
  if (k >= size - 64) return k;
  k += snprintf(line + k, size - k, " %s=", label);
  for (int n = 0; n < 12 && k < size - 80; ++n) {
    int best = -1;
    for (int i = 0; i < MAX_SYS; ++i)
      if (t[i].count && (best < 0 || t[i].count > t[best].count)) best = i;
    if (best < 0) break;
    k += snprintf(line + k, size - k, "%s%lld@%s:%llu", n ? "," : "", (long long)t[best].num,
                  t[best].module >= 0 ? module_names[t[best].module] : "?", (unsigned long long)t[best].count);
    t[best].count = 0;
  }
  return k;
}

static void
report(unsigned window, double seconds) {
  if (seconds <= 0) seconds = 1;
  for (int i = 0; i < acc_count; ++i) {
    struct acc *a = &accs[i];
    char line[4096];
    int k = snprintf(line, sizeof(line), "pc_sampler window=%u tid=%llu name=%s cpu_ms_per_s=%.1f samples=%llu running=%llu wait=%llu",
                     window, (unsigned long long)a->tid, a->name, a->t_last > a->t_first ? (double)(a->cpu_last - a->cpu_first) * 1e3 / (double)(a->t_last - a->t_first) : 0.0,
                     (unsigned long long)a->samples, (unsigned long long)a->running, (unsigned long long)a->wait);
    for (int c = 0; c < C_COUNT && k < (int)sizeof(line); ++c)
      k += snprintf(line + k, sizeof(line) - k, " run_%s=%llu", class_names[c], (unsigned long long)a->run[c]);
    for (int c = 0; c < C_COUNT && k < (int)sizeof(line); ++c)
      if (a->via[c]) k += snprintf(line + k, sizeof(line) - k, " via_%s=%llu", class_names[c], (unsigned long long)a->via[c]);
    if (sys_enabled && k < (int)sizeof(line) - 128) {
      k += snprintf(line + k, sizeof(line) - k, " sys_user_run=%llu sys_user_wait=%llu sys_overflow=%llu",
                    (unsigned long long)a->sys_user_run, (unsigned long long)a->sys_user_wait, (unsigned long long)a->sys_overflow);
      k = sys_print(line, k, (int)sizeof(line), "sys_run", a->sys_run);
      k = sys_print(line, k, (int)sizeof(line), "sys_wait", a->sys_wait);
      if (stack_enabled && k < (int)sizeof(line) - 64) {
        k += snprintf(line + k, sizeof(line) - k, " sys_owner=");
        for (int n = 0; n < 12 && k < (int)sizeof(line) - 128; ++n) {
          int best = -1;
          for (int i = 0; i < MAX_OWN; ++i)
            if (a->sys_own[i].count && (best < 0 || a->sys_own[i].count > a->sys_own[best].count)) best = i;
          if (best < 0) break;
          k += snprintf(line + k, sizeof(line) - k, "%s%lld|%s:%llu", n ? "," : "", (long long)a->sys_own[best].num,
                        owner_names[a->sys_own[best].module], (unsigned long long)a->sys_own[best].count);
          a->sys_own[best].count = 0;
        }
      }
      if (pechain_enabled) {
        // a line of its own: the chains are long
        char pl[4096];
        int q = snprintf(pl, sizeof(pl), "pc_sampler_pe window=%u tid=%llu name=%s overflow=%llu sys_pe=", window, (unsigned long long)a->tid,
                         a->name, (unsigned long long)a->sys_pe_overflow);
        for (int n = 0; n < 16 && q < (int)sizeof(pl) - 400; ++n) {
          int best = -1;
          for (int i = 0; i < MAX_PECH; ++i)
            if (a->sys_pe[i].count && (best < 0 || a->sys_pe[i].count > a->sys_pe[best].count)) best = i;
          if (best < 0) break;
          const struct pechent *e = &a->sys_pe[best];
          q += snprintf(pl + q, sizeof(pl) - q, "%s%lld|%s|%s|%s:%llu", n ? "," : "", (long long)e->num, e->running ? "r" : "w", owner_names[e->owner],
                        e->chain >= 0 ? chain_names[e->chain] : "?", (unsigned long long)e->count);
          a->sys_pe[best].count = 0;
        }
        if (q < (int)sizeof(pl) - 1) { pl[q++] = '\n'; pl[q] = 0; }
        fputs(pl, stderr);
      }
    }
    if (k < (int)sizeof(line)) k += snprintf(line + k, sizeof(line) - k, " top=");
    for (int t = 0; t < 10 && k < (int)sizeof(line) - 64; ++t) {
      int best = -1;
      for (int m = 0; m < module_count; ++m)
        if (a->modules[m] && (best < 0 || a->modules[m] > a->modules[best])) best = m;
      if (best < 0) break;
      k += snprintf(line + k, sizeof(line) - k, "%s%s:%llu", t ? "," : "", module_names[best], (unsigned long long)a->modules[best]);
      a->modules[best] = 0;
    }
    if (k < (int)sizeof(line) - 1) { line[k++] = '\n'; line[k] = 0; }
    fputs(line, stderr);
  }
  fprintf(stderr, "pc_sampler_cost window=%u sampler_cpu_ms_per_s=%.3f samples_per_s=%.1f pe_images=%d\n", window,
          (double)(sampler_cpu_ns_window) / 1e6 / seconds, (double)(sampler_samples_window) / seconds, pe_count);
  fflush(stderr);
  acc_count = 0;
  sampler_cpu_ns_window = sampler_samples_window = 0;
}

static unsigned
env_unsigned(const char *name, unsigned def, unsigned limit) {
  const char *v = getenv(name);
  if (!v || !*v) return def;
  char *end;
  unsigned long x = strtoul(v, &end, 0);
  if (*end || x > limit) return def;
  return (unsigned)x;
}

static void *
sampler_main(void *arg) {
  (void)arg;
  pthread_setname_np("macrunner-pc-sampler");
  const unsigned hz = env_unsigned("MACRUNNER_PC_SAMPLER", 0, 1000);
  const unsigned want = env_unsigned("MACRUNNER_PC_SAMPLER_THREADS", 4, MAX_PICK);
  sys_enabled = env_unsigned("MACRUNNER_PC_SAMPLER_SYSCALLS", 0, 1) == 1;
  stack_enabled = sys_enabled && env_unsigned("MACRUNNER_PC_SAMPLER_STACK", 0, 1) == 1;
  pechain_enabled = stack_enabled && env_unsigned("MACRUNNER_PC_SAMPLER_PECHAIN", 0, 1) == 1;
  const unsigned window_s = env_unsigned("MACRUNNER_PC_SAMPLER_WINDOW", 10, 3600);
  const char *forced = getenv("MACRUNNER_PC_SAMPLER_TID");
  const uint64_t forced_tid = forced ? strtoull(forced, NULL, 0) : 0;
  const unsigned delay_s = env_unsigned("MACRUNNER_PC_SAMPLER_DELAY", 2, 3600);
  fprintf(stderr, "pc_sampler start hz=%u threads=%u window_s=%u delay_s=%u forced_tid=%llu syscalls=%d stack=%d pechain=%d\n", hz, want,
          window_s, delay_s, (unsigned long long)forced_tid, sys_enabled, stack_enabled, pechain_enabled);
  sleep(delay_s);
  const uint64_t period = 1000000000ull / hz;
  uint64_t next = now_ns(), second_start = next, window_start = next, last_scan = 0;
  unsigned window = 0;
  uint64_t jitter = now_ns() ^ 0x9e3779b97f4a7c15ull;
  scan_pe_images();
  last_scan = now_ns();
  fprintf(stderr, "pc_sampler_scan images=%d\n", pe_count);
  refresh_picks(want, forced_tid, window, 0, 0);
  for (;;) {
    // Jittered period (uniform 0.5..1.5 x): a fixed 20 ms grid aliases with periodic work (frames, 4 ms chunks of the stand).
    jitter = jitter * 6364136223846793005ull + 1442695040888963407ull;
    next += period / 2 + (uint64_t)(((jitter >> 33) * (unsigned __int128)period) >> 31);
    uint64_t t = now_ns();
    if (next > t) {
      struct timespec ts = {(time_t)((next - t) / 1000000000ull), (long)((next - t) % 1000000000ull)};
      nanosleep(&ts, NULL);
    } else if (t - next > 10 * period) next = t;   // fell behind (machine asleep): do not burst
    const uint64_t c0 = self_cpu_ns();
    for (int i = 0; i < pick_count; ++i) {
      struct pick *p = &picks[i];
      thread_extended_info_data_t ext;
      mach_msg_type_number_t c1 = THREAD_EXTENDED_INFO_COUNT;
      if (thread_info(p->port, THREAD_EXTENDED_INFO, (thread_info_t)&ext, &c1) != KERN_SUCCESS) continue;
      arm_thread_state64_t st;
      mach_msg_type_number_t c2 = ARM_THREAD_STATE64_COUNT;
      if (thread_get_state(p->port, ARM_THREAD_STATE64, (thread_state_t)&st, &c2) != KERN_SUCCESS) continue;
      const uint64_t pc = (uint64_t)arm_thread_state64_get_pc(st) & ADDR_MASK;
      const uint64_t lr = (uint64_t)arm_thread_state64_get_lr(st) & ADDR_MASK;
      struct acc *a = acc_for(p->tid, p->name, ext.pth_user_time + ext.pth_system_time, now_ns());
      if (!a) continue;
      ++a->samples;
      int module, lr_module;
      const int cls = classify(pc, &module);
      const int running = ext.pth_run_state == TH_STATE_RUNNING;
      if (running) {
        ++a->running;
        ++a->run[cls];
        ++a->modules[module];
        if (cls == C_METAL || cls == C_KERNEL || cls == C_SYSTEM) ++a->via[classify(lr, &lr_module)];
      } else {
        ++a->wait;
      }
      if (sys_enabled && cls == C_KERNEL) {
        uint32_t prev = 0;
        if (pc >= 4 && read_mem(pc - 4, &prev, 4) && prev == 0xd4001001u) {
          int caller = -1;
          classify(lr, &caller);
          sys_count(a, !running, (int64_t)st.__x[16], caller);
          if (stack_enabled) {
            int chain = -1;
            const int owner = owner_of(lr, (uint64_t)arm_thread_state64_get_fp(st) & ADDR_MASK, pechain_enabled ? &chain : NULL);
            own_count(a, (int64_t)st.__x[16], owner);
            if (pechain_enabled) pe_count_add(a, (int64_t)st.__x[16], owner, chain, running);
          }
        } else if (running) ++a->sys_user_run;
        else ++a->sys_user_wait;
      }
      ++sampler_samples_window;
    }
    t = now_ns();
    if (t - second_start >= 1000000000ull) {
      if (t - last_scan >= 5000000000ull) { scan_pe_images(); last_scan = now_ns(); }
      const int report_now = t - window_start >= (uint64_t)window_s * 1000000000ull;
      const double seconds = (double)(t - window_start) / 1e9;
      refresh_picks(want, forced_tid, window, seconds, report_now);
      sampler_cpu_ns_window += self_cpu_ns() - c0;
      if (report_now) { ++window; window_start = t; }
      second_start = t;
      continue;
    }
    sampler_cpu_ns_window += self_cpu_ns() - c0;
  }
  return NULL;
}

__attribute__((constructor)) static void
pc_sampler_init(void) {
  const unsigned hz = env_unsigned("MACRUNNER_PC_SAMPLER", 0, 1000);
  if (!hz) return;
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
  pthread_t thread;
  if (pthread_create(&thread, &attr, sampler_main, NULL))
    fprintf(stderr, "pc_sampler: pthread_create failed\n");
  pthread_attr_destroy(&attr);
}
