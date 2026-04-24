# vLua

基于 SIGPROF 的 Lua VM C 函数采样器，用于把某个 Lua 内部 C 函数
（如 `luaH_getshortstr`、`luaH_newkey`、`internshrstr` 等）的 CPU
消耗**归因到具体的 Lua 源码行**。

## 背景

perf 能看到 `luaH_getshortstr` 占 13% CPU，但看不到**是哪些 Lua 代码行
在贡献这 13%**。vLua 解决的就是这个问题。

## 原理

1. `dlsym` / ELF `.symtab` 找到目标 C 函数的地址与大小 `[addr, addr+size)`
2. `setitimer(ITIMER_PROF)` 周期触发 SIGPROF（默认 10ms / 100Hz）
3. Signal handler 里：
   - 读被中断的 PC（`uc->uc_mcontext`）
   - PC 不在目标范围直接 return（99% 的采样走这里）
   - 否则从 `L->ci` 回溯到最近一个 Lua 帧，读 Proto 字段、拷贝
     `source`、算出行号
   - 一次性写入 ring buffer 的一个自包含槽位（单线程假设下无需原子）
4. `stop()` 时 handler 已写完所有值类型数据，直接聚合输出报告

signal handler 全程不调 Lua API，不分配内存，不持锁，不 deref 任何
可能被 GC 的指针。

## 构建

```bash
./build.sh
```

输出 `bin/libvlua.so`。Lua 头文件来自 `dep/lua-5.3.6.tar.gz`（和 pLua 相同）。

## 使用

```lua
local v = require "libvlua"

-- 参数1：要采样的 C 函数名（必须能在 dlsym 或 ELF .symtab 找到）
-- 参数2：输出报告文件
v.start("luaH_getshortstr", "call.pro")

do_some_thing()

v.stop()
```

输出文件 `call.pro` 是文本报告，包含：

- 总 SIGPROF 采样次数、命中目标函数的比例
- **Top hotspots BY LINE**：按 `source:line` 聚合的热点行排行
- **Top hotspots BY FUNCTION**：按 Lua 函数聚合的排行

## 外网部署注意事项

1. **二进制不要 strip**：`luaH_getshortstr` 这类内部函数是 `static`
   符号，只存在于 `.symtab` 中。`strip` 后 vLua 找不到目标函数。
   - 验证方法：`nm <binary> | grep luaH_getshortstr` 应能看到符号
   - 如果用了 `strip --strip-debug`，`.symtab` 会被保留，没问题
   - 如果用了 `strip` 或 `strip --strip-all`，`.symtab` 被删除，vLua 不可用

2. **开销**：默认 100Hz 采样（对齐 pLua 的 `CPU_SAMPLE_ITER=10ms`），
   signal handler 里只做几次指针校验 + Proto 字段读取 + 短字符串拷贝，
   单线程假设下无原子操作。实测 overhead ≈ 0.03%，可长期常开。
   如果需要更高密度，把 `SAMPLE_INTERVAL_US` 改回 1000（1000Hz）即可，
   overhead 也只有约 0.3%。

3. **Ring buffer 容量**：默认 256K 条 × ~160B = ~40MB 内存。100Hz 下可
   连续采样约 40 分钟不回绕；溢出会在报告里提示 WARN。

4. **多线程 Lua VM**：当前实现假设单 Lua VM（`g_L`）。hive 是单 Lua VM，
   直接用即可。如果是多 lua_State 场景需要扩展。

## 热更安全

**vLua 被设计成在 Lua 热更期间常开也不会崩溃。** 三层防护：

### 1. Signal handler 里就地解析，ring buffer 存值不存指针

`Sample` 结构内嵌 `source[128]` 字符缓冲和几个 `int`，signal handler
触发时就把 `proto->source` 拷贝、`proto->lineinfo[pc]` 解析成行号，
然后写入 ring buffer。之后 `stop()` 报告阶段**完全不访问任何 Lua 堆
指针**——旧 Proto 即便被 GC 回收也无影响。

### 2. Signal handler 前置 sanity check

进入读 Proto 字段前，校验：
- `L`、`ci`、`func`、`gc`、`proto` 指针对齐
- `ci->callstatus & CIST_LUA` 确认是 Lua 帧
- `ci->func` 落在 `L->stack ~ L->stack_last` 范围内
- `ci->func` 的 tag 是 `ctb(LUA_TLCL)`（Lua closure）

任何一项不通过就放弃本次采样。这能挡住 signal 恰好落在"栈扩容中"
或"ci 链表重组中"的半更新状态。

### 3. SIGSEGV/SIGBUS 兜底 trampoline

极端情况下（如热更 + GC + jemalloc munmap 将旧 Proto 内存归还 OS），
deref 仍可能 SIGSEGV。vLua 在 `start()` 时安装自己的 SIGSEGV/SIGBUS
handler，配合线程局部的 `sigsetjmp`：若在 signal handler 执行期间
挂了，`siglongjmp` 回来放弃本次采样，**不影响进程运行**。非 vlua
区间的 SIGSEGV 会转发给原 handler，保持业务崩溃栈原样可观测。

### 验证

`test/test_hotreload_stress.lua` 里做了 3w 轮热更循环（`load() + 跑 +
丢弃 + GC + 大量新分配覆盖`），采样期间全程命中 `luaH_newkey`，
`exitcode=0` 无崩溃。

### 推荐实践

- 正常情况下，**采样期间直接放任热更发生即可**，vLua 不会崩。
- 如果追求数据精度，可以在热更前 `v.stop()` 生成一份报告，热更后
  `v.start()` 开新一轮。

## 验证

`test/test_vlua.lua` 构造了深嵌套链式访问 + 大量新 key 的场景：

- `luaH_newkey` 采样 100% 归因到 `t["k"..i] = i` 那一行 ✔
- `luaV_execute` 采样按 Lua 函数/行的相对热度正确分布 ✔

本机 `luaH_getshortstr` 只有 ~70 字节，函数体极短难以命中；外网 hive
上该函数占 13% CPU，采样自然命中。

## 和 pLua 的区别

|              | pLua                                  | vLua                             |
|--------------|---------------------------------------|----------------------------------|
| 定位         | Lua 函数调用栈级 profiler             | C 函数级 PC 采样 + Lua 行归因    |
| 采样机制     | SIGPROF → lua_sethook → 走 Lua 栈     | SIGPROF → PC 范围判断 + CI 回溯 |
| 适用场景     | 看"哪些 Lua 函数耗 CPU"               | 看"哪些 Lua 行在触发特定 C 热点" |
| 关注对象     | 业务层 Lua 函数                       | VM 内部 C 函数（table/string/GC）|
