// vlua.cpp - SIGPROF-based Lua VM C function sampler
//
// 用途：在外网精确归因某个 Lua VM C 函数（如 luaH_getshortstr）
// 的 CPU 消耗到具体的 Lua 源码行。
//
// 原理：
//   1. 启动时通过 dlsym/dladdr1 拿到目标 C 函数的地址与符号大小，
//      得到 PC 范围 [addr, addr+size)。
//   2. setitimer(ITIMER_PROF) 周期触发 SIGPROF。
//   3. Signal handler 里：
//        - 读被中断的 PC（uc_mcontext）
//        - 若 PC 落在目标函数范围内，从 L->ci 拿到 (Proto*, pc_offset)
//        - 写到无锁 ring buffer（固定大小 int 数组，只做原子 +1）
//      Handler 中不调用任何 Lua API，不分配内存。
//   4. stop 时再把 (Proto*, pc_offset) 解析为 source:line，聚合输出报告。
//
// 指针稳定性假设：采样期间 Proto 对象不被 GC 回收——函数未 hot-reload 即成立。

#include <string>
#include <vector>
#include <unordered_map>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cstdarg>
#include <ctime>
#include <sys/time.h>
#include <signal.h>
#include <unistd.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <errno.h>
#include <link.h>
#include <ucontext.h>
#include <elf.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <pthread.h>
#include <setjmp.h>

extern "C" {
#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"
#include "lobject.h"
#include "lstate.h"
#include "lstring.h"
}

// ----------------------------------------------------------------------------
// 日志（与 pLua 风格一致）
// ----------------------------------------------------------------------------

static const int open_debug = 0;

static void vlog(const char *header, const char *file, const char *func, int pos,
                 const char *fmt, ...) {
    FILE *pLog = fopen("vlua.log", "a+");
    if (pLog == NULL) {
        return;
    }

    time_t clock1 = time(0);
    struct tm *tptr = localtime(&clock1);
    struct timeval tv;
    gettimeofday(&tv, NULL);

    fprintf(pLog, "===========================[%d.%d.%d, %d.%d.%d %llu]%s:%d,%s:===========================\n%s",
            tptr->tm_year + 1900, tptr->tm_mon + 1, tptr->tm_mday,
            tptr->tm_hour, tptr->tm_min, tptr->tm_sec,
            (long long) ((tv.tv_sec) * 1000 + (tv.tv_usec) / 1000),
            file, pos, func, header);

    va_list ap;
    va_start(ap, fmt);
    vfprintf(pLog, fmt, ap);
    fprintf(pLog, "\n\n");
    va_end(ap);
    fclose(pLog);
}

#define VLOG(...)  if (open_debug) { vlog("[DEBUG] ", __FILE__, __FUNCTION__, __LINE__, __VA_ARGS__); }
#define VERR(...)              { vlog("[ERROR] ", __FILE__, __FUNCTION__, __LINE__, __VA_ARGS__); }

// ----------------------------------------------------------------------------
// 采样配置
// ----------------------------------------------------------------------------

// 采样间隔（微秒）。10000us = 10ms = 100Hz，对齐 pLua 的 CPU_SAMPLE_ITER。
// 100Hz 下 signal 投递本身的开销约 0.03%，可以长时间常开。
static const int SAMPLE_INTERVAL_US = 10 * 1000;

// Ring buffer 容量（条数）。100Hz 下可连续采样约 40 分钟不回绕。
static const size_t RING_BUFFER_SIZE = 1 << 18;

// Sample 内嵌的 source 字符串最大长度（截断）
static const size_t SAMPLE_SRC_MAX = 127;

// 每个采样最大栈深度（同 pLua 的 MAX_STACK_SIZE）
static const int MAX_STACK_DEPTH = 64;

// ----------------------------------------------------------------------------
// 全局状态
// ----------------------------------------------------------------------------

// 一帧栈信息：文件名 + 行号（自包含，不持有 Lua 堆指针）
struct StackFrame {
    char source[SAMPLE_SRC_MAX + 1];
    int  line;
};

// 一次完整的采样：完整 Lua 调用栈（从栈底到栈顶）
struct Sample {
    int depth;                            // 有效帧数
    StackFrame frames[MAX_STACK_DEPTH];   // frames[0] = 栈底，frames[depth-1] = 栈顶
};

// 目标 C 函数的 PC 范围
static uintptr_t g_target_addr = 0;
static uintptr_t g_target_end  = 0;
static std::string g_target_name;

// 当前 profile 的 lua_State
// 注意：signal handler 通过此全局访问，用 volatile 保证可见性
static lua_State *volatile g_L = NULL;

// 是否正在运行。signal handler 里用 volatile 读，start/stop 里在单线程
// 主流程里写；SIGPROF 只打断当前线程，不存在跨线程写竞争。
static volatile sig_atomic_t g_running = 0;

// Ring buffer：单线程 Lua VM 假设下，signal handler 和主线程不会并发
// 访问这些计数器：
//  - SIGPROF handler 执行期间，同线程不会再收到第二个 SIGPROF
//    （kernel 保证 handler 执行期间该信号被 masked）
//  - stop() 在主线程走到 resolve_and_report 前已经关定时器 + pthread_sigmask
//    栅栏，此时不会再有 handler 在跑
// 因此用普通 volatile 计数器即可，避免原子操作开销。
static Sample *g_samples = NULL;
static volatile uint64_t g_sample_write_idx  = 0;  // 写入位置
static volatile uint64_t g_sample_hit_count  = 0;  // 命中目标函数次数
static volatile uint64_t g_sample_total_tick = 0;  // 总 SIGPROF 次数

// 输出文件
static std::string g_filename;

// ----------------------------------------------------------------------------
// 工具：从 ELF 拿到符号地址和大小
// ----------------------------------------------------------------------------

// 通过 dlsym 查符号（只能找到 DYNAMIC 段里的导出符号）
static int resolve_symbol_dlsym(const char *name, uintptr_t *out_addr, size_t *out_size) {
    void *addr = dlsym(RTLD_DEFAULT, name);
    if (!addr) {
        return -1;
    }

    Dl_info info;
    ElfW(Sym) *sym = NULL;
    if (dladdr1(addr, &info, (void**)&sym, RTLD_DL_SYMENT) == 0 || !sym) {
        return -1;
    }

    *out_addr = (uintptr_t) addr;
    *out_size = (size_t) sym->st_size;
    return 0;
}

// 从 ELF 静态 symtab 里找 symbol
// 这是为了应对：像 hive 这种把 Lua 静态链接进来的可执行文件，
// luaH_getshortstr 等内部函数往往是 'static'/'local' 符号，
// 不在 .dynsym 中，dlsym 找不到，但存在于 .symtab 中（除非 strip）。
//
// 实现思路：打开文件（由 dl_iterate_phdr 的回调给出路径和加载基址）、
// mmap 只读、扫 section header 找 .symtab 和 .strtab，遍历符号表。
struct SymtabSearchCtx {
    const char *name;
    uintptr_t   found_addr;
    size_t      found_size;
    int         found;
};

static int search_symtab_in_file(const char *path, uintptr_t load_base,
                                 SymtabSearchCtx *ctx) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        return -1;
    }

    struct stat st;
    if (fstat(fd, &st) != 0) {
        close(fd);
        return -1;
    }

    void *map = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (map == MAP_FAILED) {
        return -1;
    }

    const uint8_t *base = (const uint8_t *) map;
    const ElfW(Ehdr) *eh = (const ElfW(Ehdr) *) base;
    if (memcmp(eh->e_ident, ELFMAG, SELFMAG) != 0) {
        munmap(map, st.st_size);
        return -1;
    }

    const ElfW(Shdr) *sh = (const ElfW(Shdr) *) (base + eh->e_shoff);
    const ElfW(Shdr) *symtab = NULL;
    const ElfW(Shdr) *strtab = NULL;

    for (int i = 0; i < eh->e_shnum; i++) {
        if (sh[i].sh_type == SHT_SYMTAB) {
            symtab = &sh[i];
            if (sh[i].sh_link < eh->e_shnum) {
                strtab = &sh[sh[i].sh_link];
            }
            break;
        }
    }

    int ok = -1;
    if (symtab && strtab) {
        const ElfW(Sym) *syms = (const ElfW(Sym) *) (base + symtab->sh_offset);
        const char *strs = (const char *) (base + strtab->sh_offset);
        size_t n = symtab->sh_size / symtab->sh_entsize;

        for (size_t i = 0; i < n; i++) {
            const ElfW(Sym) *s = &syms[i];
            if (ELF64_ST_TYPE(s->st_info) != STT_FUNC) {
                continue;
            }
            if (s->st_size == 0) {
                continue;
            }
            const char *sn = strs + s->st_name;
            if (strcmp(sn, ctx->name) == 0) {
                // 对于可执行文件（ET_EXEC），st_value 是绝对地址，load_base 通常 0
                // 对于 PIE/共享库（ET_DYN），需要加上 load_base
                uintptr_t addr = (uintptr_t) s->st_value;
                if (eh->e_type == ET_DYN) {
                    addr += load_base;
                }
                ctx->found_addr = addr;
                ctx->found_size = s->st_size;
                ctx->found = 1;
                ok = 0;
                break;
            }
        }
    }

    munmap(map, st.st_size);
    return ok;
}

static int dl_iter_cb(struct dl_phdr_info *info, size_t sz, void *data) {
    (void) sz;
    SymtabSearchCtx *ctx = (SymtabSearchCtx *) data;
    if (ctx->found) {
        return 1;
    }

    const char *path = info->dlpi_name;
    // 主程序 dlpi_name 通常为 ""，需要替换成 /proc/self/exe
    char buf[1024];
    if (!path || !*path) {
        ssize_t r = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
        if (r > 0) {
            buf[r] = 0;
            path = buf;
        }
        else return 0;
    }

    search_symtab_in_file(path, (uintptr_t) info->dlpi_addr, ctx);
    return ctx->found ? 1 : 0;
}

static int resolve_symbol_symtab(const char *name, uintptr_t *out_addr, size_t *out_size) {
    SymtabSearchCtx ctx;
    ctx.name = name;
    ctx.found = 0;
    ctx.found_addr = 0;
    ctx.found_size = 0;
    dl_iterate_phdr(dl_iter_cb, &ctx);
    if (!ctx.found) {
        return -1;
    }
    *out_addr = ctx.found_addr;
    *out_size = ctx.found_size;
    return 0;
}

// 优先 dlsym（快），失败再扫 ELF symtab（能找到 static 符号）
static int resolve_symbol(const char *name, uintptr_t *out_addr, size_t *out_size) {
    if (resolve_symbol_dlsym(name, out_addr, out_size) == 0 && *out_size > 0) {
        return 0;
    }
    if (resolve_symbol_symtab(name, out_addr, out_size) == 0 && *out_size > 0) {
        return 0;
    }
    VERR("resolve_symbol(%s) failed (not in .dynsym nor .symtab). "
         "Make sure the binary is not stripped.", name);
    return -1;
}

// ----------------------------------------------------------------------------
// Signal handler —— 必须保持最小，只做异步信号安全的操作
// ----------------------------------------------------------------------------

// Thread-local jmp_buf：用于在 SIGPROF handler 读可疑指针时，如果触发
// SIGSEGV/SIGBUS 能回跳放弃本次采样。
// 使用 __thread 而不是 thread_local，避免依赖 C++11 运行时初始化（signal 安全）
static __thread sigjmp_buf  tl_segv_jmp;
static __thread volatile int tl_segv_armed = 0;
static __thread volatile int tl_segv_hit   = 0;

// SIGSEGV/SIGBUS handler：只在 vlua handler 执行期间（tl_segv_armed=1）接管
// 其他时候保持原语义（chain 到应用自己的 handler 或内核默认行为）
static struct sigaction g_prev_sigsegv;
static struct sigaction g_prev_sigbus;

static void segv_handler(int sig, siginfo_t *si, void *ucontext) {
    if (tl_segv_armed) {
        tl_segv_hit = 1;
        siglongjmp(tl_segv_jmp, 1);
        // never return
    }
    // 不是 vlua 触发的 SEGV，转发给原 handler
    struct sigaction *prev = (sig == SIGBUS) ? &g_prev_sigbus : &g_prev_sigsegv;
    if (prev->sa_flags & SA_SIGINFO) {
        if (prev->sa_sigaction) {
            prev->sa_sigaction(sig, si, ucontext);
        }
    } else {
        if (prev->sa_handler == SIG_DFL) {
            // 恢复默认行为并重新触发（让进程该崩就崩）
            signal(sig, SIG_DFL);
            raise(sig);
        } else if (prev->sa_handler == SIG_IGN) {
            // ignore
        } else if (prev->sa_handler) {
            prev->sa_handler(sig);
        }
    }
}

// 跨平台读 PC：x86_64 用 REG_RIP，aarch64 用 pc
static inline uintptr_t get_pc_from_ucontext(const ucontext_t *uc) {
#if defined(__x86_64__)
    return (uintptr_t) uc->uc_mcontext.gregs[REG_RIP];
#elif defined(__i386__)
    return (uintptr_t) uc->uc_mcontext.gregs[REG_EIP];
#elif defined(__aarch64__)
    return (uintptr_t) uc->uc_mcontext.pc;
#else
    return 0;
#endif
}

// 判断 ci 是否是一个 Lua 函数帧（复制自 lstate.h 的 isLua 宏）
static inline int ci_is_lua(const CallInfo *ci) {
    return ci->callstatus & CIST_LUA;
}

// 检查 TValue 是否是 Lua closure（不用 check_exp 以避免 assert）
// LUA_TLCL | BIT_ISCOLLECTABLE = 0x46
static inline int tvalue_is_lclosure(const TValue *o) {
    return (o->tt_ & 0x7F) == (LUA_TLCL | (1 << 6)); // ctb(LUA_TLCL)
}

// 校验指针对齐，防御 signal 打断瞬间读到半更新状态
static inline int ptr_aligned(const void *p, size_t align) {
    return ((uintptr_t) p & (align - 1)) == 0;
}

// 在 signal handler 里做"尽力防御"：如果 L 正在栈扩容、ci 链表正在被
// 修改，我们可能读到脏数据。但只要通过了这些 sanity check，访问
// 就不会 segfault（最多数据不准确，对统计热点无影响）。
//
// 极端情况（如热更 + GC 把 Proto 内存回收给 OS）下仍可能 SIGSEGV，
// 这种情况由 SEGV trampoline (sigsetjmp/siglongjmp) 兜底，放弃本次采样。
//
// 这里的关键前提：signal 打断时，CPU 正在执行目标 C 函数（如
// luaH_getshortstr），说明当前线程正在 VM 调度中，L->ci 指向的 Lua
// 帧对应的 closure/proto 在绝大多数情况下是 rooted 的——它就在 Lua 栈上。
// 数据拷贝完成后就不再持有指针，后续报告阶段绝对安全。
// 从一个 CallInfo 提取 source 和 line 到 StackFrame。
// 返回 1 成功，0 失败（跳过该帧）。
// 调用方保证 ci 非空、已过 isLua 检查。
static inline int extract_frame(const CallInfo *ci, const lua_State *L, StackFrame *out) {
    StkId func = ci->func;
    if (!func || !ptr_aligned(func, 8)) {
        return 0;
    }
    if (L->stack && L->stack_last) {
        if (func < L->stack || func > L->stack_last) {
            return 0;
        }
    }
    if (!tvalue_is_lclosure(func)) {
        return 0;
    }

    const GCObject *gc = func->value_.gc;
    if (!gc || !ptr_aligned(gc, 8)) {
        return 0;
    }

    const LClosure *cl = (const LClosure *) gc;
    const Proto *p = cl->p;
    if (!p || !ptr_aligned(p, 8)) {
        return 0;
    }

    // 解析行号
    int line = -1;
    const Instruction *code = p->code;
    int sizecode = p->sizecode;
    int *lineinfo = p->lineinfo;
    int sizelineinfo = p->sizelineinfo;
    if (code && sizecode > 0 && lineinfo && sizelineinfo > 0) {
        const Instruction *savedpc = ci->u.l.savedpc;
        if (savedpc) {
            ptrdiff_t d = savedpc - code - 1;
            if (d >= 0 && d < sizecode && d < sizelineinfo) {
                line = lineinfo[(int) d];
            }
        }
    }

    // 拷贝 source 字符串
    out->source[0] = 0;
    out->line = line;
    TString *src_ts = p->source;
    if (src_ts && ptr_aligned(src_ts, 8)) {
        const char *src = (const char *) src_ts + sizeof(UTString);
        for (size_t i = 0; i < SAMPLE_SRC_MAX; i++) {
            char c = src[i];
            out->source[i] = c;
            if (c == 0) {
                break;
            }
        }
        out->source[SAMPLE_SRC_MAX] = 0;
    }
    return 1;
}

static void signal_handler(int sig, siginfo_t *si, void *ucontext) {
    (void) sig; (void) si;

    g_sample_total_tick++;

    if (!g_running) {
        return;
    }

    ucontext_t *uc = (ucontext_t *) ucontext;
    uintptr_t pc = get_pc_from_ucontext(uc);

    // 绝大多数 SIGPROF 到这里就 return，开销极低
    if (pc < g_target_addr || pc >= g_target_end) {
        return;
    }

    lua_State *L = g_L;
    if (!L) {
        return;
    }

    // ---- 架起 SEGV 安全网 ----
    if (sigsetjmp(tl_segv_jmp, 1) != 0) {
        tl_segv_armed = 0;
        return;
    }
    tl_segv_armed = 1;

    if (!ptr_aligned(L, 8)) {
        tl_segv_armed = 0;
        return;
    }

    // ---- 遍历 ci 链，收集所有 Lua 帧（从栈顶到栈底）----
    StackFrame tmp_frames[MAX_STACK_DEPTH];
    int tmp_depth = 0;

    CallInfo *ci = L->ci;
    while (ci && tmp_depth < MAX_STACK_DEPTH) {
        if (!ptr_aligned(ci, 8)) {
            break;
        }
        if (ci_is_lua(ci)) {
            if (extract_frame(ci, L, &tmp_frames[tmp_depth])) {
                tmp_depth++;
            }
        }
        ci = ci->previous;
    }

    tl_segv_armed = 0;

    if (tmp_depth == 0) {
        return;
    }

    // ---- 写入 ring buffer：翻转为栈底在前（和 pLua 一致）----
    uint64_t idx = g_sample_write_idx++;
    Sample *slot = &g_samples[idx & (RING_BUFFER_SIZE - 1)];
    slot->depth = tmp_depth;
    for (int i = 0; i < tmp_depth; i++) {
        slot->frames[i] = tmp_frames[tmp_depth - 1 - i];
    }

    g_sample_hit_count++;
}

// ----------------------------------------------------------------------------
// start / stop
// ----------------------------------------------------------------------------

static int start_impl(lua_State *L, const char *func_name, const char *file) {
    if (g_running) {
        VERR("vlua already running");
        return -1;
    }

    uintptr_t addr = 0;
    size_t size = 0;
    if (resolve_symbol(func_name, &addr, &size) != 0) {
        VERR("resolve_symbol(%s) failed", func_name);
        return -1;
    }

    g_target_name = func_name;
    g_target_addr = addr;
    g_target_end  = addr + size;
    g_filename    = file;
    g_L           = L;

    // 分配 ring buffer
    if (!g_samples) {
        g_samples = (Sample *) calloc(RING_BUFFER_SIZE, sizeof(Sample));
        if (!g_samples) {
            VERR("alloc ring buffer failed");
            return -1;
        }
    } else {
        memset(g_samples, 0, sizeof(Sample) * RING_BUFFER_SIZE);
    }
    g_sample_write_idx = 0;
    g_sample_hit_count = 0;
    g_sample_total_tick = 0;

    // 安装 SIGPROF handler
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = signal_handler;
    sa.sa_flags = SA_RESTART | SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGPROF, &sa, NULL) != 0) {
        VERR("sigaction(SIGPROF) failed: %s", strerror(errno));
        return -1;
    }

    // 安装 SIGSEGV/SIGBUS 兜底 handler（只在 vlua 采样区间接管）
    struct sigaction sa_segv;
    memset(&sa_segv, 0, sizeof(sa_segv));
    sa_segv.sa_sigaction = segv_handler;
    // SA_NODEFER 允许 handler 内再次触发同信号（siglongjmp 后不阻塞）
    sa_segv.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigemptyset(&sa_segv.sa_mask);
    if (sigaction(SIGSEGV, &sa_segv, &g_prev_sigsegv) != 0) {
        VERR("sigaction(SIGSEGV) failed: %s", strerror(errno));
        return -1;
    }
    if (sigaction(SIGBUS, &sa_segv, &g_prev_sigbus) != 0) {
        VERR("sigaction(SIGBUS) failed: %s", strerror(errno));
        sigaction(SIGSEGV, &g_prev_sigsegv, NULL);
        return -1;
    }

    g_running = 1;

    // 启动定时器
    struct itimerval timer;
    timer.it_interval.tv_sec  = 0;
    timer.it_interval.tv_usec = SAMPLE_INTERVAL_US;
    timer.it_value = timer.it_interval;
    if (setitimer(ITIMER_PROF, &timer, NULL) != 0) {
        VERR("setitimer failed: %s", strerror(errno));
        g_running = 0;
        return -1;
    }

    printf("vlua started: target=%s addr=%p size=%zu file=%s\n",
           func_name, (void*)addr, size, file);
    return 0;
}

// ----------------------------------------------------------------------------
// 报告聚合与输出
// ----------------------------------------------------------------------------

// pLua 兼容常量（与 plua.cpp / plua.go 一致）
static const int PLUA_MAX_FUNC_NAME_SIZE = 127;
static const int PLUA_MAX_STACK_SIZE = 64;

// pLua 的 IGNORE_NAME 起始 ID 数量，vlua 不需要忽略名字，但 ID 从
// VALID_MIN_ID 开始分配，保持和 pLua 工具链兼容。
static const int VALID_MIN_ID = 9;

struct CallStack {
    int depth;
    int stack[PLUA_MAX_STACK_SIZE];
};

struct CallStackHash {
    size_t operator()(const CallStack &cs) const {
        size_t hash = 0;
        for (int i = 0; i < cs.depth; i++) {
            int id = cs.stack[i];
            hash = (hash << 8) | (hash >> (8 * (sizeof(hash) - 1)));
            hash += (id * 31) + (id * 7) + (id * 3);
        }
        return hash;
    }
};

struct CallStackEqual {
    bool operator()(const CallStack &cs1, const CallStack &cs2) const {
        if (cs1.depth != cs2.depth) {
            return false;
        }
        return memcmp(cs1.stack, cs2.stack, sizeof(int) * cs1.depth) == 0;
    }
};

static void flush_file(int fd, const char *buf, size_t len) {
    while (len > 0) {
        ssize_t r = write(fd, buf, len);
        if (r <= 0) {
            break;
        }
        buf += r;
        len -= r;
    }
}

// 把 source 字符串单行化（load(str) 产生的 chunk source 含多行源码）
static std::string sanitize_source(const std::string &in) {
    std::string s = in;
    for (char &c : s) {
        if (c == '\n' || c == '\r' || c == '\t') {
            c = ' ';
        }
    }
    if (s.size() > 80) {
        s = s.substr(0, 77) + "...";
    }
    return s;
}

// 为 StackFrame 生成显示名字：source:line
static std::string frame_name(const StackFrame &f) {
    char buf[256];
    std::string src = sanitize_source(f.source);
    if (f.line > 0) {
        snprintf(buf, sizeof(buf), "%s:%d", src.c_str(), f.line);
    } else {
        snprintf(buf, sizeof(buf), "%s:<unknown>", src.c_str());
    }
    return buf;
}

// 写 pLua 兼容的 .pro 二进制文件。
// 格式与 plua.cpp flush() 完全一致：
//   [N 个 CallStack 条目]: count(int32) + CallStack{depth(int32) + stack[64](int32×64)}
//   [M 个字符串条目]:      name(变长bytes) + name_len(int32) + id(int32)
//   [尾部 4 字节]:          M(int32) = 字符串条目总数
//
// 每帧对应一个 string ID（"@file.lua:27"），完整栈直接映射为 CallStack。
static void flush_pro_file(uint64_t to_scan) {
    int fd = open(g_filename.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0666);
    if (fd < 0) {
        VERR("open %s failed", g_filename.c_str());
        return;
    }

    std::unordered_map<std::string, int> str2id;
    int next_id = VALID_MIN_ID;

    // 为每个 Sample 构造 CallStack（用 string ID），聚合相同 CallStack 的计数
    std::unordered_map<CallStack, int, CallStackHash, CallStackEqual> cs_agg;

    for (uint64_t i = 0; i < to_scan; i++) {
        const Sample &s = g_samples[i];
        if (s.depth <= 0) {
            continue;
        }

        CallStack cs;
        memset(&cs, 0, sizeof(cs));
        cs.depth = s.depth;

        for (int j = 0; j < s.depth; j++) {
            std::string name = frame_name(s.frames[j]);
            auto it = str2id.find(name);
            if (it == str2id.end()) {
                str2id[name] = next_id;
                cs.stack[j] = next_id;
                next_id++;
            } else {
                cs.stack[j] = it->second;
            }
        }

        cs_agg[cs]++;
    }

    // 写 CallStack 条目
    for (auto &kv : cs_agg) {
        const CallStack &cs = kv.first;
        int count = kv.second;
        flush_file(fd, (const char *) &count, sizeof(count));
        flush_file(fd, (const char *) &cs, sizeof(cs));
    }

    // 写字符串表
    int total_len = 0;
    for (auto &kv : str2id) {
        const std::string &str = kv.first;
        int id = kv.second;

        int len = (int) str.length();
        if (len > PLUA_MAX_FUNC_NAME_SIZE) {
            len = PLUA_MAX_FUNC_NAME_SIZE;
        }
        flush_file(fd, str.c_str(), len);
        flush_file(fd, (const char *) &len, sizeof(len));
        flush_file(fd, (const char *) &id, sizeof(id));
        total_len++;
    }

    // 写尾部：字符串条目总数
    flush_file(fd, (const char *) &total_len, sizeof(total_len));
    close(fd);
}

// 生成文本报告（返回 string，不写文件）
// 统计维度：栈顶帧的 source:line（即直接触发目标 C 函数的 Lua 行）
static std::string build_text_report(
    uint64_t total_hit, uint64_t total_tick, uint64_t written, uint64_t to_scan)
{
    // 按栈顶帧聚合
    struct LineCount { std::string name; uint64_t count; };
    std::unordered_map<std::string, uint64_t> top_agg;

    for (uint64_t i = 0; i < to_scan; i++) {
        const Sample &s = g_samples[i];
        if (s.depth <= 0) {
            continue;
        }
        // 栈顶 = frames[depth-1]
        std::string name = frame_name(s.frames[s.depth - 1]);
        top_agg[name]++;
    }

    std::vector<LineCount> sorted;
    sorted.reserve(top_agg.size());
    for (auto &kv : top_agg) {
        sorted.push_back({kv.first, kv.second});
    }
    std::sort(sorted.begin(), sorted.end(),
              [](const LineCount &a, const LineCount &b){ return a.count > b.count; });

    uint64_t analysable = 0;
    for (auto &kv : top_agg) {
        analysable += kv.second;
    }

    char buf[1024];
    std::string report;

    report += "====================  vLua sampling report  ====================\n";
    snprintf(buf, sizeof(buf), "target function   : %s\n", g_target_name.c_str());
    report += buf;
    snprintf(buf, sizeof(buf), "target addr range : [%p, %p)  size=%zu\n",
            (void*) g_target_addr, (void*) g_target_end,
            (size_t)(g_target_end - g_target_addr));
    report += buf;
    snprintf(buf, sizeof(buf), "output file       : %s\n", g_filename.c_str());
    report += buf;
    snprintf(buf, sizeof(buf), "sample interval   : %d us\n", SAMPLE_INTERVAL_US);
    report += buf;
    snprintf(buf, sizeof(buf), "ring buffer size  : %zu\n", (size_t) RING_BUFFER_SIZE);
    report += buf;
    report += "\n";
    snprintf(buf, sizeof(buf), "total SIGPROF ticks        : %llu\n", (unsigned long long) total_tick);
    report += buf;
    snprintf(buf, sizeof(buf), "hits in target function    : %llu  (%.2f%% of ticks)\n",
            (unsigned long long) total_hit,
            total_tick > 0 ? 100.0 * total_hit / total_tick : 0.0);
    report += buf;
    snprintf(buf, sizeof(buf), "samples written to buffer  : %llu\n",
            (unsigned long long) (written < RING_BUFFER_SIZE ? written : RING_BUFFER_SIZE));
    report += buf;
    if (written > RING_BUFFER_SIZE) {
        snprintf(buf, sizeof(buf), "  * WARN: ring buffer overflowed, %llu samples discarded\n",
                (unsigned long long) (written - RING_BUFFER_SIZE));
        report += buf;
    }
    snprintf(buf, sizeof(buf), "samples analysable         : %llu\n", (unsigned long long) analysable);
    report += buf;
    report += "\n";

    report += "----------------------------------------------------------------\n";
    report += "Top hotspots (source:line -> count, pct of analysable)\n";
    report += "----------------------------------------------------------------\n";
    snprintf(buf, sizeof(buf), "%8s  %8s  %s\n", "count", "pct", "location");
    report += buf;
    size_t top_n = sorted.size() < 100 ? sorted.size() : 100;
    for (size_t i = 0; i < top_n; i++) {
        double pct = analysable > 0 ? 100.0 * sorted[i].count / analysable : 0.0;
        snprintf(buf, sizeof(buf), "%8llu  %7.2f%%  %s\n",
                (unsigned long long) sorted[i].count, pct, sorted[i].name.c_str());
        report += buf;
    }
    report += "\n";
    report += "================================================================\n";
    report += "(use plua.go + pprof on the .pro file for full callstack / flamegraph)\n";

    return report;
}

// stop 返回的文本报告
static std::string g_text_report;

static int stop_impl(lua_State *L) {
    (void) L;
    if (!g_running) {
        VERR("vlua not running");
        return -1;
    }

    // 先置 running 为 0，正在执行的 handler 走完当前这次后下次进来直接 return
    g_running = 0;

    // 关闭定时器：此后不会再有新的 SIGPROF 产生
    struct itimerval timer;
    memset(&timer, 0, sizeof(timer));
    setitimer(ITIMER_PROF, &timer, NULL);

    // 关键同步：确保当前线程没有在执行 handler。
    // 方法：短暂阻塞 SIGPROF，再解除。阻塞/解除系统调用会确保内核
    // 将任何 pending SIGPROF 要么投递要么丢弃，而已在执行的 handler
    // 会在此系统调用返回前完成（同线程不能并发执行 signal handler）。
    sigset_t block, old;
    sigemptyset(&block);
    sigaddset(&block, SIGPROF);
    pthread_sigmask(SIG_BLOCK, &block, &old);
    pthread_sigmask(SIG_SETMASK, &old, NULL);

    // 此时可以安全读 ring buffer
    uint64_t total_hit  = g_sample_hit_count;
    uint64_t total_tick = g_sample_total_tick;
    uint64_t written    = g_sample_write_idx;
    uint64_t to_scan    = written < RING_BUFFER_SIZE ? written : RING_BUFFER_SIZE;

    // 写 pLua 兼容的二进制 .pro 文件（完整栈）
    flush_pro_file(to_scan);

    // 生成文本报告（栈顶帧聚合）
    g_text_report = build_text_report(total_hit, total_tick, written, to_scan);

    printf("vlua stopped: %llu ticks, %llu hits (%.2f%%), report -> %s\n",
           (unsigned long long) total_tick,
           (unsigned long long) total_hit,
           total_tick > 0 ? 100.0 * total_hit / total_tick : 0.0,
           g_filename.c_str());

    // 恢复原 SIGSEGV/SIGBUS handler
    sigaction(SIGSEGV, &g_prev_sigsegv, NULL);
    sigaction(SIGBUS,  &g_prev_sigbus,  NULL);

    // 清理
    g_L = NULL;
    g_sample_write_idx = 0;
    g_sample_hit_count = 0;
    g_sample_total_tick = 0;
    return 0;
}

// ----------------------------------------------------------------------------
// Lua binding
// ----------------------------------------------------------------------------

// vlua.start(func_name, output_file)
static int l_start(lua_State *L) {
    const char *func_name = luaL_checkstring(L, 1);
    const char *file = luaL_checkstring(L, 2);
    int ret = start_impl(L, func_name, file);
    lua_pushinteger(L, ret);
    return 1;
}

// vlua.stop() -> string|nil
// 成功时返回文本报告，失败返回 nil
static int l_stop(lua_State *L) {
    int ret = stop_impl(L);
    if (ret == 0) {
        lua_pushstring(L, g_text_report.c_str());
    } else {
        lua_pushnil(L);
    }
    return 1;
}

extern "C" int luaopen_libvlua(lua_State *L) {
    luaL_checkversion(L);
    luaL_Reg l[] = {
        {"start", l_start},
        {"stop",  l_stop},
        {NULL, NULL},
    };
    luaL_newlib(L, l);
    return 1;
}
