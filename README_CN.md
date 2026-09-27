# vLua

[English](README.md) | [中文说明](README_CN.md)

[<img src="https://img.shields.io/github/license/esrrhs/vLua">](https://github.com/esrrhs/vLua)
[<img src="https://img.shields.io/github/languages/top/esrrhs/vLua">](https://github.com/esrrhs/vLua)
[<img src="https://img.shields.io/github/actions/workflow/status/esrrhs/vLua/ccpp.yml?branch=master">](https://github.com/esrrhs/vLua/actions)

把 Lua VM 内部 C 函数的 CPU 消耗精确归因到 Lua 源码行。

## 简介

perf 能看到 `luaH_getshortstr` 占 13% CPU，但看不到是哪些 Lua 代码在贡献这 13%。**vLua** 只在目标 C 函数占用 CPU 时采样，并把样本归因到 Lua 源码位置，同时输出完整调用栈。

## 特性

- **精准**：SIGPROF 定时采样 + PC 范围判断，只在目标 C 函数执行时捕获 Lua 调用栈
- **轻量**：100Hz 采样下 overhead ≈ 0.03%，可长期常开
- **安全**：signal handler 内就地解析，ring buffer 只存值类型；SIGSEGV trampoline 兜底，热更期间不会崩
- **兼容**：输出 [pLua](https://github.com/esrrhs/pLua) 格式的 `.pro` 文件，可直接用 pprof、[火焰图](https://github.com/brendangregg/FlameGraph) 工具链分析
- **灵活**：目标函数名作为参数传入，可以分析任意 Lua VM 内部 C 函数

## 编译

vLua 内置 **Lua 5.3.6** 头文件（需要私有 VM 结构体）。CMake 会自动解压 `dep/lua-5.3.6.tar.gz`。

```shell
./build.sh
# 或：
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

产物：

- `bin/libvlua.so` — 采样动态库
- `bin/vlua` — profile 转换工具（需本机有 Go）
- `build/lua/lua53` — 与头文件匹配的 Lua 5.3.6 解释器（用于测试）

## 测试

```shell
ctest --test-dir build --output-on-failure
```

或直接用内置解释器跑脚本：

```shell
cd test
../build/lua/lua53 test_getstr.lua
```

## 使用

### 修改 Lua 代码

```lua
local v = require "libvlua"

-- 参数1：要采样的 C 函数名
-- 参数2：采样结果文件（pLua 兼容的二进制格式）
v.start("luaH_getshortstr", "call.pro")

do_some_thing()

-- 结束采样，返回文本摘要报告
local text = v.stop()
print(text)
```

### 用 [hookso](https://github.com/esrrhs/hookso) 注入

```shell
# a) 获取进程中的 lua_State 指针，比如进程的 xxx.so 调用了 lua_settop(L)，取第一个参数
./hookso arg $PID xxx.so lua_settop 1
# 输出: 123456

# b) 加载 libvlua.so
./hookso dlopen $PID ./libvlua.so

# c) 开启采样，等价于 lrealstart(L, "luaH_getshortstr", "./call.pro")
./hookso call $PID libvlua.so lrealstart i=123456 s="luaH_getshortstr" s="./call.pro"

# d) 关闭采样，等价于 lrealstop(L)
./hookso call $PID libvlua.so lrealstop i=123456
```

## 可视化与工具

`tools/` 目录用于把 `.pro` 采样数据转换成火焰图 SVG 和调用图。

### 可选依赖

- **FlameGraph**：本地或 `PATH` 中没有时，`show.sh` 会从 GitHub 自动下载 `flamegraph.pl`。CentOS/RHEL 可能需要 `yum install perl-open`。
- **Graphviz**（调用图 PNG）：`sudo apt install graphviz` / `sudo yum install graphviz`
- **pprof**（调用图 DOT/PNG）：`sudo apt install google-perftools` / `sudo yum install gperftools`

### 生成可视化

```shell
cd tools
./show.sh ../test
```

会生成：

- `<name>.fl`：折叠调用栈
- `<name>.svg`：可交互火焰图
- `<name>.prof`：符号化 pprof profile
- `<name>.dot`：Graphviz 调用图
- `<name>.png`：调用图 PNG（需 graphviz + pprof）

`show.sh` 会优先使用 CMake 产出的 `bin/vlua`，否则在有 Go 时现场编译 `tools/vlua`。

## 示例

模拟 table get-by-string 热点场景（深嵌套 `player.role.battle.stat.kill` 等链式访问），采样 `luaH_getshortstr`：

#### 调用图

![image](test/getstr.png)

#### `v.stop()` 返回的文本摘要

```
Top hotspots (source:line -> count, pct of analysable)
   count       pct  location
     152    31.67%  @test_getstr.lua:24     -- calc_damage: player.role.battle.weapon.damage = ...
     131    27.29%  @test_getstr.lua:36     -- update_kill: player.role.battle.stat.kill = ...
      63    13.12%  @test_getstr.lua:49     -- update_pos: player.role.pos.y = ...
      57    11.88%  @test_getstr.lua:48     -- update_pos: player.role.pos.x = ...
      ...
```

## 注意事项

- 二进制不要 `strip`，`luaH_getshortstr` 等 static 符号只在 `.symtab` 中  
  验证：`nm <binary> | grep luaH_getshortstr`
- 热更期间可以常开，不会崩（三层防护：值类型 Sample + 指针校验 + SIGSEGV trampoline）
- 当前假设单 Lua VM（`g_L`），多 lua_State 场景需要扩展
- 头文件与测试解释器为 Lua **5.3.6**，宿主进程需使用匹配的 5.3.x VM 布局

## 其他

[lua全家桶](https://github.com/esrrhs/lua-family-bucket) · [pLua](https://github.com/esrrhs/pLua) · [hookso](https://github.com/esrrhs/hookso)

## License

MIT
