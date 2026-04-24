-- test_vlua.lua
-- 构造 a.b.c 风格的大量 table get-by-string，验证 vlua 能定位到热点行
package.cpath = "../bin/?.so;" .. package.cpath

local v = require "libvlua"

-- 构造一个深嵌套的大表
local player = {
    role = {
        pos = { x = 0, y = 0, z = 0 },
        battle = {
            stat = { kill = 0, death = 0, assist = 0 },
        },
    },
    bag = {
        items = {},
    },
}
for i = 1, 100 do
    player.bag.items[i] = { id = i, count = 1, expire = 0 }
end

-- hot_a: 深嵌套链式访问，每轮 12 次 getshortstr
local function hot_a()
    for i = 1, 2000 do
        player.role.pos.x = player.role.pos.x + 1
        player.role.pos.y = player.role.pos.y + 1
        player.role.pos.z = player.role.pos.z + 1
    end
end

-- hot_b: 循环里每轮 3 次 getshortstr
local function hot_b()
    for i = 1, 2000 do
        player.role.battle.stat.kill = player.role.battle.stat.kill + 1
    end
end

-- cold_a: 已缓存的局部变量（应该不会出现在热点前列）
local function cold_a()
    local pos = player.role.pos
    for i = 1, 2000 do
        pos.x = pos.x + 1
        pos.y = pos.y + 1
        pos.z = pos.z + 1
    end
end

local function work()
    for _ = 1, 5000 do
        hot_a()
        hot_b()
        cold_a()
    end
end

print("start sampling luaH_getshortstr...")
v.start("luaH_getshortstr", "call.pro")

work()

v.stop()
print("done. see call.pro")

-- luaH_newkey 函数范围较大（~1KB），在本机也容易命中，用于验证
-- 机制正确性
print("")
print("start sampling luaH_newkey (for validation)...")
v.start("luaH_newkey", "call_newkey.pro")

-- 构造大量新 key 触发 luaH_newkey
local t = {}
for i = 1, 2000000 do
    t["k" .. i] = i
end

v.stop()
print("done. see call_newkey.pro")

-- 再用 luaV_execute 验证一下机制（范围大，必然命中）
print("")
print("start sampling luaV_execute (for mechanism validation)...")
v.start("luaV_execute", "call_vexec.pro")

work()

v.stop()
print("done. see call_vexec.pro")
