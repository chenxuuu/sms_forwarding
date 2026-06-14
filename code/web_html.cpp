#include "config_types.h"

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
      
      <div class="section">
        <div class="section-title">🚫 号码黑名单</div>
        <div class="hint" style="margin-bottom:15px;">每行一个号码，来自黑名单号码的短信将被忽略。</div>
        <div class="form-group">
          <label>黑名单号码</label>
          <textarea name="numberBlackList" rows="5">%NUMBER_BLACK_LIST%</textarea>
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
