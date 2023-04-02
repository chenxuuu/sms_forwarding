local notify = {}

--你的wifi名称和密码
local wifiName = ""
local wifiPasswd = ""

--这里默认用的是LuatOS社区提供的推送服务，无使用限制
--官网：https://push.luatos.org/ 点击GitHub图标登陆即可
--支持邮件/企业微信/钉钉/飞书/电报/IOS Bark

--是否使用server酱，false则使用LuatOS社区提供的推送服务
local useServerChan = false
local usePushover = false

--LuatOS社区提供的推送服务 https://push.luatos.org/
--这里填.send前的字符串就好了
--如：https://push.luatos.org/ABCDEF1234567890ABCD.send/{title}/{data} 填入 ABCDEF1234567890ABCD
local luatosPush = "ABCDEF1234567890ABCD"

--server酱的配置，免费用户每天仅可发送五条推送消息
--server酱的SendKey，如果你用的是这个就需要填一个
--https://sct.ftqq.com/sendkey 申请一个
local serverKey = ""

--pushover配置
local pushoverApiToken = ""
local pushoverUserKey = ""


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
    print("gc1",collectgarbage("count"))
    if wlan.ready() then
        log.info("wlan", "ready !!")
        while true do
            print("gc2",collectgarbage("count"))
            while #buff > 0 do--把消息读完
                collectgarbage("collect")--防止内存不足
                local sms = table.remove(buff,1)
                local code,h, body
                local data = pdu.ucs2_utf8(sms[2])
                if useServerChan then--server酱
                    log.info("notify","send to serverChan",data)
                    code, h, body = http.request(
                            "POST",
                            "https://sctapi.ftqq.com/"..serverKey..".send",
                            {["Content-Type"] = "application/x-www-form-urlencoded"},
                            "title="..string.urlEncode("sms"..sms[1]).."&desp="..string.urlEncode(data)
                        ).wait()
                    log.info("notify","pushed sms notify",code,h,body,sms[1])
                elseif usePushover then --Pushover
                    log.info("notify","send to Pushover",data)
                    local body = {
                        token = pushoverApiToken,
                        user = pushoverUserKey,
                        message = data
                    }
                    local json_body = string.gsub(json.encode(body), "\\b", "\\n")
                    code, h, body = http.request(
                            "POST",
                            "https://api.pushover.net/1/messages.json",
                            {["Content-Type"] = "application/json; charset=utf-8"},
                            json_body
                        ).wait()
                    log.info("notify","pushed sms notify",code,h,body,sms[1])
                else--luatos推送服务
                    data = data:gsub("%%","%%25")
                    :gsub("+","%%2B")
                    :gsub("/","%%2F")
                    :gsub("?","%%3F")
                    :gsub("#","%%23")
                    :gsub("&","%%26")
                    local url = "https://push.luatos.org/"..luatosPush..".send/sms"..sms[1].."/"..data
                    log.info("notify","send to luatos push server",data,url)
                    --多试几次好了
                    for i=1,10 do
                        code, h, body = http.request("GET",url).wait()
                        log.info("notify","pushed sms notify",code,h,body,sms[1])
                        if code == 200 then
                            break
                        end
                        sys.wait(5000)
                    end
                end
            end
            log.info("notify","wait for a new sms~")
            print("gc3",collectgarbage("count"))
            sys.waitUntil("SMS_ADD")
        end
    else
        print("wlan NOT ready!!!!")
        rtos.reboot()
    end
end)



return notify
