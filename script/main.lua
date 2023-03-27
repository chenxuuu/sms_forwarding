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
        local _,phone,data,time,long,total,id = sys.waitUntil("AT_CMT")
        if long and id == 1 then--是长短信！而且是第一条
            log.info("air780","found a long sms",total,id)
            --缓存，长短信存放处
            local smsTemp = {data}
            --发件人
            local lastPhone = phone
            --等待下一条
            for i=1,total do
                local r,phone,dataTemp,time,long,total,id = sys.waitUntil("AT_CMT",60000)
                if r then
                    if phone == lastPhone then
                        log.info("air780","a part of long sms",total,id)
                        table.insert(smsTemp,dataTemp)
                        if #smsTemp == total then
                            --收到完整的长短信
                            data = table.concat(smsTemp)
                            log.info("air780","got a long sms")
                            break
                        end
                    else--手机号不一样了？那就是两条不同的短信了
                        data = table.concat(smsTemp)--这是长短信的内容
                        log.info("air780","a long sms cut")
                        notify.add(phone,dataTemp)--这次的短信
                        break
                    end
                end
            end
        end
        notify.add(phone,data)--这次的短信
    end
end)



-- 用户代码已结束---------------------------------------------
-- 结尾总是这一句
sys.run()
-- sys.run()之后后面不要加任何语句!!!!!
