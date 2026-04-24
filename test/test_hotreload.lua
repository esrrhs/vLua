-- test_hotreload.lua
-- 模拟热更场景：一边采样，一边用 loadstring 创建新函数、跑一阵、丢弃。
-- 旧 Proto 在 GC 后被回收，ring buffer 里如果还持有旧 Proto* 就会
-- use-after-free。新版 vlua 已经把关键数据拷贝到 Sample，Proto 回收
-- 不影响报告。
package.cpath = "../bin/?.so;" .. package.cpath
local v = require "libvlua"

local function make_func(tag)
    -- 每次生成一份全新 Lua 源码，每次加载都是新的 Proto
    local src = ([[
        return function(n)
            local t = {}
            for i = 1, n do
                t[TAG .. i] = i   -- 触发 luaH_newkey + 字符串 intern
            end
            return t
        end
    ]]):gsub("TAG", "'"..tag.."'")
    return load(src)()
end

print("start sampling luaH_newkey during simulated hot-reload...")
v.start("luaH_newkey", "hotreload.pro")

-- 模拟 20000 次热更循环，保证采样能打到 luaH_newkey
for round = 1, 20000 do
    local f = make_func("r" .. round)
    f(1000)          -- 跑一下，产生采样命中
    f = nil          -- 丢弃，让 GC 回收 Proto
    if round % 50 == 0 then
        collectgarbage("collect")  -- 强制 full GC，回收 Proto
    end
end

v.stop()
print("done. no crash -> hot-reload safety OK")
print("see hotreload.pro")
