#!/usr/bin/env python3
# 从 code/web_html.cpp 抽取内嵌网页，填充示例数据，生成可本地浏览器打开的 preview/index.html。
# 用法：编辑 code/web_html.cpp 后运行  python preview/build_preview.py  再刷新浏览器。
# 注意：这是“看样式”的预览；后端动态数据由文件末尾的 fetch 桩提供示例值(仅预览，不影响固件)。
import io, os, re, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, 'code', 'web_html.cpp')
OUT = os.path.join(ROOT, 'preview', 'index.html')

src = io.open(SRC, encoding='utf-8').read()

# 抽取 htmlPage 原始字面量：第一个 R"rawliteral( ... 到最后的 )rawliteral";
start_tok = 'R"rawliteral('
start = src.find(start_tok)
end = src.rfind(')rawliteral";')
if start < 0 or end < 0:
    sys.exit('找不到 htmlPage 原始字面量')
html = src[start + len(start_tok):end]

# 账号面板里有宏拼接：)rawliteral" DEFAULT_WEB_USER " / " DEFAULT_WEB_PASS R"rawliteral(
html = re.sub(r'\)rawliteral"\s*DEFAULT_WEB_USER\s*"\s*/\s*"\s*DEFAULT_WEB_PASS\s*R"rawliteral\(',
              'admin / admin123', html)


CHANNEL_TPL = (
    '<div class="push-channel{EN}" id="channel{i}">'
    '<div class="push-channel-header">'
    '<input type="checkbox" name="push{i}en" id="push{i}en" onchange="toggleChannel({i})"{CHK}>'
    '<label for="push{i}en" class="label-inline">启用推送通道 {i1}</label></div>'
    '<div class="push-channel-body">'
    '<div class="form-group"><label>通道名称</label>'
    '<input type="text" name="push{i}name" value="{NAME}" placeholder="自定义名称"></div>'
    '<div class="form-group"><label>推送方式</label>'
    '<select name="push{i}type" id="push{i}type" onchange="updateTypeHint({i})">{OPTS}</select>'
    '<div class="push-type-hint" id="hint{i}"></div></div>'
    '<div class="form-group"><label>推送URL/Webhook</label>'
    '<input type="text" name="push{i}url" value="{URL}" placeholder="http://your-server.com/api 或 webhook地址"></div>'
    '<div id="extra{i}" style="display:none;">'
    '<div class="form-group"><label id="key1label{i}">参数1</label>'
    '<input type="text" name="push{i}key1" id="key1{i}" value=""></div>'
    '<div class="form-group" id="key2group{i}"><label id="key2label{i}">参数2</label>'
    '<input type="text" name="push{i}key2" id="key2{i}" value=""></div></div>'
    '<div id="custom{i}" style="display:none;">'
    '<div class="form-group"><label>请求体模板（使用 {sender} {message} {timestamp} 占位符）</label>'
    '<textarea name="push{i}body" rows="4" style="width:100%;font-family:monospace;"></textarea></div></div>'
    '<button type="button" class="btn btn-secondary btn-sm" onclick="testPush({i})">测试推送</button>'
    '<div class="result-box" id="pushTestResult{i}"></div>'
    '</div></div>'
)


def channel_block(i, enabled, name, url, ptype=1):
    opts_def = [
        (1, 'POST JSON（通用格式）'), (2, 'Bark（iOS推送）'), (3, 'GET请求（参数在URL中）'),
        (4, '钉钉机器人'), (5, 'PushPlus'), (6, 'Server酱'), (7, '自定义模板'),
        (8, '飞书机器人'), (9, 'Gotify'), (10, 'Telegram Bot'),
    ]
    opts = ''.join(
        '<option value="%d"%s>%s</option>' % (v, ' selected' if v == ptype else '', t)
        for v, t in opts_def)
    # 用 {i1}/{OPTS}/{NAME}/{URL}/{EN}/{CHK} 这些不会与正文({sender} 等)冲突的标记做替换
    return (CHANNEL_TPL
            .replace('{i1}', str(i + 1))
            .replace('{OPTS}', opts)
            .replace('{NAME}', name)
            .replace('{URL}', url)
            .replace('{EN}', ' enabled' if enabled else '')
            .replace('{CHK}', ' checked' if enabled else '')
            .replace('{i}', str(i)))


channels = (
    channel_block(0, True, 'Bark 推送', 'https://api.day.app/yourkey', 2) +
    channel_block(1, False, '', '', 1) +
    channel_block(2, False, '', '', 1) +
    channel_block(3, False, '', '', 1) +
    channel_block(4, False, '', '', 1)
)

tokens = {
    '%IP%': '192.168.1.50', '%WIFI_SSID%': 'MyHomeWiFi', '%FREE_HEAP%': '210 KB',
    '%UPTIME%': '12:34:56', '%WEB_USER%': 'admin', '%WEB_PASS%': 'admin123',
    '%SMTP_SERVER%': 'smtp.qq.com', '%SMTP_PORT%': '465', '%SMTP_USER%': 'you@qq.com',
    '%SMTP_PASS%': '', '%SMTP_SEND_TO%': 'recv@example.com', '%ADMIN_PHONE%': '+447700900456',
    '%NUMBER_BLACK_LIST%': '', '%SMTP_CHECK%': '已配置', '%MODEM_CHECK%': '已就绪',
    '%FORWARD_RULES%': 'kw\t验证码,银行\temail\t1\nfrom\t^(10086|10010)\tdrop\t1',
    '%EMAIL_EN%': 'checked', '%PUSH_EN%': 'checked',
    '%PUSH_COUNT%': '1', '%INBOX_MAX%': '20', '%PUSH_CHANNELS%': channels,
    '%NTP%': 'ntp.aliyun.com', '%RB_CHECKED%': '', '%RB_HOUR%': '4',
    '%HB_CHECKED%': 'checked', '%HB_HOUR%': '9',
    '%DATA_CHECKED%': '', '%APN%': '', '%PHONE_NUMBER%': '', '%OPERATOR_PLMN%': '',
    '%NETMODE_OPTIONS%': '<option value="0" selected>自动（LTE+GSM）</option><option value="1">仅 LTE</option>',
    '%TZ_OPTIONS%': '<option value="480" selected>UTC+8 北京/香港</option><option value="0">UTC/GMT</option><option value="-480">UTC-8 洛杉矶</option>',
}
for k, v in tokens.items():
    html = html.replace(k, v)

# 预览专用 fetch 桩：让 /status /keepalive /log 等动态面板显示示例数据
stub = '''<script>
/* PREVIEW ONLY — 桩 fetch，使动态面板以示例数据渲染，不影响真实固件 */
(function(){
  var J=function(o){return Promise.resolve({json:function(){return Promise.resolve(o);},text:function(){return Promise.resolve(JSON.stringify(o));}});};
  window.fetch=function(u){u=String(u);
    if(u.indexOf('/wifiscan')>=0)return J([{ssid:'MyHomeWiFi',rssi:-48,enc:1},{ssid:'Office-5G',rssi:-67,enc:1},{ssid:'CMCC-Free',rssi:-72,enc:0}]);
    if(u.indexOf('/status')>=0)return J({version:'2.1.0',modemReady:true,wifiConnected:true,apMode:false,ssid:'MyHomeWiFi',rssi:-62,csq:18,rsrp:-82,rsrq:-11,sinr:14,pci:415,plmn:'23410',tac:'71F9',tz:480,operator:'giffgaff',imei:'860000000000000',iccid:'89440000000000000000',phone:'+447700900123',dataEnabled:false,apn:'',netMode:0,cellIp:'',inboxCount:3,ip:'192.168.1.50',freeHeap:215000,minFreeHeap:180000,maxAllocHeap:110000,uptime:45230,queueDepth:0,fwdQueueDepth:0,smsTotal:42,lastSmsEpoch:1750000000,resetReason:1,configValid:true,timeSynced:true});
    if(u.indexOf('/messages')>=0&&u.indexOf('box=sent')>=0)return J([{id:3,sent:1750000200,target:'10086',text:'CXLL',ok:true},{id:2,sent:1749990500,target:'+447700900456',text:'测试转发：设备运行正常',ok:true},{id:1,sent:1749980800,target:'*122#',text:'keepalive',ok:false}]);
    if(u.indexOf('/messages')>=0)return J([{id:42,recv:1750000000,sender:'10086',ts:'2026-06-25 00:01:02',text:'【中国移动】尊敬的客户，您的话费余额为 38.50 元。',fwd:true},{id:41,recv:1749990000,sender:'+447700900456',ts:'2026-06-24 21:15:00',text:'您的验证码是 482913，5分钟内有效，请勿泄露。',fwd:true},{id:40,recv:1749980000,sender:'giffgaff',ts:'2026-06-24 18:30:00',text:'Your balance is now active.',fwd:false}]);
    if(u.indexOf('/keepalive')>=0)return J({enabled:true,intervalDays:175,action:1,target:'',lastTime:1740000000,daysLeft:120,timeValid:true});
    if(u.indexOf('/log')>=0)return J({seq:4,lines:['[启动] 固件 2.0.0-opt 启动','wifi已连接','IP地址: 192.168.1.50','模组AT响应正常','网络已注册']});
    return J({success:true,message:'(预览模式：示例响应)'});
  };
})();
</script>'''
html = html.replace('</head>', stub + '\n</head>', 1)

io.open(OUT, 'w', encoding='utf-8', newline='').write(html)
print('已生成', OUT, '(%d 字节) — 用浏览器打开即可预览样式' % len(html))
