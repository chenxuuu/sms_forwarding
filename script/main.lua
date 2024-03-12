-- LuaTools需要PROJECT和VERSION这两个信息
PROJECT = "sms_forwarding"
VERSION = "1.0.0"

log.info("main", PROJECT, VERSION)

-- 引入必要的库文件(lua编写), 内部库不需要require
sys = require("sys")
require("sysplus")

if wdt then
    --添加硬狗防止程序卡死，在支持的设备上启用这个功能
    wdt.init(9000)--初始化watchdog设置为9s
    sys.timerLoopStart(wdt.feed, 3000)--3s喂一次狗
end

--检查一下固件版本，防止用户乱刷
do
    local fw = rtos.firmware():lower()--全转成小写
    local ver,bsp = fw:match("luatos%-soc_v(%d-)_(.+)")
    ver = ver and tonumber(ver) or nil
    local r
    if ver and bsp then
        if ver >= 1004 and bsp == "esp32c3" then
            r = true
        end
    end
    if not r then
        sys.timerLoopStart(function ()
            wdt.feed()
            log.info("警告","固件类型或版本不满足要求，请使用esp32c3 v1004及以上版本固件。当前："..rtos.firmware())
        end,500)
    end
end


--运营商给的dns经常抽风，手动指定
socket.setDNS(nil, 1, "119.29.29.29")
socket.setDNS(nil, 2, "223.5.5.5")

--定时GC一下
sys.timerLoopStart(function()
    collectgarbage("collect")
end, 1000)

--状态灯
led = require("led")
--串口处理
air780 = require("uartTask")
--服务器上传处理
notify = require("notify")

local function clear_table(table)
    for i = 0, #table do
        table[i] = nil
    end
end

local function fix_time(time)
    return string.format("20%s-%s-%s %s:%s:%s %s", time:sub(1,2), time:sub(4,5), time:sub(7,8),
    time:sub(10,11), time:sub(13,14), time:sub(16,17), time:sub(18,20))
end

local long_sms_buffer = {}

local function concat_and_send_long_sms(phone, time, sms)
    --拼接长短信
    local full_content = ""

    table.sort(sms, function(a,b) return a.id < b.id end)

    for _, v in ipairs(sms) do
        log.debug("long_sms", "message id: " .. v.id .. ", content: " .. v.data .. ", time: " .. v.time)
        full_content = full_content .. v.data
    end

    notify.add(phone, full_content)

    -- 清空缓冲区
    clear_table(sms)
end

local function clean_sms_buffer(phone, time)
    if not long_sms_buffer[phone] then
        return
    end

    if not long_sms_buffer[phone][time] then
        return
    end

    log.warn("sms", "long sms receive timeout from ", phone, time)
    if #long_sms_buffer[phone][time] > 0 then
        concat_and_send_long_sms(phone, time, long_sms_buffer[phone][time])
        long_sms_buffer[phone][time] = nil
    end
end

sys.taskInit(function()
    led.status = 1
    log.info("air780","sync at")
    --同步AT命令看通不通
    air780.loopAT("AT","AT_AT")

    --重启
    air780.write("AT+RESET")
    --同步AT命令看通不通（确保重启完）
    air780.loopAT("AT","AT_AT")
    air780.loopAT("ATE1","AT_ATE1")

    --关闭自动升级
    air780.loopAT("AT+UPGRADE=\"AUTO\",0","AT_UPGRADE")

    led.status = 3
    log.info("air780","check sim card")
    --检查下有没有卡
    local r = air780.loopAT("AT+CPIN?","AT_CPIN")
    if not r then
        log.error("air780","no sim card! exit script!!!!!!!!")
        led.status = 2
        return
    end

    --配置一下参数
    log.info("air780","configrate")
    --PDU模式
    air780.loopAT("AT+CMGF=0","AT_CMGF")
    --编码
    air780.loopAT("AT+CSCS=\"UCS2\"","AT_CSCS")
    --短信内容直接上报不缓存
    air780.loopAT("AT+CNMI=2,2,0,0,0","AT_CNMI")

    --检查附着
    log.info("air780","wait for connection")
    while true do
        local r = air780.loopAT("AT+CGATT?","AT_CGATT",1000)
        log.info("air780","connection status",r)
        if r then break end
    end
    led.status = 4
    log.info("air780","connected! wait sms")

    while true do
        collectgarbage("collect")--防止内存不足
        local _, phone, data, time, long, total, id = sys.waitUntil("AT_CMT")
        time = fix_time(time)

        if long then--是长短信！
            log.info("air780","receive a long sms", phone, id .. " / " .. total, time)
            --缓存，长短信存放处
            if not long_sms_buffer[phone] then
                long_sms_buffer[phone] = {}
            end
            if not long_sms_buffer[phone][time] then
                long_sms_buffer[phone][time] = {}
                sys.timerStart(clean_sms_buffer, 30*1000, phone, time)
            end

            table.insert(long_sms_buffer[phone][time], {id = id, data = data, time = time})

            if long_sms_buffer[phone][time] and #long_sms_buffer[phone][time] == total then
                concat_and_send_long_sms(phone, time, long_sms_buffer[phone][time])
                long_sms_buffer[phone][time] = nil
            end
        else
            log.info("air780", "receive a sms", phone, time)
            notify.add(phone, data)--这次的短信
        end
    end
end)



-- 用户代码已结束---------------------------------------------
-- 结尾总是这一句
sys.run()
-- sys.run()之后后面不要加任何语句!!!!!
