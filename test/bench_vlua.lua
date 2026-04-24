-- bench_vlua.lua - 测量 vlua 开启 vs 关闭的 workload 耗时
package.cpath = "../bin/?.so;" .. package.cpath
local v = require "libvlua"

-- 和 test_vlua.lua 相同的 workload，但重复多次取平均
local player = {
    role = { pos = {x=0,y=0,z=0}, battle = {stat = {kill=0,death=0,assist=0}} },
    bag = { items = {} },
}

local function hot_a()
    for i = 1, 2000 do
        player.role.pos.x = player.role.pos.x + 1
        player.role.pos.y = player.role.pos.y + 1
        player.role.pos.z = player.role.pos.z + 1
    end
end

local function hot_b()
    for i = 1, 2000 do
        player.role.battle.stat.kill = player.role.battle.stat.kill + 1
    end
end

local function work()
    for _ = 1, 5000 do
        hot_a(); hot_b()
    end
end

local function bench(label, fn)
    -- 预热
    fn()
    collectgarbage("collect")
    collectgarbage("stop")

    local samples = {}
    local rounds = 7
    for i = 1, rounds do
        local t0 = os.clock()
        fn()
        local dt = os.clock() - t0
        samples[#samples+1] = dt
    end
    collectgarbage("restart")

    table.sort(samples)
    -- 去掉最小最大，取中间 5 个平均
    local sum = 0
    for i = 2, rounds - 1 do sum = sum + samples[i] end
    local avg = sum / (rounds - 2)
    print(string.format("%-30s  avg=%.4fs  min=%.4fs  max=%.4fs",
          label, avg, samples[1], samples[rounds]))
    return avg
end

print("=== benchmark: vlua overhead ===")
local base = bench("baseline (no sampling)", work)

v.start("luaV_execute", "/tmp/bench_vexec.pro")  -- 命中率高 ~60%
local with_vexec = bench("sampling luaV_execute (~60% hit)", work)
v.stop()

v.start("luaH_getshortstr", "/tmp/bench_gs.pro")  -- 本机几乎不命中
local with_gs = bench("sampling luaH_getshortstr (~0% hit)", work)
v.stop()

v.start("luaH_newkey", "/tmp/bench_nk.pro")       -- 低命中
local with_nk = bench("sampling luaH_newkey (~5% hit)", work)
v.stop()

print(string.format("\noverhead (vs baseline):"))
print(string.format("  luaV_execute      : +%.2f%%", (with_vexec/base - 1) * 100))
print(string.format("  luaH_getshortstr  : +%.2f%%", (with_gs/base - 1) * 100))
print(string.format("  luaH_newkey       : +%.2f%%", (with_nk/base - 1) * 100))
