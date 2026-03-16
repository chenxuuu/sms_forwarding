#include <Arduino.h>
#include <WiFi.h>
#include <WiFiMulti.h>
#include <WiFiClientSecure.h>
#include <WebServer.h>
#include <Preferences.h>
#include <pdulib.h>
#define ENABLE_SMTP
#define ENABLE_DEBUG
#include <ReadyMail.h>
#include <HTTPClient.h>
#include <mbedtls/md.h>  // 用于钉钉签名的HMAC-SHA256
#include <base64.h>      // Base64编码

//wifi信息，需要你打开这个去改
#include "wifi_config.h"

//串口映射
#define TXD 3
#define RXD 4
#define MODEM_EN_PIN 5

// LED引脚定义（用于通过CI验证，给个假的）
#ifndef LED_BUILTIN
#define LED_BUILTIN 8
#endif

// 推送通道类型
enum PushType {
  PUSH_TYPE_NONE = 0,      // 未启用
  PUSH_TYPE_POST_JSON = 1, // POST JSON格式 {"sender":"xxx","message":"xxx","timestamp":"xxx"}
  PUSH_TYPE_BARK = 2,      // Bark格式 POST {"title":"xxx","body":"xxx"}
  PUSH_TYPE_GET = 3,       // GET请求，参数放URL中
  PUSH_TYPE_DINGTALK = 4,  // 钉钉机器人
  PUSH_TYPE_PUSHPLUS = 5,  // PushPlus
  PUSH_TYPE_SERVERCHAN = 6,// Server酱
  PUSH_TYPE_CUSTOM = 7,    // 自定义模板
  PUSH_TYPE_FEISHU = 8,    // 飞书机器人
  PUSH_TYPE_GOTIFY = 9,    // Gotify
  PUSH_TYPE_TELEGRAM = 10  // Telegram Bot
};

// 最大推送通道数
#define MAX_PUSH_CHANNELS 5

// 推送通道配置（通用设计，支持多种推送方式）
struct PushChannel {
  bool enabled;           // 是否启用
  PushType type;          // 推送类型
  String name;            // 通道名称（用于显示）
  String url;             // 推送URL（webhook地址）
  String key1;            // 额外参数1（如：钉钉secret、pushplus token等）
  String key2;            // 额外参数2（备用）
  String customBody;      // 自定义请求体模板（使用 {sender} {message} {timestamp} 占位符）
};

// 配置参数结构体
struct Config {
  String smtpServer;
  int smtpPort;
  String smtpUser;
  String smtpPass;
  String smtpSendTo;
  String adminPhone;
  PushChannel pushChannels[MAX_PUSH_CHANNELS];  // 多推送通道
  String webUser;      // Web管理账号
  String webPass;      // Web管理密码
};

// 默认Web管理账号密码
#define DEFAULT_WEB_USER "admin"
#define DEFAULT_WEB_PASS "admin123"

Config config;
Preferences preferences;
WiFiMulti WiFiMulti;
PDU pdu = PDU(4096);
WiFiClientSecure ssl_client;
SMTPClient smtp(ssl_client);
WebServer server(80);

bool configValid = false;  // 配置是否有效
bool timeSynced = false;   // NTP时间是否已同步
bool networkRegistered = false;  // 网络是否已注册
unsigned long lastPrintTime = 0;  // 上次打印IP的时间
unsigned long lastCeregCheck = 0; // 上次检查网络注册的时间
#define CEREG_CHECK_INTERVAL 5000 // 网络注册检查间隔(毫秒)

#define SERIAL_BUFFER_SIZE 500
#define MAX_PDU_LENGTH 300
char serialBuf[SERIAL_BUFFER_SIZE];
int serialBufLen = 0;

// 长短信合并相关定义
#define MAX_CONCAT_PARTS 10       // 最大支持的长短信分段数
#define CONCAT_TIMEOUT_MS 30000   // 长短信等待超时时间(毫秒)
#define MAX_CONCAT_MESSAGES 5     // 最多同时缓存的长短信组数

// 长短信分段结构
struct SmsPart {
  bool valid;           // 该分段是否有效
  String text;          // 分段内容
};

// 长短信缓存结构
struct ConcatSms {
  bool inUse;                           // 是否正在使用
  int refNumber;                        // 参考号
  String sender;                        // 发送者
  String timestamp;                     // 时间戳（使用第一个收到的分段的时间戳）
  int totalParts;                       // 总分段数
  int receivedParts;                    // 已收到的分段数
  unsigned long firstPartTime;          // 收到第一个分段的时间
  SmsPart parts[MAX_CONCAT_PARTS];      // 各分段内容
};

ConcatSms concatBuffer[MAX_CONCAT_MESSAGES];  // 长短信缓存

// 保存配置到NVS
void saveConfig() {
  preferences.begin("sms_config", false);
  preferences.putString("smtpServer", config.smtpServer);
  preferences.putInt("smtpPort", config.smtpPort);
  preferences.putString("smtpUser", config.smtpUser);
  preferences.putString("smtpPass", config.smtpPass);
  preferences.putString("smtpSendTo", config.smtpSendTo);
  preferences.putString("adminPhone", config.adminPhone);
  preferences.putString("webUser", config.webUser);
  preferences.putString("webPass", config.webPass);
  
  // 保存推送通道配置
  for (int i = 0; i < MAX_PUSH_CHANNELS; i++) {
    String prefix = "push" + String(i);
    preferences.putBool((prefix + "en").c_str(), config.pushChannels[i].enabled);
    preferences.putUChar((prefix + "type").c_str(), (uint8_t)config.pushChannels[i].type);
    preferences.putString((prefix + "url").c_str(), config.pushChannels[i].url);
    preferences.putString((prefix + "name").c_str(), config.pushChannels[i].name);
    preferences.putString((prefix + "k1").c_str(), config.pushChannels[i].key1);
    preferences.putString((prefix + "k2").c_str(), config.pushChannels[i].key2);
    preferences.putString((prefix + "body").c_str(), config.pushChannels[i].customBody);
  }
  
  preferences.end();
  Serial.println("配置已保存");
}

// 从NVS加载配置
void loadConfig() {
  preferences.begin("sms_config", true);
  config.smtpServer = preferences.getString("smtpServer", "");
  config.smtpPort = preferences.getInt("smtpPort", 465);
  config.smtpUser = preferences.getString("smtpUser", "");
  config.smtpPass = preferences.getString("smtpPass", "");
  config.smtpSendTo = preferences.getString("smtpSendTo", "");
  config.adminPhone = preferences.getString("adminPhone", "");
  config.webUser = preferences.getString("webUser", DEFAULT_WEB_USER);
  config.webPass = preferences.getString("webPass", DEFAULT_WEB_PASS);
  
  // 加载推送通道配置
  for (int i = 0; i < MAX_PUSH_CHANNELS; i++) {
    String prefix = "push" + String(i);
    config.pushChannels[i].enabled = preferences.getBool((prefix + "en").c_str(), false);
    config.pushChannels[i].type = (PushType)preferences.getUChar((prefix + "type").c_str(), PUSH_TYPE_POST_JSON);
    config.pushChannels[i].url = preferences.getString((prefix + "url").c_str(), "");
    config.pushChannels[i].name = preferences.getString((prefix + "name").c_str(), "通道" + String(i + 1));
    config.pushChannels[i].key1 = preferences.getString((prefix + "k1").c_str(), "");
    config.pushChannels[i].key2 = preferences.getString((prefix + "k2").c_str(), "");
    config.pushChannels[i].customBody = preferences.getString((prefix + "body").c_str(), "");
  }
  
  // 兼容旧配置：如果有旧的httpUrl配置，迁移到第一个通道
  String oldHttpUrl = preferences.getString("httpUrl", "");
  if (oldHttpUrl.length() > 0 && !config.pushChannels[0].enabled) {
    config.pushChannels[0].enabled = true;
    config.pushChannels[0].url = oldHttpUrl;
    config.pushChannels[0].type = preferences.getUChar("barkMode", 0) != 0 ? PUSH_TYPE_BARK : PUSH_TYPE_POST_JSON;
    config.pushChannels[0].name = "迁移通道";
    Serial.println("已迁移旧HTTP配置到推送通道1");
  }
  
  preferences.end();
  Serial.println("配置已加载");
}

// 检查推送通道是否有效配置
bool isPushChannelValid(const PushChannel& ch) {
  if (!ch.enabled) return false;
  
  switch (ch.type) {
    case PUSH_TYPE_POST_JSON:
    case PUSH_TYPE_BARK:
    case PUSH_TYPE_GET:
    case PUSH_TYPE_DINGTALK:
    case PUSH_TYPE_FEISHU:
    case PUSH_TYPE_CUSTOM:
      return ch.url.length() > 0;
    case PUSH_TYPE_PUSHPLUS:
    case PUSH_TYPE_SERVERCHAN:
      return ch.key1.length() > 0;  // 这两个主要靠key1（token/sendkey）
    case PUSH_TYPE_GOTIFY:
      return ch.url.length() > 0 && ch.key1.length() > 0;  // 需要URL和Token
    case PUSH_TYPE_TELEGRAM:
      return ch.key1.length() > 0 && ch.key2.length() > 0; // 需要Chat ID和Token
    default:
      return false;
  }
}

// 检查配置是否有效（至少配置了邮件或任一推送通道）
bool isConfigValid() {
  bool emailValid = config.smtpServer.length() > 0 && 
                    config.smtpUser.length() > 0 && 
                    config.smtpPass.length() > 0 && 
                    config.smtpSendTo.length() > 0;
  
  bool pushValid = false;
  for (int i = 0; i < MAX_PUSH_CHANNELS; i++) {
    if (isPushChannelValid(config.pushChannels[i])) {
      pushValid = true;
      break;
    }
  }
  
  return emailValid || pushValid;
}

// 获取当前设备URL
String getDeviceUrl() {
  return "http://" + WiFi.localIP().toString() + "/";
}

// HTML配置页面
const char* htmlPage = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>短信转发配置</title>
  <style>
    body { font-family: Arial, sans-serif; margin: 20px; background: #f5f5f5; }
    .container { max-width: 600px; margin: 0 auto; background: white; padding: 20px; border-radius: 10px; box-shadow: 0 2px 10px rgba(0,0,0,0.1); }
    h1 { color: #333; text-align: center; }
    .form-group { margin-bottom: 15px; }
    label { display: block; margin-bottom: 5px; font-weight: bold; color: #555; }
    input[type="text"], input[type="password"], input[type="number"], textarea, select { width: 100%; padding: 10px; border: 1px solid #ddd; border-radius: 5px; box-sizing: border-box; }
    textarea { resize: vertical; min-height: 80px; }
    button { width: 100%; padding: 12px; background: #4CAF50; color: white; border: none; border-radius: 5px; cursor: pointer; font-size: 16px; margin-top: 10px; }
    button:hover { background: #45a049; }
    .label-inline { display:inline; font-weight:normal; margin-left: 5px; }
    .btn-send { background: #2196F3; }
    .btn-send:hover { background: #1976D2; }
    .section { border: 1px solid #ddd; padding: 15px; margin-bottom: 20px; border-radius: 5px; }
    .section-title { font-size: 18px; color: #333; margin-bottom: 10px; }
    .status { padding: 10px; background: #e7f3fe; border-left: 4px solid #2196F3; margin-bottom: 20px; }
    .warning { padding: 10px; background: #fff3cd; border-left: 4px solid #ffc107; margin-bottom: 20px; font-size: 12px; }
    .hint { font-size: 12px; color: #888; }
    .nav { display: flex; gap: 10px; margin-bottom: 20px; }
    .nav a { flex: 1; text-align: center; padding: 10px; background: #eee; border-radius: 5px; text-decoration: none; color: #333; }
    .nav a.active { background: #4CAF50; color: white; }
    .push-channel { border: 1px solid #e0e0e0; padding: 12px; margin-bottom: 15px; border-radius: 5px; background: #fafafa; }
    .push-channel-header { display: flex; align-items: center; margin-bottom: 10px; }
    .push-channel-header input[type="checkbox"] { width: auto; margin-right: 8px; }
    .push-channel-header label { margin: 0; font-weight: bold; }
    .push-channel-body { display: none; }
    .push-channel.enabled .push-channel-body { display: block; }
    .push-type-hint { font-size: 11px; color: #666; margin-top: 5px; padding: 8px; background: #f0f0f0; border-radius: 3px; }
  </style>
</head>
<body>
  <div class="container">
    <h1>📱 短信转发器</h1>
    <div class="nav">
      <a href="/" class="active">⚙️ 系统配置</a>
      <a href="/tools">🧰 工具箱</a>
    </div>
    <div class="status" id="status">设备IP: <strong>%IP%</strong></div>
    
    <form action="/save" method="POST">
      <div class="section">
        <div class="section-title">🔐 Web管理账号设置</div>
        <div class="warning">⚠️ 首次使用请修改默认密码！默认账号: )rawliteral" DEFAULT_WEB_USER "，默认密码: " DEFAULT_WEB_PASS R"rawliteral(
        </div>
        <div class="form-group">
          <label>管理账号</label>
          <input type="text" name="webUser" value="%WEB_USER%" placeholder="admin">
        </div>
        <div class="form-group">
          <label>管理密码</label>
          <input type="password" name="webPass" value="%WEB_PASS%" placeholder="请设置复杂密码">
        </div>
      </div>
      
      <div class="section">
        <div class="section-title">📧 邮件通知设置</div>
        <div class="form-group">
          <label>SMTP服务器</label>
          <input type="text" name="smtpServer" value="%SMTP_SERVER%" placeholder="smtp.qq.com">
        </div>
        <div class="form-group">
          <label>SMTP端口</label>
          <input type="number" name="smtpPort" value="%SMTP_PORT%" placeholder="465">
        </div>
        <div class="form-group">
          <label>邮箱账号</label>
          <input type="text" name="smtpUser" value="%SMTP_USER%" placeholder="your@qq.com">
        </div>
        <div class="form-group">
          <label>邮箱密码/授权码</label>
          <input type="password" name="smtpPass" value="%SMTP_PASS%" placeholder="授权码">
        </div>
        <div class="form-group">
          <label>接收邮件地址</label>
          <input type="text" name="smtpSendTo" value="%SMTP_SEND_TO%" placeholder="receiver@example.com">
        </div>
      </div>
      
      <div class="section">
        <div class="section-title">🔗 HTTP推送通道设置</div>
        <div class="hint" style="margin-bottom:15px;">可同时启用多个推送通道，每个通道独立配置。支持POST JSON、Bark、GET、钉钉、PushPlus、Server酱等多种方式。</div>
        
        %PUSH_CHANNELS%
      </div>
      
      <div class="section">
        <div class="section-title">👤 管理员设置</div>
        <div class="form-group">
          <label>管理员手机号</label>
          <input type="text" name="adminPhone" value="%ADMIN_PHONE%" placeholder="13800138000">
        </div>
      </div>
      
      <button type="submit">💾 保存配置</button>
    </form>
  </div>
  <script>
    function toggleChannel(idx) {
      var ch = document.getElementById('channel' + idx);
      var cb = document.getElementById('push' + idx + 'en');
      if (cb.checked) {
        ch.classList.add('enabled');
      } else {
        ch.classList.remove('enabled');
      }
    }
    function updateTypeHint(idx) {
      var sel = document.getElementById('push' + idx + 'type');
      var hint = document.getElementById('hint' + idx);
      var extraFields = document.getElementById('extra' + idx);
      var customFields = document.getElementById('custom' + idx);
      var type = parseInt(sel.value);
      
      // 隐藏所有额外字段
      extraFields.style.display = 'none';
      customFields.style.display = 'none';
      document.getElementById('key1label' + idx).innerText = '参数1';
      document.getElementById('key2label' + idx).innerText = '参数2';
      document.getElementById('key1' + idx).placeholder = '';
      document.getElementById('key2' + idx).placeholder = '';
      // key2 区域默认隐藏，只在需要用到 key2 的通知方式中显示
      document.getElementById('key2' + idx).closest('.form-group').style.display = 'none';
      
      if (type == 1) {
        hint.innerHTML = '<b>POST JSON格式：</b><br>{"sender":"发送者号码","message":"短信内容","timestamp":"时间戳"}';
      } else if (type == 2) {
        hint.innerHTML = '<b>Bark格式：</b><br>POST {"title":"发送者号码","body":"短信内容"}';
      } else if (type == 3) {
        hint.innerHTML = '<b>GET请求格式：</b><br>URL?sender=xxx&message=xxx&timestamp=xxx';
      } else if (type == 4) {
        hint.innerHTML = '<b>钉钉机器人：</b><br>填写Webhook地址，如需加签请填Secret';
        extraFields.style.display = 'block';
        document.getElementById('key1label' + idx).innerText = 'Secret（加签密钥，可选）';
        document.getElementById('key1' + idx).placeholder = 'SEC...';
      } else if (type == 5) {
        hint.innerHTML = '<b>PushPlus：</b><br>填写Token，URL留空使用默认';
        extraFields.style.display = 'block';
        document.getElementById('key1label' + idx).innerText = 'Token';
        document.getElementById('key1' + idx).placeholder = 'pushplus的token';
        // 显示 key2 区域
        document.getElementById('key2' + idx).closest('.form-group').style.display = 'block';
        document.getElementById('key2label' + idx).innerText = '发送渠道';
        document.getElementById('key2' + idx).placeholder = 'wechat(default), extension, app';
      } else if (type == 6) {
        hint.innerHTML = '<b>Server酱：</b><br>填写SendKey，URL留空使用默认';
        extraFields.style.display = 'block';
        document.getElementById('key1label' + idx).innerText = 'SendKey';
        document.getElementById('key1' + idx).placeholder = 'SCT...';
      } else if (type == 7) {
        hint.innerHTML = '<b>自定义模板：</b><br>在请求体模板中使用 {sender} {message} {timestamp} 作为占位符';
        customFields.style.display = 'block';
      } else if (type == 8) {
        hint.innerHTML = '<b>飞书机器人：</b><br>填写Webhook地址，如需签名验证请填Secret';
        extraFields.style.display = 'block';
        document.getElementById('key1label' + idx).innerText = 'Secret（签名密钥，可选）';
        document.getElementById('key1' + idx).placeholder = '飞书机器人的签名密钥';
      } else if (type == 9) {
        hint.innerHTML = '<b>Gotify：</b><br>填写服务器地址（如 http://gotify.example.com），Token填写应用Token';
        extraFields.style.display = 'block';
        document.getElementById('key1label' + idx).innerText = 'Token（应用Token）';
        document.getElementById('key1' + idx).placeholder = 'A...';
      } else if (type == 10) {
        hint.innerHTML = '<b>Telegram Bot：</b><br>填写Chat ID（参数1）和Bot Token（参数2），URL留空默认使用官方API';
        extraFields.style.display = 'block';
        document.getElementById('key1label' + idx).innerText = 'Chat ID';
        document.getElementById('key1' + idx).placeholder = '123456789';
        document.getElementById('key2label' + idx).innerText = 'Bot Token';
        document.getElementById('key2' + idx).placeholder = '12345678:ABC...';
      }
    }
    document.addEventListener('DOMContentLoaded', function() {
      for (var i = 0; i < 5; i++) {
        toggleChannel(i);
        updateTypeHint(i);
      }
    });
  </script>
</body>
</html>
)rawliteral";

// HTML工具箱页面
const char* htmlToolsPage = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>工具箱</title>
  <style>
    body { font-family: Arial, sans-serif; margin: 20px; background: #f5f5f5; }
    .container { max-width: 600px; margin: 0 auto; background: white; padding: 20px; border-radius: 10px; box-shadow: 0 2px 10px rgba(0,0,0,0.1); }
    h1 { color: #333; text-align: center; }
    .form-group { margin-bottom: 15px; }
    label { display: block; margin-bottom: 5px; font-weight: bold; color: #555; }
    input[type="text"], textarea { width: 100%; padding: 10px; border: 1px solid #ddd; border-radius: 5px; box-sizing: border-box; }
    textarea { resize: vertical; min-height: 100px; }
    button { width: 100%; padding: 12px; background: #2196F3; color: white; border: none; border-radius: 5px; cursor: pointer; font-size: 16px; margin-top: 10px; }
    button:hover { background: #1976D2; }
    .btn-query { background: #9C27B0; }
    .btn-query:hover { background: #7B1FA2; }
    .btn-ping { background: #FF9800; }
    .btn-ping:hover { background: #F57C00; }
    .btn-info { background: #607D8B; }
    .btn-info:hover { background: #455A64; }
    button:disabled { background: #ccc; cursor: not-allowed; }
    .section { border: 1px solid #ddd; padding: 15px; margin-bottom: 20px; border-radius: 5px; }
    .section-title { font-size: 18px; color: #333; margin-bottom: 10px; }
    .status { padding: 10px; background: #e7f3fe; border-left: 4px solid #2196F3; margin-bottom: 20px; }
    .nav { display: flex; gap: 10px; margin-bottom: 20px; }
    .nav a { flex: 1; text-align: center; padding: 10px; background: #eee; border-radius: 5px; text-decoration: none; color: #333; }
    .nav a.active { background: #2196F3; color: white; }
    .char-count { font-size: 12px; color: #888; text-align: right; }
    .hint { font-size: 12px; color: #888; margin-top: 5px; }
    .result-box { margin-top: 10px; padding: 10px; border-radius: 5px; display: none; }
    .result-success { background: #e8f5e9; border-left: 4px solid #4CAF50; color: #2e7d32; }
    .result-error { background: #ffebee; border-left: 4px solid #f44336; color: #c62828; }
    .result-loading { background: #fff3e0; border-left: 4px solid #FF9800; color: #e65100; }
    .result-info { background: #e3f2fd; border-left: 4px solid #2196F3; color: #1565c0; }
    .info-table { width: 100%; border-collapse: collapse; margin-top: 8px; }
    .info-table td { padding: 5px 8px; border-bottom: 1px solid #ddd; }
    .info-table td:first-child { font-weight: bold; width: 40%; color: #555; }
    .btn-group { display: flex; gap: 10px; flex-wrap: wrap; }
    .btn-group button { flex: 1; min-width: 120px; }
    #atLog { background: #333; color: #00ff00; font-family: 'Courier New', Courier, monospace; min-height: 150px; max-height: 300px; overflow-y: auto; padding: 10px; border-radius: 5px; margin-bottom: 10px; font-size: 13px; white-space: pre-wrap; word-break: break-all; }
    .at-input-group { display: flex; gap: 10px; }
    .at-input-group input { flex: 1; font-family: monospace; }
    .at-input-group button { width: auto; min-width: 80px; margin-top: 0; }
  </style>
</head>
<body>
  <div class="container">
    <h1>📱 短信转发器</h1>
    <div class="nav">
      <a href="/">⚙️ 系统配置</a>
      <a href="/tools" class="active">🧰 工具箱</a>
    </div>
    <div class="status" id="status">设备IP: <strong>%IP%</strong></div>
    
    <form action="/sendsms" method="POST">
      <div class="section">
        <div class="section-title">📤 发送短信</div>
        <div class="form-group">
          <label>目标号码</label>
          <input type="text" name="phone" placeholder="13800138000" required>
        </div>
        <div class="form-group">
          <label>短信内容</label>
          <textarea name="content" placeholder="请输入短信内容..." required oninput="updateCount(this)"></textarea>
          <div class="char-count">已输入 <span id="charCount">0</span> 字符</div>
        </div>
        <button type="submit">📨 发送短信</button>
      </div>
    </form>
    
    <div class="section">
      <div class="section-title">📊 模组信息查询</div>
      <div class="btn-group">
        <button type="button" class="btn-query" onclick="queryInfo('ati')">📋 固件信息</button>
        <button type="button" class="btn-query" onclick="queryInfo('signal')">📶 信号质量</button>
      </div>
      <div class="btn-group">
        <button type="button" class="btn-info" onclick="queryInfo('siminfo')">💳 SIM卡信息</button>
        <button type="button" class="btn-info" onclick="queryInfo('network')">🌍 网络状态</button>
      </div>
      <div class="btn-group">
        <button type="button" class="btn-info" onclick="queryInfo('wifi')" style="background:#00BCD4;">📡 WiFi状态</button>
      </div>
      <div class="result-box" id="queryResult"></div>
    </div>
    
    <div class="section">
      <div class="section-title">🌐 网络测试</div>
      <button type="button" class="btn-ping" id="pingBtn" onclick="confirmPing()">📡 点我消耗一点流量</button>
      <div class="hint">将向 8.8.8.8 进行 ping 操作，一次性消耗极少流量费用</div>
      <div class="result-box" id="pingResult"></div>
    </div>
    
    <div class="section">
      <div class="section-title">✈️ 模组控制</div>
      <div class="btn-group">
        <button type="button" id="flightBtn" onclick="toggleFlightMode()" style="background:#E91E63;">✈️ 切换飞行模式</button>
        <button type="button" onclick="queryFlightMode()" style="background:#9C27B0;">🔍 查询状态</button>
      </div>
      <div class="hint">飞行模式关闭时模组可正常收发短信，开启后将关闭射频无法使用移动网络</div>
      <div class="result-box" id="flightResult"></div>
    </div>

    <div class="section">
      <div class="section-title">💻 AT 指令调试</div>
      <div id="atLog">等待输入指令...</div>
      <div class="at-input-group">
        <input type="text" id="atCmd" placeholder="输入 AT 指令，如: AT+CSQ">
        <button type="button" onclick="sendAT()" id="atBtn">发送</button>
      </div>
      <div class="btn-group" style="margin-top:10px;">
        <button type="button" class="btn-info" onclick="clearATLog()">🧹 清空日志</button>
      </div>
      <div class="hint">直接向模组串口发送指令并接收响应，请谨慎操作</div>
    </div>
  </div>
  <script>
    function updateCount(el) {
      document.getElementById('charCount').textContent = el.value.length;
    }
    
    function queryInfo(type) {
      var result = document.getElementById('queryResult');
      result.className = 'result-box result-loading';
      result.style.display = 'block';
      result.textContent = '正在查询，请稍候...';
      
      fetch('/query?type=' + type)
        .then(response => response.json())
        .then(data => {
          if (data.success) {
            result.className = 'result-box result-info';
            result.innerHTML = data.message;
          } else {
            result.className = 'result-box result-error';
            result.innerHTML = '❌ 查询失败<br>' + data.message;
          }
        })
        .catch(error => {
          result.className = 'result-box result-error';
          result.textContent = '❌ 请求失败: ' + error;
        });
    }

    function confirmPing() {
      if (confirm("确定要执行 Ping 操作吗？\n\n这将消耗少量流量。")) {
        doPing();
      }
    }

    function doPing() {
      var btn = document.getElementById('pingBtn');
      var result = document.getElementById('pingResult');
      
      btn.disabled = true;
      btn.textContent = '⏳ 正在 Ping...';
      result.className = 'result-box result-loading';
      result.style.display = 'block';
      result.textContent = '正在执行 Ping 操作，请稍候（最长等待30秒）...';
      
      fetch('/ping', { method: 'POST' })
        .then(response => response.json())
        .then(data => {
          btn.disabled = false;
          btn.textContent = '📡 点我消耗一点流量';
          if (data.success) {
            result.className = 'result-box result-success';
            result.innerHTML = '✅ Ping 成功！<br>' + data.message;
          } else {
            result.className = 'result-box result-error';
            result.innerHTML = '❌ Ping 失败<br>' + data.message;
          }
        })
        .catch(error => {
          btn.disabled = false;
          btn.textContent = '📡 点我消耗一点流量';
          result.className = 'result-box result-error';
          result.textContent = '❌ 请求失败: ' + error;
        });
    }
    
    function queryFlightMode() {
      var result = document.getElementById('flightResult');
      result.className = 'result-box result-loading';
      result.style.display = 'block';
      result.textContent = '正在查询飞行模式状态...';
      
      fetch('/flight?action=query')
        .then(response => response.json())
        .then(data => {
          if (data.success) {
            result.className = 'result-box result-info';
            result.innerHTML = data.message;
          } else {
            result.className = 'result-box result-error';
            result.innerHTML = '❌ 查询失败: ' + data.message;
          }
        })
        .catch(error => {
          result.className = 'result-box result-error';
          result.textContent = '❌ 请求失败: ' + error;
        });
    }
    
    function toggleFlightMode() {
      if (!confirm('确定要切换飞行模式吗？\n\n开启飞行模式后模组将无法收发短信。')) return;
      
      var btn = document.getElementById('flightBtn');
      var result = document.getElementById('flightResult');
      btn.disabled = true;
      result.className = 'result-box result-loading';
      result.style.display = 'block';
      result.textContent = '正在切换飞行模式...';
      
      fetch('/flight?action=toggle')
        .then(response => response.json())
        .then(data => {
          btn.disabled = false;
          if (data.success) {
            result.className = 'result-box result-success';
            result.innerHTML = '✅ ' + data.message;
          } else {
            result.className = 'result-box result-error';
            result.innerHTML = '❌ 切换失败: ' + data.message;
          }
        })
        .catch(error => {
          btn.disabled = false;
          result.className = 'result-box result-error';
          result.textContent = '❌ 请求失败: ' + error;
        });
    }

    function addLog(msg, type = 'resp') {
      var log = document.getElementById('atLog');
      var div = document.createElement('div');
      var b = document.createElement('b');
      
      if (type === 'user') {
        b.style.color = '#fff';
        b.textContent = '> ';
      } else if (type === 'error') {
        b.style.color = '#f44336';
        b.textContent = '❌ ';
      } else {
        b.style.color = '#4CAF50';
        b.textContent = '[RESP] ';
      }
      
      div.appendChild(b);
      var textNode = document.createTextNode(msg);
      div.appendChild(textNode);
      
      log.appendChild(div);
      log.scrollTop = log.scrollHeight;
    }

    function sendAT() {
      var input = document.getElementById('atCmd');
      var cmd = input.value.trim();
      if (!cmd) return;
      
      var btn = document.getElementById('atBtn');
      btn.disabled = true;
      btn.textContent = '...';
      
      addLog(cmd, 'user');
      input.value = '';
      
      fetch('/at?cmd=' + encodeURIComponent(cmd))
        .then(response => response.json())
        .then(data => {
          if (data.success) {
            addLog(data.message);
          } else {
            addLog(data.message, 'error');
          }
        })
        .catch(error => {
          addLog('网络错误: ' + error, 'error');
        })
        .finally(() => {
          btn.disabled = false;
          btn.textContent = '发送';
        });
    }

    function clearATLog() {
      document.getElementById('atLog').innerHTML = '';
    }
    document.getElementById('atCmd').addEventListener('keydown', function(event) {
      if (event.key === 'Enter') {
        sendAT();
      }
    });
  </script>
</body>
</html>
)rawliteral";

// 检查HTTP Basic认证
bool checkAuth() {
  if (!server.authenticate(config.webUser.c_str(), config.webPass.c_str())) {
    server.requestAuthentication(BASIC_AUTH, "SMS Forwarding", "请输入管理员账号密码");
    return false;
  }
  return true;
}

// 处理配置页面请求
void handleRoot() {
  if (!checkAuth()) return;
  
  String html = String(htmlPage);
  html.replace("%IP%", WiFi.localIP().toString());
  html.replace("%WEB_USER%", config.webUser);
  html.replace("%WEB_PASS%", config.webPass);
  html.replace("%SMTP_SERVER%", config.smtpServer);
  html.replace("%SMTP_PORT%", String(config.smtpPort));
  html.replace("%SMTP_USER%", config.smtpUser);
  html.replace("%SMTP_PASS%", config.smtpPass);
  html.replace("%SMTP_SEND_TO%", config.smtpSendTo);
  html.replace("%ADMIN_PHONE%", config.adminPhone);
  
  // 生成推送通道HTML
  String channelsHtml = "";
  for (int i = 0; i < MAX_PUSH_CHANNELS; i++) {
    String idx = String(i);
    String enabledClass = config.pushChannels[i].enabled ? " enabled" : "";
    String checked = config.pushChannels[i].enabled ? " checked" : "";
    
    channelsHtml += "<div class=\"push-channel" + enabledClass + "\" id=\"channel" + idx + "\">";
    channelsHtml += "<div class=\"push-channel-header\">";
    channelsHtml += "<input type=\"checkbox\" name=\"push" + idx + "en\" id=\"push" + idx + "en\" onchange=\"toggleChannel(" + idx + ")\"" + checked + ">";
    channelsHtml += "<label for=\"push" + idx + "en\" class=\"label-inline\">启用推送通道 " + String(i + 1) + "</label>";
    channelsHtml += "</div>";
    channelsHtml += "<div class=\"push-channel-body\">";
    
    // 通道名称
    channelsHtml += "<div class=\"form-group\">";
    channelsHtml += "<label>通道名称</label>";
    channelsHtml += "<input type=\"text\" name=\"push" + idx + "name\" value=\"" + config.pushChannels[i].name + "\" placeholder=\"自定义名称\">";
    channelsHtml += "</div>";
    
    // 推送类型
    channelsHtml += "<div class=\"form-group\">";
    channelsHtml += "<label>推送方式</label>";
    channelsHtml += "<select name=\"push" + idx + "type\" id=\"push" + idx + "type\" onchange=\"updateTypeHint(" + idx + ")\">";
    channelsHtml += "<option value=\"1\"" + String(config.pushChannels[i].type == PUSH_TYPE_POST_JSON ? " selected" : "") + ">POST JSON（通用格式）</option>";
    channelsHtml += "<option value=\"2\"" + String(config.pushChannels[i].type == PUSH_TYPE_BARK ? " selected" : "") + ">Bark（iOS推送）</option>";
    channelsHtml += "<option value=\"3\"" + String(config.pushChannels[i].type == PUSH_TYPE_GET ? " selected" : "") + ">GET请求（参数在URL中）</option>";
    channelsHtml += "<option value=\"4\"" + String(config.pushChannels[i].type == PUSH_TYPE_DINGTALK ? " selected" : "") + ">钉钉机器人</option>";
    channelsHtml += "<option value=\"5\"" + String(config.pushChannels[i].type == PUSH_TYPE_PUSHPLUS ? " selected" : "") + ">PushPlus</option>";
    channelsHtml += "<option value=\"6\"" + String(config.pushChannels[i].type == PUSH_TYPE_SERVERCHAN ? " selected" : "") + ">Server酱</option>";
    channelsHtml += "<option value=\"7\"" + String(config.pushChannels[i].type == PUSH_TYPE_CUSTOM ? " selected" : "") + ">自定义模板</option>";
    channelsHtml += "<option value=\"8\"" + String(config.pushChannels[i].type == PUSH_TYPE_FEISHU ? " selected" : "") + ">飞书机器人</option>";
    channelsHtml += "<option value=\"9\"" + String(config.pushChannels[i].type == PUSH_TYPE_GOTIFY ? " selected" : "") + ">Gotify</option>";
    channelsHtml += "<option value=\"10\"" + String(config.pushChannels[i].type == PUSH_TYPE_TELEGRAM ? " selected" : "") + ">Telegram Bot</option>";
    channelsHtml += "</select>";
    channelsHtml += "<div class=\"push-type-hint\" id=\"hint" + idx + "\"></div>";
    channelsHtml += "</div>";
    
    // URL
    channelsHtml += "<div class=\"form-group\">";
    channelsHtml += "<label>推送URL/Webhook</label>";
    channelsHtml += "<input type=\"text\" name=\"push" + idx + "url\" value=\"" + config.pushChannels[i].url + "\" placeholder=\"http://your-server.com/api 或 webhook地址\">";
    channelsHtml += "</div>";
    
    // 额外参数区域（钉钉/PushPlus/Server酱等需要）
    channelsHtml += "<div id=\"extra" + idx + "\" style=\"display:none;\">";
    channelsHtml += "<div class=\"form-group\">";
    channelsHtml += "<label id=\"key1label" + idx + "\">参数1</label>";
    channelsHtml += "<input type=\"text\" name=\"push" + idx + "key1\" id=\"key1" + idx + "\" value=\"" + config.pushChannels[i].key1 + "\">";
    channelsHtml += "</div>";
    channelsHtml += "<div class=\"form-group\" id=\"key2group" + idx + "\">";
    channelsHtml += "<label id=\"key2label" + idx + "\">参数2</label>";
    channelsHtml += "<input type=\"text\" name=\"push" + idx + "key2\" id=\"key2" + idx + "\" value=\"" + config.pushChannels[i].key2 + "\">";
    channelsHtml += "</div>";
    channelsHtml += "</div>";
    
    // 自定义模板区域
    channelsHtml += "<div id=\"custom" + idx + "\" style=\"display:none;\">";
    channelsHtml += "<div class=\"form-group\">";
    channelsHtml += "<label>请求体模板（使用 {sender} {message} {timestamp} 占位符）</label>";
    channelsHtml += "<textarea name=\"push" + idx + "body\" rows=\"4\" style=\"width:100%;font-family:monospace;\">" + config.pushChannels[i].customBody + "</textarea>";
    channelsHtml += "</div>";
    channelsHtml += "</div>";
    
    channelsHtml += "</div></div>";
  }
  html.replace("%PUSH_CHANNELS%", channelsHtml);
  
  server.send(200, "text/html", html);
}

// 处理工具箱页面请求
void handleToolsPage() {
  if (!checkAuth()) return;
  
  String html = String(htmlToolsPage);
  html.replace("%IP%", WiFi.localIP().toString());
  server.send(200, "text/html", html);
}

// 发送AT命令并获取响应
String sendATCommand(const char* cmd, unsigned long timeout) {
  while (Serial1.available()) Serial1.read();
  Serial1.println(cmd);
  
  unsigned long start = millis();
  String resp = "";
  while (millis() - start < timeout) {
    while (Serial1.available()) {
      char c = Serial1.read();
      resp += c;
      if (resp.indexOf("OK") >= 0 || resp.indexOf("ERROR") >= 0) {
        delay(50);  // 等待剩余数据
        while (Serial1.available()) resp += (char)Serial1.read();
        return resp;
      }
    }
  }
  return resp;
}

// 处理飞行模式控制请求
void handleFlightMode() {
  if (!checkAuth()) return;
  
  String action = server.arg("action");
  String json = "{";
  bool success = false;
  String message = "";
  
  if (action == "query") {
    // 查询当前功能模式
    String resp = sendATCommand("AT+CFUN?", 2000);
    Serial.println("CFUN查询响应: " + resp);
    
    if (resp.indexOf("+CFUN:") >= 0) {
      success = true;
      int idx = resp.indexOf("+CFUN:");
      int mode = resp.substring(idx + 6).toInt();
      
      String modeStr;
      String statusIcon;
      if (mode == 0) {
        modeStr = "最小功能模式（关机）";
        statusIcon = "🔴";
      } else if (mode == 1) {
        modeStr = "全功能模式（正常）";
        statusIcon = "🟢";
      } else if (mode == 4) {
        modeStr = "飞行模式（射频关闭）";
        statusIcon = "✈️";
      } else {
        modeStr = "未知模式 (" + String(mode) + ")";
        statusIcon = "❓";
      }
      
      message = "<table class='info-table'>";
      message += "<tr><td>当前状态</td><td>" + statusIcon + " " + modeStr + "</td></tr>";
      message += "<tr><td>CFUN值</td><td>" + String(mode) + "</td></tr>";
      message += "</table>";
    } else {
      message = "查询失败";
    }
  }
  else if (action == "toggle") {
    // 先查询当前状态
    String resp = sendATCommand("AT+CFUN?", 2000);
    Serial.println("CFUN查询响应: " + resp);
    
    if (resp.indexOf("+CFUN:") >= 0) {
      int idx = resp.indexOf("+CFUN:");
      int currentMode = resp.substring(idx + 6).toInt();
      
      // 切换模式：1(正常) <-> 4(飞行模式)
      int newMode = (currentMode == 1) ? 4 : 1;
      String cmd = "AT+CFUN=" + String(newMode);
      
      Serial.println("切换飞行模式: " + cmd);
      String setResp = sendATCommand(cmd.c_str(), 5000);
      Serial.println("CFUN设置响应: " + setResp);
      
      if (setResp.indexOf("OK") >= 0) {
        success = true;
        if (newMode == 4) {
          message = "已开启飞行模式 ✈️<br>模组射频已关闭，无法收发短信";
        } else {
          message = "已关闭飞行模式 🟢<br>模组恢复正常工作";
        }
      } else {
        message = "切换失败: " + setResp;
      }
    } else {
      message = "无法获取当前状态";
    }
  }
  else if (action == "on") {
    // 强制开启飞行模式
    String resp = sendATCommand("AT+CFUN=4", 5000);
    if (resp.indexOf("OK") >= 0) {
      success = true;
      message = "已开启飞行模式 ✈️";
    } else {
      message = "开启失败: " + resp;
    }
  }
  else if (action == "off") {
    // 强制关闭飞行模式
    String resp = sendATCommand("AT+CFUN=1", 5000);
    if (resp.indexOf("OK") >= 0) {
      success = true;
      message = "已关闭飞行模式 🟢";
    } else {
      message = "关闭失败: " + resp;
    }
  }
  else {
    message = "未知操作";
  }
  
  json += "\"success\":" + String(success ? "true" : "false") + ",";
  json += "\"message\":\"" + message + "\"";
  json += "}";
  
  server.send(200, "application/json", json);
}

// 处理AT指令测试请求
void handleATCommand() {
  if (!checkAuth()) return;
  
  String cmd = server.arg("cmd");
  bool success = false;
  String message = "";
  
  if (cmd.length() == 0) {
    message = "错误：指令不能为空";
  } else {
    Serial.println("网页端发送AT指令: " + cmd);
    String resp = sendATCommand(cmd.c_str(), 5000);
    Serial.println("模组响应: " + resp);
    
    if (resp.length() > 0) {
      success = true;
      message = resp;
    } else {
      message = "超时或无响应";
    }
  }
  
  String json = "{";
  json += "\"success\":" + String(success ? "true" : "false") + ",";
  json += "\"message\":\"" + jsonEscape(message) + "\"";
  json += "}";
  
  server.send(200, "application/json", json);
}

// 处理模组信息查询请求
void handleQuery() {
  if (!checkAuth()) return;
  
  String type = server.arg("type");
  String json = "{";
  bool success = false;
  String message = "";
  
  if (type == "ati") {
    // 固件信息查询
    String resp = sendATCommand("ATI", 2000);
    Serial.println("ATI响应: " + resp);
    
    if (resp.indexOf("OK") >= 0) {
      success = true;
      // 解析ATI响应
      String manufacturer = "未知";
      String model = "未知";
      String version = "未知";
      
      // 按行解析
      int lineStart = 0;
      int lineNum = 0;
      for (int i = 0; i < resp.length(); i++) {
        if (resp.charAt(i) == '\n' || i == resp.length() - 1) {
          String line = resp.substring(lineStart, i);
          line.trim();
          if (line.length() > 0 && line != "ATI" && line != "OK") {
            lineNum++;
            if (lineNum == 1) manufacturer = line;
            else if (lineNum == 2) model = line;
            else if (lineNum == 3) version = line;
          }
          lineStart = i + 1;
        }
      }
      
      message = "<table class='info-table'>";
      message += "<tr><td>制造商</td><td>" + manufacturer + "</td></tr>";
      message += "<tr><td>模组型号</td><td>" + model + "</td></tr>";
      message += "<tr><td>固件版本</td><td>" + version + "</td></tr>";
      message += "</table>";
    } else {
      message = "查询失败";
    }
  }
  else if (type == "signal") {
    // 信号质量查询
    String resp = sendATCommand("AT+CESQ", 2000);
    Serial.println("CESQ响应: " + resp);
    
    if (resp.indexOf("+CESQ:") >= 0) {
      success = true;
      // 解析 +CESQ: <rxlev>,<ber>,<rscp>,<ecno>,<rsrq>,<rsrp>
      int idx = resp.indexOf("+CESQ:");
      String params = resp.substring(idx + 6);
      int endIdx = params.indexOf('\r');
      if (endIdx < 0) endIdx = params.indexOf('\n');
      if (endIdx > 0) params = params.substring(0, endIdx);
      params.trim();
      
      // 分割参数
      String values[6];
      int valIdx = 0;
      int startPos = 0;
      for (int i = 0; i <= params.length() && valIdx < 6; i++) {
        if (i == params.length() || params.charAt(i) == ',') {
          values[valIdx] = params.substring(startPos, i);
          values[valIdx].trim();
          valIdx++;
          startPos = i + 1;
        }
      }
      
      // RSRP转换为dBm (0-97映射到-140到-44 dBm, 99表示未知)
      int rsrp = values[5].toInt();
      String rsrpStr;
      if (rsrp == 99 || rsrp == 255) {
        rsrpStr = "未知";
      } else {
        int rsrpDbm = -140 + rsrp;
        rsrpStr = String(rsrpDbm) + " dBm";
        if (rsrpDbm >= -80) rsrpStr += " (信号极好)";
        else if (rsrpDbm >= -90) rsrpStr += " (信号良好)";
        else if (rsrpDbm >= -100) rsrpStr += " (信号一般)";
        else if (rsrpDbm >= -110) rsrpStr += " (信号较弱)";
        else rsrpStr += " (信号很差)";
      }
      
      // RSRQ转换 (0-34映射到-19.5到-3 dB)
      int rsrq = values[4].toInt();
      String rsrqStr;
      if (rsrq == 99 || rsrq == 255) {
        rsrqStr = "未知";
      } else {
        float rsrqDb = -19.5 + rsrq * 0.5;
        rsrqStr = String(rsrqDb, 1) + " dB";
      }
      
      message = "<table class='info-table'>";
      message += "<tr><td>信号强度 (RSRP)</td><td>" + rsrpStr + "</td></tr>";
      message += "<tr><td>信号质量 (RSRQ)</td><td>" + rsrqStr + "</td></tr>";
      message += "<tr><td>原始数据</td><td>" + params + "</td></tr>";
      message += "</table>";
    } else {
      message = "查询失败";
    }
  }
  else if (type == "siminfo") {
    // SIM卡信息查询
    success = true;
    message = "<table class='info-table'>";
    
    // 查询IMSI
    String resp = sendATCommand("AT+CIMI", 2000);
    String imsi = "未知";
    if (resp.indexOf("OK") >= 0) {
      int start = resp.indexOf('\n');
      if (start >= 0) {
        int end = resp.indexOf('\n', start + 1);
        if (end < 0) end = resp.indexOf('\r', start + 1);
        if (end > start) {
          imsi = resp.substring(start + 1, end);
          imsi.trim();
          if (imsi == "OK" || imsi.length() < 10) imsi = "未知";
        }
      }
    }
    message += "<tr><td>IMSI</td><td>" + imsi + "</td></tr>";
    
    // 查询ICCID
    resp = sendATCommand("AT+ICCID", 2000);
    String iccid = "未知";
    if (resp.indexOf("+ICCID:") >= 0) {
      int idx = resp.indexOf("+ICCID:");
      String tmp = resp.substring(idx + 7);
      int endIdx = tmp.indexOf('\r');
      if (endIdx < 0) endIdx = tmp.indexOf('\n');
      if (endIdx > 0) iccid = tmp.substring(0, endIdx);
      iccid.trim();
    }
    message += "<tr><td>ICCID</td><td>" + iccid + "</td></tr>";
    
    // 查询本机号码 (如果SIM卡支持)
    resp = sendATCommand("AT+CNUM", 2000);
    String phoneNum = "未存储或不支持";
    if (resp.indexOf("+CNUM:") >= 0) {
      int idx = resp.indexOf(",\"");
      if (idx >= 0) {
        int endIdx = resp.indexOf("\"", idx + 2);
        if (endIdx > idx) {
          phoneNum = resp.substring(idx + 2, endIdx);
        }
      }
    }
    message += "<tr><td>本机号码</td><td>" + phoneNum + "</td></tr>";
    
    message += "</table>";
  }
  else if (type == "network") {
    // 网络状态查询
    success = true;
    message = "<table class='info-table'>";
    
    // 查询网络注册状态
    String resp = sendATCommand("AT+CEREG?", 2000);
    String regStatus = "未知";
    if (resp.indexOf("+CEREG:") >= 0) {
      int idx = resp.indexOf("+CEREG:");
      String tmp = resp.substring(idx + 7);
      int commaIdx = tmp.indexOf(',');
      if (commaIdx >= 0) {
        String stat = tmp.substring(commaIdx + 1, commaIdx + 2);
        int s = stat.toInt();
        switch(s) {
          case 0: regStatus = "未注册，未搜索"; break;
          case 1: regStatus = "已注册，本地网络"; break;
          case 2: regStatus = "未注册，正在搜索"; break;
          case 3: regStatus = "注册被拒绝"; break;
          case 4: regStatus = "未知"; break;
          case 5: regStatus = "已注册，漫游"; break;
          default: regStatus = "状态码: " + stat;
        }
      }
    }
    message += "<tr><td>网络注册</td><td>" + regStatus + "</td></tr>";
    
    // 查询运营商
    resp = sendATCommand("AT+COPS?", 2000);
    String oper = "未知";
    if (resp.indexOf("+COPS:") >= 0) {
      int idx = resp.indexOf(",\"");
      if (idx >= 0) {
        int endIdx = resp.indexOf("\"", idx + 2);
        if (endIdx > idx) {
          oper = resp.substring(idx + 2, endIdx);
        }
      }
    }
    message += "<tr><td>运营商</td><td>" + oper + "</td></tr>";
    
    // 查询PDP上下文激活状态
    resp = sendATCommand("AT+CGACT?", 2000);
    String pdpStatus = "未激活";
    if (resp.indexOf("+CGACT: 1,1") >= 0) {
      pdpStatus = "已激活";
    } else if (resp.indexOf("+CGACT:") >= 0) {
      pdpStatus = "未激活";
    }
    message += "<tr><td>数据连接</td><td>" + pdpStatus + "</td></tr>";
    
    // 查询APN
    resp = sendATCommand("AT+CGDCONT?", 2000);
    String apn = "未知";
    if (resp.indexOf("+CGDCONT:") >= 0) {
      int idx = resp.indexOf(",\"");
      if (idx >= 0) {
        idx = resp.indexOf(",\"", idx + 2);  // 跳过PDP类型
        if (idx >= 0) {
          int endIdx = resp.indexOf("\"", idx + 2);
          if (endIdx > idx) {
            apn = resp.substring(idx + 2, endIdx);
            if (apn.length() == 0) apn = "(自动)";
          }
        }
      }
    }
    message += "<tr><td>APN</td><td>" + apn + "</td></tr>";
    
    message += "</table>";
  }
  else if (type == "wifi") {
    // WiFi状态查询
    success = true;
    message = "<table class='info-table'>";
    
    // WiFi连接状态
    String wifiStatus = WiFi.isConnected() ? "已连接" : "未连接";
    message += "<tr><td>连接状态</td><td>" + wifiStatus + "</td></tr>";
    
    // SSID
    String ssid = WiFi.SSID();
    if (ssid.length() == 0) ssid = "未知";
    message += "<tr><td>当前SSID</td><td>" + ssid + "</td></tr>";
    
    // 信号强度 RSSI
    int rssi = WiFi.RSSI();
    String rssiStr = String(rssi) + " dBm";
    if (rssi >= -50) rssiStr += " (信号极好)";
    else if (rssi >= -60) rssiStr += " (信号很好)";
    else if (rssi >= -70) rssiStr += " (信号良好)";
    else if (rssi >= -80) rssiStr += " (信号一般)";
    else if (rssi >= -90) rssiStr += " (信号较弱)";
    else rssiStr += " (信号很差)";
    message += "<tr><td>信号强度 (RSSI)</td><td>" + rssiStr + "</td></tr>";
    
    // IP地址
    message += "<tr><td>IP地址</td><td>" + WiFi.localIP().toString() + "</td></tr>";
    
    // 网关
    message += "<tr><td>网关</td><td>" + WiFi.gatewayIP().toString() + "</td></tr>";
    
    // 子网掩码
    message += "<tr><td>子网掩码</td><td>" + WiFi.subnetMask().toString() + "</td></tr>";
    
    // DNS
    message += "<tr><td>DNS服务器</td><td>" + WiFi.dnsIP().toString() + "</td></tr>";
    
    // MAC地址
    message += "<tr><td>MAC地址</td><td>" + WiFi.macAddress() + "</td></tr>";
    
    // BSSID (路由器MAC)
    message += "<tr><td>路由器BSSID</td><td>" + WiFi.BSSIDstr() + "</td></tr>";
    
    // 信道
    message += "<tr><td>WiFi信道</td><td>" + String(WiFi.channel()) + "</td></tr>";
    
    message += "</table>";
  }
  else {
    message = "未知的查询类型";
  }
  
  json += "\"success\":" + String(success ? "true" : "false") + ",";
  json += "\"message\":\"" + message + "\"";
  json += "}";
  
  server.send(200, "application/json", json);
}

// 前置声明
void sendEmailNotification(const char* subject, const char* body);
bool sendSMS(const char* phoneNumber, const char* message);

// 处理发送短信请求
void handleSendSms() {
  if (!checkAuth()) return;
  
  String phone = server.arg("phone");
  String content = server.arg("content");
  
  phone.trim();
  content.trim();
  
  bool success = false;
  String resultMsg = "";
  
  if (phone.length() == 0) {
    resultMsg = "错误：请输入目标号码";
  } else if (content.length() == 0) {
    resultMsg = "错误：请输入短信内容";
  } else {
    Serial.println("网页端发送短信请求");
    Serial.println("目标号码: " + phone);
    Serial.println("短信内容: " + content);
    
    success = sendSMS(phone.c_str(), content.c_str());
    resultMsg = success ? "短信发送成功！" : "短信发送失败，请检查模组状态";
  }
  
  String html = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta charset="UTF-8">
  <meta http-equiv="refresh" content="3;url=/sms">
  <title>发送结果</title>
  <style>
    body { font-family: Arial, sans-serif; text-align: center; padding-top: 100px; background: #f5f5f5; }
    .result { padding: 20px; border-radius: 10px; display: inline-block; }
    .success { background: #4CAF50; color: white; }
    .error { background: #f44336; color: white; }
  </style>
</head>
<body>
  <div class="result %CLASS%">
    <h2>%ICON% %MSG%</h2>
    <p>3秒后返回发送页面...</p>
  </div>
</body>
</html>
)rawliteral";
  
  html.replace("%CLASS%", success ? "success" : "error");
  html.replace("%ICON%", success ? "✅" : "❌");
  html.replace("%MSG%", resultMsg);
  
  server.send(200, "text/html", html);
}

// 处理Ping请求
void handlePing() {
  if (!checkAuth()) return;
  
  Serial.println("网页端发起Ping请求");
  
  // 清空串口缓冲区
  while (Serial1.available()) Serial1.read();
  
  // 激活PDP上下文（数据连接）
  Serial.println("激活数据连接(CGACT)...");
  String activateResp = sendATCommand("AT+CGACT=1,1", 10000);
  Serial.println("CGACT响应: " + activateResp);
  
  // 检查激活是否成功（OK或已激活的情况）
  bool networkActivated = (activateResp.indexOf("OK") >= 0);
  if (!networkActivated) {
    Serial.println("数据连接激活失败，尝试继续执行...");
  }
  
  // 清空串口缓冲区
  while (Serial1.available()) Serial1.read();
  delay(500);  // 等待网络稳定
  
  // 发送MPING命令，ping 8.8.8.8，超时30秒，ping 1次
  Serial1.println("AT+MPING=\"8.8.8.8\",30,1");
  
  // 等待响应
  unsigned long start = millis();
  String resp = "";
  bool gotOK = false;
  bool gotError = false;
  bool gotPingResult = false;
  String pingResultMsg = "";
  
  // 等待最多35秒（30秒超时 + 5秒余量）
  while (millis() - start < 35000) {
    while (Serial1.available()) {
      char c = Serial1.read();
      resp += c;
      Serial.print(c);  // 调试输出
      
      // 检查是否收到OK
      if (resp.indexOf("OK") >= 0 && !gotOK) {
        gotOK = true;
      }
      
      // 检查是否收到ERROR
      if (resp.indexOf("+CME ERROR") >= 0 || resp.indexOf("ERROR") >= 0) {
        gotError = true;
        pingResultMsg = "模组返回错误";
        break;
      }
      
      // 检查是否收到Ping结果URC
      // 成功格式: +MPING: 1,8.8.8.8,32,xxx,xxx
      // 失败格式: +MPING: 2 或其他
      int mpingIdx = resp.indexOf("+MPING:");
      if (mpingIdx >= 0) {
        // 找到换行符确定完整的一行
        int lineEnd = resp.indexOf('\n', mpingIdx);
        if (lineEnd >= 0) {
          String mpingLine = resp.substring(mpingIdx, lineEnd);
          mpingLine.trim();
          Serial.println("收到MPING结果: " + mpingLine);
          
          // 解析结果
          // +MPING: <result>[,<ip>,<packet_len>,<time>,<ttl>]
          int colonIdx = mpingLine.indexOf(':');
          if (colonIdx >= 0) {
            String params = mpingLine.substring(colonIdx + 1);
            params.trim();
            
            // 获取第一个参数（result）
            int commaIdx = params.indexOf(',');
            String resultStr;
            if (commaIdx >= 0) {
              resultStr = params.substring(0, commaIdx);
            } else {
              resultStr = params;
            }
            resultStr.trim();
            int result = resultStr.toInt();
            
            gotPingResult = true;
            
            // result=0或1都表示成功（不同模组可能返回不同值）
            // 如果有完整的响应参数（IP、时间等），也视为成功
            bool pingSuccess = (result == 0 || result == 1) || (params.indexOf(',') >= 0 && params.length() > 5);
            
            if (pingSuccess) {
              // 成功，解析详细信息
              // 格式: 0/1,"8.8.8.8",16,时间,TTL
              int idx1 = params.indexOf(',');
              if (idx1 >= 0) {
                String rest = params.substring(idx1 + 1);
                // 处理IP地址（可能带引号）
                String ip;
                int idx2;
                if (rest.startsWith("\"")) {
                  // 带引号的IP
                  int quoteEnd = rest.indexOf('\"', 1);
                  if (quoteEnd >= 0) {
                    ip = rest.substring(1, quoteEnd);
                    idx2 = rest.indexOf(',', quoteEnd);
                  } else {
                    idx2 = rest.indexOf(',');
                    ip = rest.substring(0, idx2);
                  }
                } else {
                  idx2 = rest.indexOf(',');
                  ip = rest.substring(0, idx2);
                }
                
                if (idx2 >= 0) {
                  rest = rest.substring(idx2 + 1);
                  int idx3 = rest.indexOf(',');  // packet_len后
                  if (idx3 >= 0) {
                    rest = rest.substring(idx3 + 1);
                    int idx4 = rest.indexOf(',');  // time后
                    String timeStr, ttlStr;
                    if (idx4 >= 0) {
                      timeStr = rest.substring(0, idx4);
                      ttlStr = rest.substring(idx4 + 1);
                    } else {
                      timeStr = rest;
                      ttlStr = "N/A";
                    }
                    timeStr.trim();
                    ttlStr.trim();
                    pingResultMsg = "目标: " + ip + ", 延迟: " + timeStr + "ms, TTL: " + ttlStr;
                  }
                }
              }
              if (pingResultMsg.length() == 0) {
                pingResultMsg = "Ping成功";
              }
            } else {
              // 失败
              pingResultMsg = "Ping超时或目标不可达 (错误码: " + String(result) + ")";
            }
            break;
          }
        }
      }
    }
    
    if (gotError || gotPingResult) break;
    delay(10);
  }
  
  Serial.println("\nPing操作完成");
  
  // 关闭数据连接以节省流量
  Serial.println("关闭PDP上下文(CGACT=0)...");
  String deactivateResp = sendATCommand("AT+CGACT=0,1", 5000);
  Serial.println("CGACT关闭响应: " + deactivateResp);
  
  // 构建JSON响应
  String json = "{";
  if (gotPingResult && pingResultMsg.indexOf("延迟") >= 0) {
    json += "\"success\":true,";
    json += "\"message\":\"" + pingResultMsg + "\"";
  } else if (gotError) {
    json += "\"success\":false,";
    json += "\"message\":\"" + pingResultMsg + "\"";
  } else if (gotPingResult) {
    json += "\"success\":false,";
    json += "\"message\":\"" + pingResultMsg + "\"";
  } else {
    json += "\"success\":false,";
    json += "\"message\":\"操作超时，未收到Ping结果\"";
  }
  json += "}";
  
  server.send(200, "application/json", json);
}

// 处理保存配置请求
void handleSave() {
  if (!checkAuth()) return;
  
  // 获取新的Web账号密码
  String newWebUser = server.arg("webUser");
  String newWebPass = server.arg("webPass");
  
  // 验证Web账号密码不能为空
  if (newWebUser.length() == 0) newWebUser = DEFAULT_WEB_USER;
  if (newWebPass.length() == 0) newWebPass = DEFAULT_WEB_PASS;
  
  config.webUser = newWebUser;
  config.webPass = newWebPass;
  config.smtpServer = server.arg("smtpServer");
  config.smtpPort = server.arg("smtpPort").toInt();
  if (config.smtpPort == 0) config.smtpPort = 465;
  config.smtpUser = server.arg("smtpUser");
  config.smtpPass = server.arg("smtpPass");
  config.smtpSendTo = server.arg("smtpSendTo");
  config.adminPhone = server.arg("adminPhone");
  
  // 保存推送通道配置
  for (int i = 0; i < MAX_PUSH_CHANNELS; i++) {
    String idx = String(i);
    config.pushChannels[i].enabled = server.arg("push" + idx + "en") == "on";
    config.pushChannels[i].type = (PushType)server.arg("push" + idx + "type").toInt();
    config.pushChannels[i].url = server.arg("push" + idx + "url");
    config.pushChannels[i].name = server.arg("push" + idx + "name");
    config.pushChannels[i].key1 = server.arg("push" + idx + "key1");
    config.pushChannels[i].key2 = server.arg("push" + idx + "key2");
    config.pushChannels[i].customBody = server.arg("push" + idx + "body");
    if (config.pushChannels[i].name.length() == 0) {
      config.pushChannels[i].name = "通道" + String(i + 1);
    }
  }
  
  saveConfig();
  configValid = isConfigValid();
  
  String html = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta charset="UTF-8">
  <meta http-equiv="refresh" content="3;url=/">
  <title>保存成功</title>
  <style>
    body { font-family: Arial, sans-serif; text-align: center; padding-top: 100px; background: #f5f5f5; }
    .success { background: #4CAF50; color: white; padding: 20px; border-radius: 10px; display: inline-block; }
  </style>
</head>
<body>
  <div class="success">
    <h2>✅ 配置保存成功！</h2>
    <p>3秒后返回配置页面...</p>
    <p>如果修改了账号密码，请使用新的账号密码登录</p>
  </div>
</body>
</html>
)rawliteral";
  server.send(200, "text/html", html);
  
  // 如果配置有效，发送启动通知
  if (configValid) {
    Serial.println("配置有效，发送启动通知...");
    String subject = "短信转发器配置已更新";
    String body = "设备配置已更新\n设备地址: " + getDeviceUrl();
    sendEmailNotification(subject.c_str(), body.c_str());
  }
}

// 发送邮件通知函数
void sendEmailNotification(const char* subject, const char* body) {
  if (config.smtpServer.length() == 0 || config.smtpUser.length() == 0 || 
      config.smtpPass.length() == 0 || config.smtpSendTo.length() == 0) {
    Serial.println("邮件配置不完整，跳过发送");
    return;
  }
  
  auto statusCallback = [](SMTPStatus status) {
    Serial.println(status.text);
  };
  smtp.connect(config.smtpServer.c_str(), config.smtpPort, statusCallback);
  if (smtp.isConnected()) {
    smtp.authenticate(config.smtpUser.c_str(), config.smtpPass.c_str(), readymail_auth_password);

    SMTPMessage msg;
    String from = "sms notify <"; from += config.smtpUser; from += ">";
    msg.headers.add(rfc822_from, from.c_str());
    String to = "your_email <"; to += config.smtpSendTo; to += ">";
    msg.headers.add(rfc822_to, to.c_str());
    msg.headers.add(rfc822_subject, subject);
    msg.text.body(body);
    msg.timestamp = time(nullptr);
    smtp.send(msg);
    Serial.println("邮件发送完成");
  } else {
    Serial.println("邮件服务器连接失败");
  }
}

// 发送短信（PDU模式）
bool sendSMS(const char* phoneNumber, const char* message) {
  Serial.println("准备发送短信...");
  Serial.print("目标号码: "); Serial.println(phoneNumber);
  Serial.print("短信内容: "); Serial.println(message);

  // 使用pdulib编码PDU
  pdu.setSCAnumber();  // 使用默认短信中心
  int pduLen = pdu.encodePDU(phoneNumber, message);
  
  if (pduLen < 0) {
    Serial.print("PDU编码失败，错误码: ");
    Serial.println(pduLen);
    return false;
  }
  
  Serial.print("PDU数据: "); Serial.println(pdu.getSMS());
  Serial.print("PDU长度: "); Serial.println(pduLen);
  
  // 发送AT+CMGS命令
  String cmgsCmd = "AT+CMGS=";
  cmgsCmd += pduLen;
  
  while (Serial1.available()) Serial1.read();
  Serial1.println(cmgsCmd);
  
  // 等待 > 提示符
  unsigned long start = millis();
  bool gotPrompt = false;
  while (millis() - start < 5000) {
    if (Serial1.available()) {
      char c = Serial1.read();
      Serial.print(c);
      if (c == '>') {
        gotPrompt = true;
        break;
      }
    }
  }
  
  if (!gotPrompt) {
    Serial.println("未收到>提示符");
    return false;
  }
  
  // 发送PDU数据
  Serial1.print(pdu.getSMS());
  Serial1.write(0x1A);  // Ctrl+Z 结束
  
  // 等待响应
  start = millis();
  String resp = "";
  while (millis() - start < 30000) {
    while (Serial1.available()) {
      char c = Serial1.read();
      resp += c;
      Serial.print(c);
      if (resp.indexOf("OK") >= 0) {
        Serial.println("\n短信发送成功");
        return true;
      }
      if (resp.indexOf("ERROR") >= 0) {
        Serial.println("\n短信发送失败");
        return false;
      }
    }
  }
  Serial.println("短信发送超时");
  return false;
}

// 新增“模组断电重启”函数
void modemPowerCycle() {
  pinMode(MODEM_EN_PIN, OUTPUT);

  Serial.println("EN 拉低：关闭模组");
  digitalWrite(MODEM_EN_PIN, LOW);
  delay(1200);  // 关机时间给够

  Serial.println("EN 拉高：开启模组");
  digitalWrite(MODEM_EN_PIN, HIGH);
  delay(6000);  // 等模组完全启动再发AT（关键）
}


// 重启模组
void resetModule() {
  Serial.println("正在硬重启模组（EN 断电重启）...");

  modemPowerCycle();

  // 清掉上电噪声/残留
  while (Serial1.available()) Serial1.read();

  // 硬重启后做 AT 握手确认（最多等 10 秒）
  bool ok = false;
  for (int i = 0; i < 10; i++) {
    if (sendATandWaitOK("AT", 1000)) {
      ok = true;
      break;
    }
    Serial.println("AT未响应，继续等模组启动...");
  }

  if (ok) Serial.println("模组AT恢复正常");
  else    Serial.println("模组AT仍未响应（检查EN接线/供电/波特率）");
}


// 检查发送者是否为管理员
bool isAdmin(const char* sender) {
  if (config.adminPhone.length() == 0) return false;
  
  // 去除可能的国际区号前缀进行比较
  String senderStr = String(sender);
  String adminStr = config.adminPhone;
  
  // 去除+86前缀
  if (senderStr.startsWith("+86")) {
    senderStr = senderStr.substring(3);
  }
  if (adminStr.startsWith("+86")) {
    adminStr = adminStr.substring(3);
  }
  
  return senderStr.equals(adminStr);
}

// 处理管理员命令
void processAdminCommand(const char* sender, const char* text) {
  String cmd = String(text);
  cmd.trim();
  
  Serial.println("处理管理员命令: " + cmd);
  
  // 处理 SMS:号码:内容 命令
  if (cmd.startsWith("SMS:")) {
    int firstColon = cmd.indexOf(':');
    int secondColon = cmd.indexOf(':', firstColon + 1);
    
    if (secondColon > firstColon + 1) {
      String targetPhone = cmd.substring(firstColon + 1, secondColon);
      String smsContent = cmd.substring(secondColon + 1);
      
      targetPhone.trim();
      smsContent.trim();
      
      Serial.println("目标号码: " + targetPhone);
      Serial.println("短信内容: " + smsContent);
      
      bool success = sendSMS(targetPhone.c_str(), smsContent.c_str());
      
      // 发送邮件通知结果
      String subject = success ? "短信发送成功" : "短信发送失败";
      String body = "管理员命令执行结果:\n";
      body += "命令: " + cmd + "\n";
      body += "目标号码: " + targetPhone + "\n";
      body += "短信内容: " + smsContent + "\n";
      body += "执行结果: " + String(success ? "成功" : "失败");
      
      sendEmailNotification(subject.c_str(), body.c_str());
    } else {
      Serial.println("SMS命令格式错误");
      sendEmailNotification("命令执行失败", "SMS命令格式错误，正确格式: SMS:号码:内容");
    }
  }
  // 处理 RESET 命令
  else if (cmd.equals("RESET")) {
    Serial.println("执行RESET命令");
    
    // 先发送邮件通知（因为重启后就发不了了）
    sendEmailNotification("重启命令已执行", "收到RESET命令，即将重启模组和ESP32...");
    
    // 重启模组
    resetModule();
    
    // 重启ESP32
    Serial.println("正在重启ESP32...");
    delay(1000);
    ESP.restart();
  }
  else {
    Serial.println("未知命令: " + cmd);
  }
}

// 初始化长短信缓存
void initConcatBuffer() {
  for (int i = 0; i < MAX_CONCAT_MESSAGES; i++) {
    concatBuffer[i].inUse = false;
    concatBuffer[i].receivedParts = 0;
    for (int j = 0; j < MAX_CONCAT_PARTS; j++) {
      concatBuffer[i].parts[j].valid = false;
      concatBuffer[i].parts[j].text = "";
    }
  }
}

// 查找或创建长短信缓存槽位
int findOrCreateConcatSlot(int refNumber, const char* sender, int totalParts) {
  // 先查找是否已存在
  for (int i = 0; i < MAX_CONCAT_MESSAGES; i++) {
    if (concatBuffer[i].inUse && 
        concatBuffer[i].refNumber == refNumber &&
        concatBuffer[i].sender.equals(sender)) {
      return i;
    }
  }
  
  // 查找空闲槽位
  for (int i = 0; i < MAX_CONCAT_MESSAGES; i++) {
    if (!concatBuffer[i].inUse) {
      concatBuffer[i].inUse = true;
      concatBuffer[i].refNumber = refNumber;
      concatBuffer[i].sender = String(sender);
      concatBuffer[i].totalParts = totalParts;
      concatBuffer[i].receivedParts = 0;
      concatBuffer[i].firstPartTime = millis();
      for (int j = 0; j < MAX_CONCAT_PARTS; j++) {
        concatBuffer[i].parts[j].valid = false;
        concatBuffer[i].parts[j].text = "";
      }
      return i;
    }
  }
  
  // 没有空闲槽位，查找最老的槽位覆盖
  int oldestSlot = 0;
  unsigned long oldestTime = concatBuffer[0].firstPartTime;
  for (int i = 1; i < MAX_CONCAT_MESSAGES; i++) {
    if (concatBuffer[i].firstPartTime < oldestTime) {
      oldestTime = concatBuffer[i].firstPartTime;
      oldestSlot = i;
    }
  }
  
  // 覆盖最老的槽位
  Serial.println("⚠️ 长短信缓存已满，覆盖最老的槽位");
  concatBuffer[oldestSlot].inUse = true;
  concatBuffer[oldestSlot].refNumber = refNumber;
  concatBuffer[oldestSlot].sender = String(sender);
  concatBuffer[oldestSlot].totalParts = totalParts;
  concatBuffer[oldestSlot].receivedParts = 0;
  concatBuffer[oldestSlot].firstPartTime = millis();
  for (int j = 0; j < MAX_CONCAT_PARTS; j++) {
    concatBuffer[oldestSlot].parts[j].valid = false;
    concatBuffer[oldestSlot].parts[j].text = "";
  }
  return oldestSlot;
}

// 合并长短信各分段
String assembleConcatSms(int slot) {
  String result = "";
  for (int i = 0; i < concatBuffer[slot].totalParts; i++) {
    if (concatBuffer[slot].parts[i].valid) {
      result += concatBuffer[slot].parts[i].text;
    } else {
      result += "[缺失分段" + String(i + 1) + "]";
    }
  }
  return result;
}

// 清空长短信槽位
void clearConcatSlot(int slot) {
  concatBuffer[slot].inUse = false;
  concatBuffer[slot].receivedParts = 0;
  concatBuffer[slot].sender = "";
  concatBuffer[slot].timestamp = "";
  for (int j = 0; j < MAX_CONCAT_PARTS; j++) {
    concatBuffer[slot].parts[j].valid = false;
    concatBuffer[slot].parts[j].text = "";
  }
}

// 前置声明
void processSmsContent(const char* sender, const char* text, const char* timestamp);

// 检查长短信超时并转发
void checkConcatTimeout() {
  unsigned long now = millis();
  for (int i = 0; i < MAX_CONCAT_MESSAGES; i++) {
    if (concatBuffer[i].inUse) {
      if (now - concatBuffer[i].firstPartTime >= CONCAT_TIMEOUT_MS) {
        Serial.println("⏰ 长短信超时，强制转发不完整消息");
        Serial.printf("  参考号: %d, 已收到: %d/%d\n", 
                      concatBuffer[i].refNumber,
                      concatBuffer[i].receivedParts,
                      concatBuffer[i].totalParts);
        
        // 合并已收到的分段
        String fullText = assembleConcatSms(i);
        
        // 处理短信内容
        processSmsContent(concatBuffer[i].sender.c_str(), 
                         fullText.c_str(), 
                         concatBuffer[i].timestamp.c_str());
        
        // 清空槽位
        clearConcatSlot(i);
      }
    }
  }
}

// 发送短信数据到服务器
// URL编码辅助函数
String urlEncode(const String& str) {
  String encoded = "";
  char c;
  char code0;
  char code1;
  for (unsigned int i = 0; i < str.length(); i++) {
    c = str.charAt(i);
    if (c == ' ') {
      encoded += '+';
    } else if (isalnum(c)) {
      encoded += c;
    } else {
      code1 = (c & 0xf) + '0';
      if ((c & 0xf) > 9) code1 = (c & 0xf) - 10 + 'A';
      c = (c >> 4) & 0xf;
      code0 = c + '0';
      if (c > 9) code0 = c - 10 + 'A';
      encoded += '%';
      encoded += code0;
      encoded += code1;
    }
  }
  return encoded;
}

// 钉钉签名函数（时间戳为UTC毫秒级）
String dingtalkSign(const String& secret, int64_t timestamp) {
  String stringToSign = String(timestamp) + "\n" + secret;
  
  uint8_t hmacResult[32];
  mbedtls_md_context_t ctx;
  mbedtls_md_init(&ctx);
  mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 1);
  mbedtls_md_hmac_starts(&ctx, (const unsigned char*)secret.c_str(), secret.length());
  mbedtls_md_hmac_update(&ctx, (const unsigned char*)stringToSign.c_str(), stringToSign.length());
  mbedtls_md_hmac_finish(&ctx, hmacResult);
  mbedtls_md_free(&ctx);
  
  String base64Encoded = base64::encode(hmacResult, 32);
  return urlEncode(base64Encoded);
}

// 获取当前UTC毫秒级时间戳（用于钉钉签名）
int64_t getUtcMillis() {
  struct timeval tv;
  if (gettimeofday(&tv, NULL) == 0) {
    return (int64_t)tv.tv_sec * 1000LL + tv.tv_usec / 1000;
  }
  // 如果获取失败，使用time()函数
  return (int64_t)time(nullptr) * 1000LL;
}

// JSON转义函数
String jsonEscape(const String& str) {
  String result = "";
  for (unsigned int i = 0; i < str.length(); i++) {
    char c = str.charAt(i);
    if (c == '"') result += "\\\"";
    else if (c == '\\') result += "\\\\";
    else if (c == '\n') result += "\\n";
    else if (c == '\r') result += "\\r";
    else if (c == '\t') result += "\\t";
    else result += c;
  }
  return result;
}

// 发送单个推送通道
void sendToChannel(const PushChannel& channel, const char* sender, const char* message, const char* timestamp) {
  if (!channel.enabled) return;
  
  // 对于某些推送方式，URL可以为空（使用默认URL）
  bool needUrl = (channel.type == PUSH_TYPE_POST_JSON || channel.type == PUSH_TYPE_BARK || 
                  channel.type == PUSH_TYPE_GET || channel.type == PUSH_TYPE_DINGTALK || 
                  channel.type == PUSH_TYPE_CUSTOM);
  if (needUrl && channel.url.length() == 0) return;
  
  HTTPClient http;
  String channelName = channel.name.length() > 0 ? channel.name : ("通道" + String(channel.type));
  Serial.println("发送到推送通道: " + channelName);
  
  int httpCode = 0;
  String senderEscaped = jsonEscape(String(sender));
  String messageEscaped = jsonEscape(String(message));
  String timestampEscaped = jsonEscape(String(timestamp));
  
  switch (channel.type) {
    case PUSH_TYPE_POST_JSON: {
      // 标准POST JSON格式
      http.begin(channel.url);
      http.addHeader("Content-Type", "application/json");
      String jsonData = "{";
      jsonData += "\"sender\":\"" + senderEscaped + "\",";
      jsonData += "\"message\":\"" + messageEscaped + "\",";
      jsonData += "\"timestamp\":\"" + timestampEscaped + "\"";
      jsonData += "}";
      Serial.println("POST JSON: " + jsonData);
      httpCode = http.POST(jsonData);
      break;
    }
    
    case PUSH_TYPE_BARK: {
      // Bark推送格式
      http.begin(channel.url);
      http.addHeader("Content-Type", "application/json");
      String jsonData = "{";
      jsonData += "\"title\":\"" + senderEscaped + "\",";
      jsonData += "\"body\":\"" + messageEscaped + "\"";
      jsonData += "}";
      Serial.println("BARK JSON: " + jsonData);
      httpCode = http.POST(jsonData);
      break;
    }
    
    case PUSH_TYPE_GET: {
      // GET请求，参数放URL里
      String getUrl = channel.url;
      if (getUrl.indexOf('?') == -1) {
        getUrl += "?";
      } else {
        getUrl += "&";
      }
      getUrl += "sender=" + urlEncode(String(sender));
      getUrl += "&message=" + urlEncode(String(message));
      getUrl += "&timestamp=" + urlEncode(String(timestamp));
      Serial.println("GET URL: " + getUrl);
      http.begin(getUrl);
      httpCode = http.GET();
      break;
    }
    
    case PUSH_TYPE_DINGTALK: {
      // 钉钉机器人
      String webhookUrl = channel.url;
      
      // 如果配置了secret，需要添加签名
      if (channel.key1.length() > 0) {
        // 获取UTC毫秒级时间戳（钉钉要求）
        int64_t ts = getUtcMillis();
        String sign = dingtalkSign(channel.key1, ts);
        if (webhookUrl.indexOf('?') == -1) {
          webhookUrl += "?";
        } else {
          webhookUrl += "&";
        }
        // 使用字符串拼接避免int64_t转换问题
        char tsBuf[21];
        snprintf(tsBuf, sizeof(tsBuf), "%lld", ts);
        webhookUrl += "timestamp=" + String(tsBuf) + "&sign=" + sign;
      }
      
      http.begin(webhookUrl);
      http.addHeader("Content-Type", "application/json");
      String jsonData = "{\"msgtype\":\"text\",\"text\":{\"content\":\"";
      jsonData += "📱短信通知\\n发送者: " + senderEscaped + "\\n内容: " + messageEscaped + "\\n时间: " + timestampEscaped;
      jsonData += "\"}}";
      Serial.println("钉钉: " + jsonData);
      httpCode = http.POST(jsonData);
      break;
    }

    case PUSH_TYPE_PUSHPLUS: {
      // PushPlus
      String pushUrl = channel.url.length() > 0 ? channel.url : "http://www.pushplus.plus/send";
      http.begin(pushUrl);
      http.addHeader("Content-Type", "application/json");
      // 发送渠道
      String channelValue = "wechat";
      if (channel.key2.length() > 0) {
          // 仅支持微信公众号（wechat）、浏览器插件（extension）和 PushPlus App（app）三种渠道
          if (channel.key2 == "wechat" || channel.key2 == "extension" || channel.key2 == "app") {
              channelValue = channel.key2;
          } else {
              Serial.println("Invalid PushPlus channel '" + channel.key2 + "'. Using default 'wechat'.");
          }
      }
      String jsonData = "{";
      jsonData += "\"token\":\"" + channel.key1 + "\",";
      jsonData += "\"title\":\"短信来自: " + senderEscaped + "\",";
      jsonData += "\"content\":\"<b>发送者:</b> " + senderEscaped + "<br><b>时间:</b> " + timestampEscaped + "<br><b>内容:</b><br>" + messageEscaped + "\",";
      jsonData += "\"channel\":\"" + channelValue + "\"";
      jsonData += "}";
      Serial.println("PushPlus: " + jsonData);
      httpCode = http.POST(jsonData);
      break;
    }

    case PUSH_TYPE_SERVERCHAN: {
      // Server酱
      String scUrl = channel.url.length() > 0 ? channel.url : ("https://sctapi.ftqq.com/" + channel.key1 + ".send");
      http.begin(scUrl);
      http.addHeader("Content-Type", "application/x-www-form-urlencoded");
      String postData = "title=" + urlEncode("短信来自: " + String(sender));
      postData += "&desp=" + urlEncode("**发送者:** " + String(sender) + "\n\n**时间:** " + String(timestamp) + "\n\n**内容:**\n\n" + String(message));
      Serial.println("Server酱: " + postData);
      httpCode = http.POST(postData);
      break;
    }
    
    case PUSH_TYPE_CUSTOM: {
      // 自定义模板
      if (channel.customBody.length() == 0) {
        Serial.println("自定义模板为空，跳过");
        return;
      }
      http.begin(channel.url);
      http.addHeader("Content-Type", "application/json");
      String body = channel.customBody;
      body.replace("{sender}", senderEscaped);
      body.replace("{message}", messageEscaped);
      body.replace("{timestamp}", timestampEscaped);
      Serial.println("自定义: " + body);
      httpCode = http.POST(body);
      break;
    }
    
    case PUSH_TYPE_FEISHU: {
      // 飞书机器人
      String webhookUrl = channel.url;
      String jsonData = "{";
      
      // 如果配置了secret，需要添加签名
      if (channel.key1.length() > 0) {
        // 飞书使用秒级时间戳
        int64_t ts = time(nullptr);
        // 飞书签名: base64(hmac-sha256(timestamp + "\n" + secret, secret))
        String stringToSign = String(ts) + "\n" + channel.key1;
        uint8_t hmacResult[32];
        mbedtls_md_context_t ctx;
        mbedtls_md_init(&ctx);
        mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 1);
        mbedtls_md_hmac_starts(&ctx, (const unsigned char*)channel.key1.c_str(), channel.key1.length());
        mbedtls_md_hmac_update(&ctx, (const unsigned char*)stringToSign.c_str(), stringToSign.length());
        mbedtls_md_hmac_finish(&ctx, hmacResult);
        mbedtls_md_free(&ctx);
        String sign = base64::encode(hmacResult, 32);
        
        jsonData += "\"timestamp\":\"" + String(ts) + "\",";
        jsonData += "\"sign\":\"" + sign + "\",";
      }
      
      // 飞书消息体
      jsonData += "\"msg_type\":\"text\",";
      jsonData += "\"content\":{\"text\":\"";
      jsonData += "📱短信通知\\n发送者: " + senderEscaped + "\\n内容: " + messageEscaped + "\\n时间: " + timestampEscaped;
      jsonData += "\"}}";
      
      http.begin(webhookUrl);
      http.addHeader("Content-Type", "application/json");
      Serial.println("飞书: " + jsonData);
      httpCode = http.POST(jsonData);
      break;
    }
    
    case PUSH_TYPE_GOTIFY: {
      // Gotify 推送
      String gotifyUrl = channel.url;
      // 确保URL以/结尾
      if (!gotifyUrl.endsWith("/")) gotifyUrl += "/";
      gotifyUrl += "message?token=" + channel.key1;
      
      http.begin(gotifyUrl);
      http.addHeader("Content-Type", "application/json");
      String jsonData = "{";
      jsonData += "\"title\":\"短信来自: " + senderEscaped + "\",";
      jsonData += "\"message\":\"" + messageEscaped + "\\n\\n时间: " + timestampEscaped + "\",";
      jsonData += "\"priority\":5";
      jsonData += "}";
      Serial.println("Gotify: " + jsonData);
      httpCode = http.POST(jsonData);
      break;
    }
    
    case PUSH_TYPE_TELEGRAM: {
      // Telegram Bot 推送
      // channel.key1 是 Chat ID, channel.key2 是 Bot Token
      String tgBaseUrl = channel.url.length() > 0 ? channel.url : "https://api.telegram.org";
      if (tgBaseUrl.endsWith("/")) tgBaseUrl.remove(tgBaseUrl.length() - 1);
      
      String tgUrl = tgBaseUrl + "/bot" + channel.key2 + "/sendMessage";
      http.begin(tgUrl);
      http.addHeader("Content-Type", "application/json");
      
      String jsonData = "{";
      jsonData += "\"chat_id\":\"" + channel.key1 + "\",";
      String text = "📱短信通知\n发送者: " + senderEscaped + "\n内容: " + messageEscaped + "\n时间: " + timestampEscaped;
      jsonData += "\"text\":\"" + text + "\"";
      jsonData += "}";
      
      Serial.println("Telegram: " + jsonData);
      httpCode = http.POST(jsonData);
      break;
    }
    
    default:
      Serial.println("未知推送类型");
      return;
  }
  
  if (httpCode > 0) {
    Serial.printf("[%s] 响应码: %d\n", channelName.c_str(), httpCode);
    if (httpCode == HTTP_CODE_OK || httpCode == HTTP_CODE_CREATED) {
      String response = http.getString();
      Serial.println("响应: " + response);
    }
  } else {
    Serial.printf("[%s] HTTP请求失败: %s\n", channelName.c_str(), http.errorToString(httpCode).c_str());
  }
  http.end();
}

// 发送短信到所有启用的推送通道
void sendSMSToServer(const char* sender, const char* message, const char* timestamp) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi未连接，跳过推送");
    return;
  }
  
  bool hasEnabledChannel = false;
  for (int i = 0; i < MAX_PUSH_CHANNELS; i++) {
    if (isPushChannelValid(config.pushChannels[i])) {
      hasEnabledChannel = true;
      break;
    }
  }
  
  if (!hasEnabledChannel) {
    Serial.println("没有启用的推送通道");
    return;
  }
  
  Serial.println("\n=== 开始多通道推送 ===");
  for (int i = 0; i < MAX_PUSH_CHANNELS; i++) {
    if (isPushChannelValid(config.pushChannels[i])) {
      sendToChannel(config.pushChannels[i], sender, message, timestamp);
      delay(100); // 短暂延迟避免请求过快
    }
  }
  Serial.println("=== 多通道推送完成 ===\n");
}

// 读取串口一行（含回车换行），返回行字符串，无新行时返回空
String readSerialLine(HardwareSerial& port) {
  static char lineBuf[SERIAL_BUFFER_SIZE];
  static int linePos = 0;

  while (port.available()) {
    char c = port.read();
    if (c == '\n') {
      lineBuf[linePos] = 0;
      String res = String(lineBuf);
      linePos = 0;
      return res;
    } else if (c != '\r') {  // 跳过\r
      if (linePos < SERIAL_BUFFER_SIZE - 1)
        lineBuf[linePos++] = c;
      else
        linePos = 0;  //超长报错保护，重头计
    }
  }
  return "";
}

// 检查字符串是否为有效的十六进制PDU数据
bool isHexString(const String& str) {
  if (str.length() == 0) return false;
  for (unsigned int i = 0; i < str.length(); i++) {
    char c = str.charAt(i);
    if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f'))) {
      return false;
    }
  }
  return true;
}

// 处理最终的短信内容（管理员命令检查和转发）
void processSmsContent(const char* sender, const char* text, const char* timestamp) {
  Serial.println("=== 处理短信内容 ===");
  Serial.println("发送者: " + String(sender));
  Serial.println("时间戳: " + String(timestamp));
  Serial.println("内容: " + String(text));
  Serial.println("====================");

  // 检查是否为管理员命令
  if (isAdmin(sender)) {
    Serial.println("收到管理员短信，检查命令...");
    String smsText = String(text);
    smsText.trim();
    
    // 检查是否为命令格式
    if (smsText.startsWith("SMS:") || smsText.equals("RESET")) {
      processAdminCommand(sender, text);
      // 命令已处理，不再发送普通通知邮件
      return;
    }
  }

  // 发送通知http（推送到所有启用的通道）
  sendSMSToServer(sender, text, timestamp);
  // 发送通知邮件
  String subject = ""; subject+="短信";subject+=sender;subject+=",";subject+=text;
  String body = ""; body+="来自：";body+=sender;body+="，时间：";body+=timestamp;body+="，内容：";body+=text;
  sendEmailNotification(subject.c_str(), body.c_str());
}

// 处理URC和PDU
void checkSerial1URC() {
  static enum { IDLE,
                WAIT_PDU } state = IDLE;

  String line = readSerialLine(Serial1);
  if (line.length() == 0) return;

  // 打印到调试串口
  Serial.println("Debug> " + line);

  if (state == IDLE) {
    // 检测到短信上报URC头
    if (line.startsWith("+CMT:")) {
      Serial.println("检测到+CMT，等待PDU数据...");
      state = WAIT_PDU;
    }
  } else if (state == WAIT_PDU) {
    // 跳过空行
    if (line.length() == 0) {
      return;
    }
    
    // 如果是十六进制字符串，认为是PDU数据
    if (isHexString(line)) {
      Serial.println("收到PDU数据: " + line);
      Serial.println("PDU长度: " + String(line.length()) + " 字符");
      
      // 解析PDU
      if (!pdu.decodePDU(line.c_str())) {
        Serial.println("❌ PDU解析失败！");
      } else {
        Serial.println("✓ PDU解析成功");
        Serial.println("=== 短信内容 ===");
        Serial.println("发送者: " + String(pdu.getSender()));
        Serial.println("时间戳: " + String(pdu.getTimeStamp()));
        Serial.println("内容: " + String(pdu.getText()));
        
        // 获取长短信信息
        int* concatInfo = pdu.getConcatInfo();
        int refNumber = concatInfo[0];
        int partNumber = concatInfo[1];
        int totalParts = concatInfo[2];
        
        Serial.printf("长短信信息: 参考号=%d, 当前=%d, 总计=%d\n", refNumber, partNumber, totalParts);
        Serial.println("===============");

        // 判断是否为长短信
        if (totalParts > 1 && partNumber > 0) {
          // 这是长短信的一部分
          Serial.printf("📧 收到长短信分段 %d/%d\n", partNumber, totalParts);
          
          // 查找或创建缓存槽位
          int slot = findOrCreateConcatSlot(refNumber, pdu.getSender(), totalParts);
          
          // 存储该分段（partNumber从1开始，数组从0开始）
          int partIndex = partNumber - 1;
          if (partIndex >= 0 && partIndex < MAX_CONCAT_PARTS) {
            if (!concatBuffer[slot].parts[partIndex].valid) {
              concatBuffer[slot].parts[partIndex].valid = true;
              concatBuffer[slot].parts[partIndex].text = String(pdu.getText());
              concatBuffer[slot].receivedParts++;
              
              // 如果是第一个收到的分段，保存时间戳
              if (concatBuffer[slot].receivedParts == 1) {
                concatBuffer[slot].timestamp = String(pdu.getTimeStamp());
              }
              
              Serial.printf("  已缓存分段 %d，当前已收到 %d/%d\n", 
                           partNumber, 
                           concatBuffer[slot].receivedParts, 
                           totalParts);
            } else {
              Serial.printf("  ⚠️ 分段 %d 已存在，跳过\n", partNumber);
            }
          }
          
          // 检查是否已收齐所有分段
          if (concatBuffer[slot].receivedParts >= totalParts) {
            Serial.println("✅ 长短信已收齐，开始合并转发");
            
            // 合并所有分段
            String fullText = assembleConcatSms(slot);
            
            // 处理完整短信
            processSmsContent(concatBuffer[slot].sender.c_str(), 
                             fullText.c_str(), 
                             concatBuffer[slot].timestamp.c_str());
            
            // 清空槽位
            clearConcatSlot(slot);
          }
        } else {
          // 普通短信，直接处理
          processSmsContent(pdu.getSender(), pdu.getText(), pdu.getTimeStamp());
        }
      }
      
      // 返回IDLE状态
      state = IDLE;
    } 
    // 如果是其他内容（OK、ERROR等），也返回IDLE
    else {
      Serial.println("收到非PDU数据，返回IDLE状态");
      state = IDLE;
    }
  }
}

void blink_short(unsigned long gap_time = 500) {
  digitalWrite(LED_BUILTIN, LOW);
  delay(50);
  digitalWrite(LED_BUILTIN, HIGH);
  delay(gap_time);
}

bool sendATandWaitOK(const char* cmd, unsigned long timeout) {
  while (Serial1.available()) Serial1.read();
  Serial1.println(cmd);
  unsigned long start = millis();
  String resp = "";
  while (millis() - start < timeout) {
    while (Serial1.available()) {
      char c = Serial1.read();
      resp += c;
      if (resp.indexOf("OK") >= 0) return true;
      if (resp.indexOf("ERROR") >= 0) return false;
    }
  }
  return false;
}

// 检测网络注册状态（LTE/4G）
// CEREG状态: 1=已注册本地, 5=已注册漫游
bool waitCEREG() {
  Serial1.println("AT+CEREG?");
  unsigned long start = millis();
  String resp = "";
  while (millis() - start < 2000) {
    while (Serial1.available()) {
      char c = Serial1.read();
      resp += c;
      // +CEREG: <n>,<stat> 其中stat=1或5表示已注册
      if (resp.indexOf("+CEREG:") >= 0) {
        // 检查是否已注册（状态1或5）
        if (resp.indexOf(",1") >= 0 || resp.indexOf(",5") >= 0) return true;
        if (resp.indexOf(",0") >= 0 || resp.indexOf(",2") >= 0 || 
            resp.indexOf(",3") >= 0 || resp.indexOf(",4") >= 0) return false;
      }
    }
  }
  return false;
}

void setup() {
  //  指示灯
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, HIGH);

  // USB 串口日志
  Serial.begin(115200);
  delay(1500);  // 等 USB CDC 稳定

  // 模组串口（UART）
  Serial1.begin(115200, SERIAL_8N1, RXD, TXD);
  Serial1.setRxBufferSize(SERIAL_BUFFER_SIZE);

  // 模组从“干净状态”启动（EN 断电重启 + 清串口噪声）
  while (Serial1.available()) Serial1.read();
  modemPowerCycle();
  while (Serial1.available()) Serial1.read();
  
  // 初始化长短信缓存
  initConcatBuffer();
  
  // 加载配置
  loadConfig();
  configValid = isConfigValid();
  

  // ========== 先初始化模组 ==========
  while (!sendATandWaitOK("AT", 1000)) {
    Serial.println("AT未响应，重试...");
    blink_short();
  }
  Serial.println("模组AT响应正常");
  
  //先设置CGACT，禁用数据连接
  while (!sendATandWaitOK("AT+CGACT=0,1", 5000)) {
    Serial.println("设置CGACT失败，重试...");
    blink_short();
  }
  Serial.println("已禁用数据连接(AT+CGACT=0,1)，防止流量消耗");
  
  //设置短信自动上报
  while (!sendATandWaitOK("AT+CNMI=2,2,0,0,0", 1000)) {
    Serial.println("设置CNMI失败，重试...");
    blink_short();
  }
  Serial.println("CNMI参数设置完成");
  
  //配置PDU模式
  while (!sendATandWaitOK("AT+CMGF=0", 1000)) {
    Serial.println("设置PDU模式失败，重试...");
    blink_short();
  }
  Serial.println("PDU模式设置完成");
  
  // ========== 模组AT初始化完成 ==========

  // 连接WiFi（支持隐藏SSID）
  // 参数: ssid, password, channel(0=自动), bssid(nullptr=自动), connect(true=连接隐藏网络)
  WiFi.begin(WIFI_SSID, WIFI_PASS, 0, nullptr, true);
  Serial.println("连接wifi");
  Serial.println(WIFI_SSID);
  while (WiFi.status() != WL_CONNECTED) blink_short();
  Serial.println("wifi已连接");
  Serial.print("IP地址: ");
  Serial.println(WiFi.localIP());
  
  // NTP时间同步（获取UTC时间）
  Serial.println("正在同步NTP时间...");
  configTime(0, 0, "ntp.ntsc.ac.cn", "ntp.aliyun.com", "pool.ntp.org");
  int ntpRetry = 0;
  while (time(nullptr) < 100000 && ntpRetry < 100) {
    delay(100);
    ntpRetry++;
  }
  if (time(nullptr) >= 100000) {
    timeSynced = true;
    Serial.println("NTP时间同步成功");
    time_t now = time(nullptr);
    Serial.print("当前UTC时间戳: ");
    Serial.println(now);
  } else {
    Serial.println("NTP时间同步失败，将使用设备时间");
  }
  
  // 启动HTTP服务器
  server.on("/", handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/tools", handleToolsPage);
  server.on("/sms", handleToolsPage);  // 兼容旧链接
  server.on("/sendsms", HTTP_POST, handleSendSms);
  server.on("/ping", HTTP_POST, handlePing);
  server.on("/query", handleQuery);
  server.on("/flight", handleFlightMode);
  server.on("/at", handleATCommand);
  server.begin();
  Serial.println("HTTP服务器已启动");
  
  ssl_client.setInsecure();
  digitalWrite(LED_BUILTIN, LOW);
  
  // 非阻塞检查网络注册状态（不再阻塞等待，改为loop中周期性检查）
  networkRegistered = waitCEREG();
  if (networkRegistered) {
    Serial.println("网络已注册");
  } else {
    Serial.println("网络尚未注册，将在后台继续检查...");
  }
  lastCeregCheck = millis();
  
  // 如果配置有效，发送启动通知
  if (configValid && networkRegistered) {
    Serial.println("配置有效，发送启动通知...");
    String subject = "短信转发器已启动";
    String body = "设备已启动\n设备地址: " + getDeviceUrl();
    sendEmailNotification(subject.c_str(), body.c_str());
  }
}

void loop() {
  // 处理HTTP请求
  server.handleClient();
  
  // 周期性检查网络注册状态
  if (!networkRegistered && millis() - lastCeregCheck >= CEREG_CHECK_INTERVAL) {
    lastCeregCheck = millis();
    networkRegistered = waitCEREG();
    if (networkRegistered) {
      Serial.println("网络已注册");
      // 网络刚注册成功，发送启动通知
      if (configValid) {
        Serial.println("配置有效，发送启动通知...");
        String subject = "短信转发器已启动";
        String body = "设备已启动\n设备地址: " + getDeviceUrl();
        sendEmailNotification(subject.c_str(), body.c_str());
      }
    } else {
      Serial.println("等待网络注册...");
    }
  }
  
  // 如果配置无效，每秒打印一次IP地址
  if (!configValid) {
    if (millis() - lastPrintTime >= 1000) {
      lastPrintTime = millis();
      Serial.println("⚠️ 请访问 " + getDeviceUrl() + " 配置系统参数");
    }
  }
  
  // 检查长短信超时
  checkConcatTimeout();
  
  // 本地透传
  if (Serial.available()) Serial1.write(Serial.read());
  // 检查URC和解析
  checkSerial1URC();
}
