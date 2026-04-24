-- test_getstr.lua
-- 模拟实际业务：深嵌套 table 的 get-by-string 热点
package.cpath = "../bin/?.so;" .. package.cpath
local v = require "libvlua"

local player = {
    role = {
        pos = { x = 0, y = 0, z = 0 },
        battle = {
            stat = { kill = 0, death = 0, assist = 0 },
        },
    },
}

-- 模拟战斗结算：深链式访问 player.role.battle.stat.xxx
local function update_stat()
    for i = 1, 2000 do
        player.role.battle.stat.kill = player.role.battle.stat.kill + 1
        player.role.battle.stat.death = player.role.battle.stat.death + 1
        player.role.battle.stat.assist = player.role.battle.stat.assist + 1
    end
end

-- 模拟位置更新：player.role.pos.xxx
local function update_pos()
    for i = 1, 2000 do
        player.role.pos.x = player.role.pos.x + 1
        player.role.pos.y = player.role.pos.y + 1
        player.role.pos.z = player.role.pos.z + 1
    end
end

local function on_tick()
    update_stat()
    update_pos()
end

local function main()
    for i = 1, 5000 do
        on_tick()
    end
end

v.start("luaV_execute", "getstr.pro")
main()
local text = v.stop()
print(text)
