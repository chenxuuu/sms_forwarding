local notify = {}

--你的wifi名称和密码
local wifiName = ""
local wifiPasswd = ""

--server酱的SendKey
--https://sct.ftqq.com/sendkey 申请一个
local serverKey = ""

--缓存消息
local buff = {}

--来新消息了
function notify.add(phone,data)
    log.info("notify","got sms",phone,data)
    table.insert(buff,{phone,data})
    sys.publish("SMS_ADD")--推个事件
end


sys.taskInit(function()
    sys.wait(1000)
    wlan.init()--初始化wifi
    wlan.connect(wifiName, wifiPasswd)
    log.info("wlan", "wait for IP_READY")
    sys.waitUntil("IP_READY", 30000)
    if wlan.ready() then
        log.info("wlan", "ready !!")
        while true do
            while #buff > 0 do--把消息读完
                local sms = table.remove(buff,1)
                local code, headers, body =
                    http2.request(
                        "POST",
                        "https://sctapi.ftqq.com/"..serverKey..".send",
                        {["Content-Type"] = "application/x-www-form-urlencoded"},
                        "title="..string.urlEncode("短信来自"..sms[1]).."&desp="..string.urlEncode(sms[2])
                    ).wait()
                log.info("notify","pushed sms notify",code,body,sms[1],sms[2])
            end
            log.info("notify","wait for a new sms~")
            sys.waitUntil("SMS_ADD")
        end
    else
        print("wlan NOT ready!!!!")
        rtos.reboot()
    end
end)



return notify
