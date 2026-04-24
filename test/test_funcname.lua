-- test_funcname.lua
-- 验证 try_get_funcname_from_caller 的所有 case
package.cpath = "../bin/?.so;" .. package.cpath
local v = require "libvlua"

------------------------------------------------------------------------
-- Case 1: GETTABUP (全局函数)
------------------------------------------------------------------------
function global_func()
    local s = 0
    for i = 1, 3000 do
        s = s + i
    end
    return s
end

------------------------------------------------------------------------
-- Case 2: GETTABLE (table 字段调用)
------------------------------------------------------------------------
local module = {}
function module.table_func()
    local s = 0
    for i = 1, 3000 do
        s = s + i
    end
    return s
end

------------------------------------------------------------------------
-- Case 3: SELF (方法调用 obj:method())
------------------------------------------------------------------------
local obj = {}
function obj:method_func()
    local s = 0
    for i = 1, 3000 do
        s = s + i
    end
    return s
end

------------------------------------------------------------------------
-- Case 4: GETUPVAL (upvalue，闭包捕获的外层 local)
------------------------------------------------------------------------
local function upval_func()
    local s = 0
    for i = 1, 3000 do
        s = s + i
    end
    return s
end

------------------------------------------------------------------------
-- Case 5: MOVE (local 变量直接调用)
------------------------------------------------------------------------
local function make_caller()
    local local_func = function()
        local s = 0
        for i = 1, 3000 do
            s = s + i
        end
        return s
    end
    return function()
        local_func()  -- MOVE + CALL
    end
end
local call_local = make_caller()

------------------------------------------------------------------------
-- 调用入口
------------------------------------------------------------------------
local function run_all()
    for i = 1, 20000 do
        global_func()        -- case 1: GETTABUP
        module.table_func()  -- case 2: GETTABLE
        obj:method_func()    -- case 3: SELF
        upval_func()         -- case 4: GETUPVAL
        call_local()         -- case 5: MOVE (inside make_caller's closure)
    end
end

-- 验证字节码
print("=== bytecode of run_all ===")
os.execute("luac -l -l test_funcname.lua 2>&1 | grep -A1 'CALL' | head -40")

print("\n=== sampling ===")
v.start("luaV_execute", "funcname.pro")
run_all()
local text = v.stop()
print(text)

print("=== collapsed ===")
os.execute("cd ../tools && ./vlua -i ../test/funcname.pro -pprof /tmp/funcname.prof 2>&1 && ./pprof --collapsed /tmp/funcname.prof 2>/dev/null")
