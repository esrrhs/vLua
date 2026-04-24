-- test_hotreload_stress.lua
-- 极端压力：每轮创建新函数、运行、丢弃、强制 GC、立即大量分配新内存
-- 尽可能让释放的 Proto 内存被新对象覆盖——如果 vlua 在 signal handler
-- 里 deref 旧 Proto*，这种场景必然 segfault。
package.cpath = "../bin/?.so;" .. package.cpath
local v = require "libvlua"

local function make_func(tag)
    local src = ([[
        return function(n)
            local t = {}
            for i = 1, n do
                t[TAG .. i] = i
            end
            return t
        end
    ]]):gsub("TAG", "'"..tag.."'")
    return load(src)()
end

print("start sampling luaH_newkey during AGGRESSIVE hot-reload stress...")
v.start("luaH_newkey", "hotreload_stress.pro")

for round = 1, 30000 do
    local f = make_func("R" .. round)
    f(500)
    f = nil
    -- 每 10 轮强制 GC 后立刻分配大量临时对象，尽力覆盖刚释放的 Proto 内存
    if round % 10 == 0 then
        collectgarbage("collect")
        local trash = {}
        for j = 1, 500 do trash[j] = { j, tostring(j), {j, j, j} } end
        trash = nil
    end
end

collectgarbage("collect")
collectgarbage("collect")

v.stop()
print("done. no crash -> aggressive hot-reload safety OK")
print("see hotreload_stress.pro")
