// ptrace dlopen injector, android arm64, root. usage: injector <pid> <lib.so>
#include <sys/ptrace.h>
#include <sys/wait.h>
#include <sys/uio.h>
#include <asm/ptrace.h>
#include <elf.h>
#include <dlfcn.h>
#include <unistd.h>
#include <errno.h>
#include <stdint.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>

static uintptr_t RemoteBase(pid_t pid, const char* path) {
    char f[64]; snprintf(f, sizeof f, "/proc/%d/maps", pid);
    FILE* m = fopen(f, "r"); if (!m) return 0;
    char line[1024]; uintptr_t r = 0;
    while (fgets(line, sizeof line, m)) {
        unsigned long s = 0, off = 1; char perm[8];
        if (sscanf(line, "%lx-%*lx %7s %lx", &s, perm, &off) == 3 && off == 0 && strstr(line, path)) { r = s; break; }
    }
    fclose(m); return r;
}
static bool Poke(pid_t pid, uintptr_t addr, const void* data, size_t n) {
    const uint8_t* p = (const uint8_t*)data;
    for (size_t i = 0; i < n; i += 8) {
        long w = 0; size_t c = n - i < 8 ? n - i : 8;
        if (c < 8) { errno = 0; w = ptrace(PTRACE_PEEKDATA, pid, addr + i, 0); }
        memcpy(&w, p + i, c);
        if (ptrace(PTRACE_POKEDATA, pid, addr + i, w) < 0) return false;
    }
    return true;
}
static bool GetRegs(pid_t pid, user_pt_regs* r) { iovec v{r, sizeof *r}; return ptrace(PTRACE_GETREGSET, pid, (void*)NT_PRSTATUS, &v) == 0; }
static bool SetRegs(pid_t pid, user_pt_regs* r) { iovec v{r, sizeof *r}; return ptrace(PTRACE_SETREGSET, pid, (void*)NT_PRSTATUS, &v) == 0; }

int main(int argc, char** argv) {
    if (argc < 3) { puts("usage: injector <pid> <lib.so>"); return 1; }
    pid_t pid = atoi(argv[1]); std::string lib = argv[2];
    void* d = dlsym(RTLD_DEFAULT, "dlopen"); Dl_info di{};
    if (!d || !dladdr(d, &di)) { puts("no local dlopen"); return 2; }
    uintptr_t rb = RemoteBase(pid, di.dli_fname);
    if (!rb) { printf("remote %s not found\n", di.dli_fname); return 2; }
    uintptr_t target = (uintptr_t)d - (uintptr_t)di.dli_fbase + rb;
    if (ptrace(PTRACE_ATTACH, pid, 0, 0) < 0) { perror("attach"); return 3; }
    int st = 0; waitpid(pid, &st, __WALL);
    user_pt_regs saved{}, regs{};
    if (!GetRegs(pid, &saved)) { perror("getregs"); ptrace(PTRACE_DETACH, pid, 0, 0); return 3; }
    regs = saved;
    uintptr_t str = (regs.sp - 0x400 - (lib.size() + 1)) & ~0xFULL;
    if (!Poke(pid, str, lib.c_str(), lib.size() + 1)) { perror("poke"); ptrace(PTRACE_DETACH, pid, 0, 0); return 3; }
    regs.regs[0] = str; regs.regs[1] = 2 /*RTLD_NOW*/; regs.regs[30] = 0; regs.pc = target; regs.sp = (str - 0x80) & ~0xFULL;
    int nr = -1; iovec sv{&nr, sizeof nr}; ptrace(PTRACE_SETREGSET, pid, (void*)0x404 /*NT_ARM_SYSTEM_CALL*/, &sv); // no syscall restart
    SetRegs(pid, &regs);
    ptrace(PTRACE_CONT, pid, 0, 0); waitpid(pid, &st, __WALL); // returns to pc=0 -> SIGSEGV
    user_pt_regs after{}; GetRegs(pid, &after);
    printf("dlopen -> 0x%llx (stop sig %d)\n", (unsigned long long)after.regs[0], WIFSTOPPED(st) ? WSTOPSIG(st) : -1);
    SetRegs(pid, &saved); ptrace(PTRACE_DETACH, pid, 0, 0);
    return after.regs[0] ? 0 : 4;
}
