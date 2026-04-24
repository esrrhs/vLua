-- test_getstr.lua
-- 模拟实际业务的多层调用，各分支调用量不同
package.cpath = "../bin/?.so;" .. package.cpath
local v = require "libvlua"

local player = {
    role = {
        pos = { x = 0, y = 0, z = 0 },
        battle = {
            stat = { kill = 0, death = 0, assist = 0 },
            weapon = { ammo = 100, damage = 50 },
        },
        bag = {
            items = {},
            capacity = 100,
        },
    },
}

-- 叶子函数们，各自做不同量的 table get-by-string

local function calc_damage()
    for i = 1, 3000 do
        player.role.battle.weapon.damage = player.role.battle.weapon.damage + 1
    end
end

local function calc_ammo()
    for i = 1, 1000 do
        player.role.battle.weapon.ammo = player.role.battle.weapon.ammo - 1
    end
end

local function update_kill()
    for i = 1, 4000 do
        player.role.battle.stat.kill = player.role.battle.stat.kill + 1
    end
end

local function update_death()
    for i = 1, 500 do
        player.role.battle.stat.death = player.role.battle.stat.death + 1
    end
end

local function update_pos()
    for i = 1, 2000 do
        player.role.pos.x = player.role.pos.x + 1
        player.role.pos.y = player.role.pos.y + 1
    end
end

local function check_bag()
    for i = 1, 800 do
        local _ = player.role.bag.capacity
    end
end

-- 中间层函数

local function process_battle()
    calc_damage()
    calc_ammo()
    update_kill()
    update_death()
end

local function process_move()
    update_pos()
end

local function process_bag()
    check_bag()
end

-- 顶层

local function on_tick()
    process_battle()
    process_move()
    process_bag()
end

local function main()
    for i = 1, 3000 do
        on_tick()
    end
end

v.start("luaV_execute", "getstr.pro")
main()
local text = v.stop()
print(text)
