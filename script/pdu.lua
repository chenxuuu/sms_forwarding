--大部分代码来自：https://gitee.com/openLuat/Luat_Lua_Air724U/blob/master/script_LuaTask/lib/sms.lua

local libT = {}

--[[
函数名：numtobcdnum
功能  ：号码ASCII字符串 转化为 BCD编码格式字符串，仅支持数字和+，例如"+8618126324567" -> 91688121364265f7 （表示第1个字节是0x91，第2个字节为0x68，......）
参数  ：
num：待转换字符串
返回值：转换后的字符串
]]
local function numtobcdnum(num)
    local len, numfix, convnum = #num, "81", ""

    if num:sub(1, 1) == "+" then
        numfix = "91"
        len = len - 1
        num = num:sub(2, -1)
    end

    if len % 2 ~= 0 then --奇数位
        for i = 1, (len - (len % 2)) / 2 do
            convnum = convnum .. num:sub(i * 2, i * 2) .. num:sub(i * 2 - 1, i * 2 - 1)
        end
        convnum = convnum .. "F" .. num:sub(len, len)
    else --偶数位
        for i = 1, (len - (len % 2)) / 2 do
            convnum = convnum .. num:sub(i * 2, i * 2) .. num:sub(i * 2 - 1, i * 2 - 1)
        end
    end

    return numfix .. convnum
end

--[[
函数名：gsm8bitdecode
功能  ：8位编码
参数  ：data
longsms
返回值：
]]
local function gsm8bitdecode(data)
    local ucsdata, lpcnt = "", #data / 2
    for i = 1, lpcnt do
        ucsdata = ucsdata .. "00" .. data:sub((i - 1) * 2 + 1, i * 2)
    end
    return ucsdata, lpcnt
end


local Charmap = {[0] = 0x40, 0xa3, 0x24, 0xa5, 0xe8, 0xE9, 0xF9, 0xEC, 0xF2, 0xC7, 0x0A, 0xD8, 0xF8, 0x0D, 0xC5, 0xE5
    , 0x0394, 0x5F, 0x03A6, 0x0393, 0x039B, 0x03A9, 0x03A0, 0x03A8, 0x03A3, 0x0398, 0x039E, 0x1B, 0xC6, 0xE5, 0xDF, 0xA9
    , 0x20, 0x21, 0x22, 0x23, 0xA4, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2A, 0x2B, 0x2C, 0x2D, 0x2E, 0x2F
    , 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E, 0x3F
    , 0xA1, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4A, 0x4B, 0x4C, 0x4D, 0x4E, 0x4F
    , 0X50, 0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5A, 0xC4, 0xD6, 0xD1, 0xDC, 0xA7
    , 0xBF, 0x61, 0x62, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6A, 0x6B, 0x6C, 0x6D, 0x6E, 0x6F
    , 0x70, 0x71, 0x72, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7A, 0xE4, 0xF6, 0xF1, 0xFC, 0xE0}

local Charmapctl = {[10] = 0x0C, [20] = 0x5E, [40] = 0x7B, [41] = 0x7D, [47] = 0x5C, [60] = 0x5B, [61] = 0x7E
    , [62] = 0x5D, [64] = 0x7C, [101] = 0xA4}
--[[
函数名：gsm7bitdecode
功能  ：7位编码, 在PDU模式中，当使用7位编码时，最多可发160个字符
参数  ：data
longsms
返回值：
]]
local function gsm7bitdecode(data, longsms)
    local ucsdata, lpcnt, tmpdata, resdata, nbyte, nleft, ucslen, olddat = "", #data / 2, 0, 0, 0, 0, 0

    if longsms then
        tmpdata = tonumber("0x" .. data:sub(1, 2))
        resdata = tmpdata >> 1
        if olddat == 27 then
            if Charmapctl[resdata] then --特殊字符
                olddat, resdata = resdata, Charmapctl[resdata]
                ucsdata = ucsdata:sub(1, -5)
            else
                olddat, resdata = resdata, Charmap[resdata]
            end
        else
            olddat, resdata = resdata, Charmap[resdata]
        end
        ucsdata = ucsdata .. string.format("%04X", resdata)
    else
        tmpdata = tonumber("0x" .. data:sub(1, 2))
        resdata = ((tmpdata<<nbyte)|nleft)&0x7f
        if olddat == 27 then
            if Charmapctl[resdata] then --特殊字符
                olddat, resdata = resdata, Charmapctl[resdata]
                ucsdata = ucsdata:sub(1, -5)
            else
                olddat, resdata = resdata, Charmap[resdata]
            end
        else
            olddat, resdata = resdata, Charmap[resdata]
        end
        ucsdata = ucsdata .. string.format("%04X", resdata)

        nleft = tmpdata >> (7 - nbyte)
        nbyte = nbyte + 1
        ucslen = ucslen + 1
    end

    for i = 2, lpcnt do
        tmpdata = tonumber("0x" .. data:sub((i - 1) * 2 + 1, i * 2))
        if tmpdata == nil then break end
        resdata = ((tmpdata<<nbyte)|nleft)&0x7f
        if olddat == 27 then
            if Charmapctl[resdata] then --特殊字符
                olddat, resdata = resdata, Charmapctl[resdata]
                ucsdata = ucsdata:sub(1, -5)
            else
                olddat, resdata = resdata, Charmap[resdata]
            end
        else
            olddat, resdata = resdata, Charmap[resdata]
        end
        ucsdata = ucsdata .. string.format("%04X", resdata)

        nleft = tmpdata >> (7 - nbyte)
        nbyte = nbyte + 1
        ucslen = ucslen + 1

        if nbyte == 7 then
            if olddat == 27 then
                if Charmapctl[nleft] then --特殊字符
                    olddat, nleft = nleft, Charmapctl[nleft]
                    ucsdata = ucsdata:sub(1, -5)
                else
                    olddat, nleft = nleft, Charmap[nleft]
                end
            else
                olddat, nleft = nleft, Charmap[nleft]
            end
            ucsdata = ucsdata .. string.format("%04X", nleft)
            nbyte, nleft = 0, 0
            ucslen = ucslen + 1
        end
    end

    return ucsdata, ucslen
end

function libT.ucs2_utf8(s)
    local temp = {}
    for i=1,#s,2 do
        local d1,d2 = s:byte(i),s:byte(i+1)
        if d1 == 0 and d2 <= 0x7f then  --不大于0x007F
            table.insert(temp,string.char(d2))
        elseif d1 < 0x07 then  --不大于0x07FF  00000aaa bbbbbbbb ==> 110aaabb 10bbbbbb
            table.insert(temp,string.char(0xc0+(d1<<2)+(d2>>6), 0x80+(d2&0x3f)))
        else    --aaaaaaaa bbbbbbbb ==> 1110aaaa 10aaaabb 10bbbbbb
            table.insert(temp,string.char(0xe0 + (d1>>4), 0x80 + ((d1&0x0f)<<2) + (d2>>6), 0x80 + (d2&0x3f)))
        end
    end
    return table.concat(temp)
end

--utf8转ucs2（copilot写的）
function libT.utf8_ucs2(s)
    local ucsdata = ""
    local i = 1
    while i <= #s do
        local c = string.byte(s, i)
        local resdata = 0
        local nbyte = 0
        if c < 128 then
            resdata = c
            nbyte = 1
        elseif c < 224 then
            resdata = (c - 192) * 64 + (string.byte(s, i + 1) - 128)
            nbyte = 2
        elseif c < 240 then
            resdata = (c - 224) * 4096 + (string.byte(s, i + 1) - 128) * 64 + (string.byte(s, i + 2) - 128)
            nbyte = 3
        elseif c < 248 then
            resdata = (c - 240) * 262144 + (string.byte(s, i + 1) - 128) * 4096 + (string.byte(s, i + 2) - 128) * 64 + (string.byte(s, i + 3) - 128)
            nbyte = 4
        end
        ucsdata = ucsdata .. string.format("%04X", resdata):fromHex()
        i = i + nbyte
    end
    return ucsdata
end

--[[
函数名：bcdnumtonum
功能  ：BCD编码格式字符串 转化为 号码ASCII字符串，仅支持数字和+，例如91688121364265f7 （表示第1个字节是0x91，第2个字节为0x68，......） -> "+8618126324567"
参数  ：
num：待转换字符串
返回值：转换后的字符串
]]
local function bcdnumtonum(num, sender_address_length_raw)
    local len, numfix, convnum = #num, "", ""
    if len % 2 ~= 0 then print("your bcdnum is err " .. num) return end
    if num:sub(1, 2) == "91" then numfix = "+" end
    if num:sub(1, 2):upper() == "D0" then
        --将convnum按gsm 7bit decode转换为字符串
        --长度为lenend
        convnum = gsm7bitdecode(num:sub(3), false)
        log.debug("pdu", "GSM-7 decoded, data: \""..convnum.."\"")
        convnum = convnum:fromHex()
        local decoded_number_in_utf8 = libT.ucs2_utf8(convnum)
        log.debug("pdu", "number in UTF-8: "..decoded_number_in_utf8)

        -- 取出decoded_number_in_utf8中的有效字符 = sender_address_length_raw * 4 / 7 向下取整
        decoded_number_in_utf8 = decoded_number_in_utf8:sub(1, math.floor(sender_address_length_raw * 4 / 7))

        return decoded_number_in_utf8
    end
    len, num = len - 2, num:sub(3, -1)
    for i = 1, (len - (len % 2)) / 2 do
        convnum = convnum .. num:sub(i * 2, i * 2) .. num:sub(i * 2 - 1, i * 2 - 1)
    end
    if convnum:sub(len, len) == "f" or convnum:sub(len, len) == "F" then
        convnum = convnum:sub(1, -2)
    end
    return numfix .. convnum
end

---解析PDU短信
--返回值：
--发送者号码
--短信内容
--接收时间
--是否为长短信
--如果为长短信，分了几包
--如果为长短信，当前是第几包
function libT.decodePDU(pdu,len)
    collectgarbage("collect")--防止内存不足
    local offset = 5
    local addlen, addnum, flag, dcs, tz, txtlen, fo, longsms
    pdu = pdu:sub((#pdu / 2 - len) * 2 + 1)--PDU数据，不包括短信息中心号码
    fo = tonumber("0x" .. pdu:sub(1, 1))--PDU短信首字节的高4位,第6位为数据报头标志位
    if fo & 0x4 ~= 0 then
        longsms = true
    end
    addlen = tonumber(string.format("%d", "0x" .. pdu:sub(3, 4)))--回复地址数字个数
    local sender_address_length_raw = addlen

    addlen = addlen % 2 == 0 and addlen + 2 or addlen + 3 --加上号码类型2位（5，6）or 加上号码类型2位（5，6）和1位F

    offset = offset + addlen

    addnum = pdu:sub(5, 5 + addlen - 1)
    local convnum = bcdnumtonum(addnum, sender_address_length_raw)

    flag = tonumber(string.format("%d", "0x" .. pdu:sub(offset, offset + 1)))--协议标识 (TP-PID)
    offset = offset + 2
    dcs = tonumber(string.format("%d", "0x" .. pdu:sub(offset, offset + 1)))--用户信息编码方式 Dcs=8，表示短信存放的格式为UCS2编码
    offset = offset + 2
    tz = pdu:sub(offset, offset + 13)--时区7个字节
    offset = offset + 14
    txtlen = tonumber(string.format("%d", "0x" .. pdu:sub(offset, offset + 1)))--短信文本长度
    offset = offset + 2
    local data = pdu:sub(offset, offset + txtlen * 2 - 1)--短信文本
    local total, idx
    if longsms then
        if tonumber("0x" .. data:sub(5, 6)) == 3 then
            total, idx = tonumber("0x" .. data:sub(9, 10)), tonumber("0x" .. data:sub(11, 12))
            data = data:sub(13, -1)--去掉报头6个字节
        elseif tonumber("0x" .. data:sub(5, 6)) == 4 then
            total, idx = tonumber("0x" .. data:sub(11, 12)), tonumber("0x" .. data:sub(13, 14))
            data = data:sub(15, -1)--去掉报头7个字节
        end
    end

    --log.info("TP-PID : ", flag, "dcs: ", dcs, "tz: ", tz, "data: ", data, "txtlen", txtlen)

    if dcs == 0x00 then --7bit encode
        local newlen
        data, newlen = gsm7bitdecode(data, longsms)
        if newlen > txtlen then
            data = data:sub(1, txtlen * 4)
        end
        --log.info("7bit to ucs2 data: ", data, "txtlen", txtlen, "newlen", newlen)
    elseif dcs == 0x04 then --8bit encode
        data, txtlen = gsm8bitdecode(data)
        --log.info("8bit to ucs2 data: ", data, "txtlen", txtlen)
    end
    data = data:fromHex()--还是要转回bin数据的

    local t = ""
    for i = 1, 6 do
        t = t .. tz:sub(i * 2, i * 2) .. tz:sub(i * 2 - 1, i * 2 - 1)

        if i <= 3 then
            t = i < 3 and (t .. "/") or (t .. ",")
        elseif i < 6 then
            t = t .. ":"
        end
    end

    local timezone = tz:sub(13,14)
    timezone = tonumber(timezone, 16)
    local tzNegative = (timezone & 0x08) == 0x08
    timezone = timezone & 0xF7 -- 按位与 1111 0111
    timezone = tonumber(string.format("%x", timezone):sub(2,2) .. string.format("%x", timezone):sub(1,1)) * 15 / 60

    if tzNegative then
        timezone = -timezone
    end
    t = t..string.format("%+03d",timezone)

    return convnum, data, t,longsms, total, idx
end

---生成PDU短信编码
--仅支持单条短信，传入数据为utf8编码
--返回值为pdu编码与长度
function libT.encodePDU(num,data)
    data = libT.utf8_ucs2(data):toHex()
    local numlen, datalen, pducnt, pdu, pdulen, udhi = string.format("%02X", #num), #data / 2, 1, "", "", ""
    if datalen > 140 then--短信内容太长啦
        data = data:sub(1, 140 * 2)
    end
    datalen = string.format("%02X", datalen)
    pdu = "001110" .. numlen .. numtobcdnum(num) .. "000800" .. datalen .. data
    return pdu, #pdu // 2 - 1
end

return libT
