#include "push.h"
#include "web_handlers.h"
#include "config.h"
#include "inbox.h"
#include <HTTPClient.h>
#include <mbedtls/md.h>
#include <base64.h>
#include <sys/time.h>

// 发送邮件通知函数（在后台 worker 线程执行）。先在锁内快照 SMTP 配置(String 拷贝)，
// 之后整个 SMTP 会话只用本地副本，杜绝与 handleSave 改写 config.smtp* 的撕裂读。
void sendEmailNotification(const char* subject, const char* body) {
  muxLock(gWorkMux);
  String smServer = config.smtpServer;
  int    smPort   = config.smtpPort;
  String smUser   = config.smtpUser;
  String smPass   = config.smtpPass;
  String smTo     = config.smtpSendTo;
  muxUnlock(gWorkMux);

  if (smServer.length() == 0 || smUser.length() == 0 ||
      smPass.length() == 0 || smTo.length() == 0) {
    logCaptureLn("邮件配置不完整，跳过发送");
    return;
  }

  auto statusCallback = [](SMTPStatus status) {
    logCaptureLn(String(status.text));
  };
  smtp.connect(smServer.c_str(), smPort, statusCallback);
  if (smtp.isConnected()) {
    smtp.authenticate(smUser.c_str(), smPass.c_str(), readymail_auth_password);

    SMTPMessage msg;
    String from = "sms notify <"; from += smUser; from += ">";
    msg.headers.add(rfc822_from, from.c_str());
    String to = "your_email <"; to += smTo; to += ">";
    msg.headers.add(rfc822_to, to.c_str());
    msg.headers.add(rfc822_subject, subject);
    msg.text.body(body);
    msg.timestamp = time(nullptr);
    smtp.send(msg);
    logCaptureLn("邮件发送完成");
  } else {
    logCaptureLn("邮件服务器连接失败");
  }
}

// urlEncode 已移至 sms_logic.h（带 reserve，设备与主机测试共用）

// HMAC-SHA256(data, key) 的 base64（钉钉/飞书签名共用，避免重复 mbedtls 样板）
static String hmacSha256Base64(const String& data, const String& key) {
  uint8_t hmac[32];
  mbedtls_md_context_t ctx;
  mbedtls_md_init(&ctx);
  mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 1);
  mbedtls_md_hmac_starts(&ctx, (const unsigned char*)key.c_str(), key.length());
  mbedtls_md_hmac_update(&ctx, (const unsigned char*)data.c_str(), data.length());
  mbedtls_md_hmac_finish(&ctx, hmac);
  mbedtls_md_free(&ctx);
  return base64::encode(hmac, 32);
}

// 钉钉签名函数（时间戳为UTC毫秒级）：签名串 = timestamp\nsecret，HMAC key = secret，结果再 urlEncode
String dingtalkSign(const String& secret, int64_t timestamp) {
  String stringToSign = String(timestamp) + "\n" + secret;
  return urlEncode(hmacSha256Base64(stringToSign, secret));
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

// jsonEscape 已移至 sms_logic.h（带 reserve + 控制字符 \u00XX，设备与主机测试共用）

// P0-5 脱敏：推送负载含短信正文，默认只记字节数(不入网页日志)，SMS_LOG_VERBOSE 时记完整内容
static void logPayload(const char* label, const String& payload) {
#if SMS_LOG_VERBOSE
  logCaptureLn(String(label) + ": " + payload);
#else
  logCaptureF("%s: [%u bytes]\n", label, (unsigned)payload.length());
#endif
}

// 发送单个推送通道。返回 true 表示送达成功(2xx)。
bool sendToChannel(const PushChannel& channel, const char* sender, const char* message, const char* timestamp) {
  if (!channel.enabled) return false;

  // 对于某些推送方式，URL可以为空（使用默认URL）
  bool needUrl = (channel.type == PUSH_TYPE_POST_JSON || channel.type == PUSH_TYPE_BARK ||
                  channel.type == PUSH_TYPE_GET || channel.type == PUSH_TYPE_DINGTALK ||
                  channel.type == PUSH_TYPE_CUSTOM);
  if (needUrl && channel.url.length() == 0) return false;

  // P1-1 TLS 前预检：可分配堆不足时跳过，避免握手途中 OOM 崩溃(由重试队列稍后补发)
  if (ESP.getMaxAllocHeap() < TLS_MIN_FREE_HEAP) {
    logCaptureF("可分配堆不足(%u<%u)，跳过本次推送，稍后重试\n",
                (unsigned)ESP.getMaxAllocHeap(), (unsigned)TLS_MIN_FREE_HEAP);
    return false;
  }

  HTTPClient http;
  http.setReuse(false);
  http.setConnectTimeout(HTTP_CONNECT_TIMEOUT_MS);  // P1-1 连接超时
  http.setTimeout(HTTP_READ_TIMEOUT_MS);            // P1-1 读超时，避免无限阻塞
  String channelName = channel.name.length() > 0 ? channel.name : ("通道" + String(channel.type));
  logCaptureLn(String("发送到推送通道: " + channelName));

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
      logPayload("POST JSON", jsonData);
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
      logPayload("BARK JSON", jsonData);
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
      logPayload("GET URL", getUrl);
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
      jsonData += "短信通知\\n发送者: " + senderEscaped + "\\n内容: " + messageEscaped + "\\n时间: " + timestampEscaped;
      jsonData += "\"}}";
      logPayload("钉钉", jsonData);
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
              logCaptureLn(String("Invalid PushPlus channel '" + channel.key2 + "'. Using default 'wechat'."));
          }
      }
      String jsonData = "{";
      jsonData += "\"token\":\"" + channel.key1 + "\",";
      jsonData += "\"title\":\"短信来自: " + senderEscaped + "\",";
      jsonData += "\"content\":\"<b>发送者:</b> " + senderEscaped + "<br><b>时间:</b> " + timestampEscaped + "<br><b>内容:</b><br>" + messageEscaped + "\",";
      jsonData += "\"channel\":\"" + channelValue + "\"";
      jsonData += "}";
      logPayload("PushPlus", jsonData);
      httpCode = http.POST(jsonData);
      break;
    }

    case PUSH_TYPE_SERVERCHAN: {
      // Server酱
      // 健壮拼接：url 可填完整(.send)或仅 base；key1 可填 SendKey，复制时多余空格会被清理。
      String sendKey = channel.key1;
      String scBase = channel.url;
      sendKey.trim();
      scBase.trim();
      if (sendKey.length() == 0 && scBase.length() > 0 &&
          !scBase.startsWith("http://") && !scBase.startsWith("https://")) {
        sendKey = scBase;  // 兼容历史/误填：把 SendKey 填到了 URL 字段
        scBase = "";
      }
      if (scBase.length() == 0) scBase = "https://sctapi.ftqq.com";
      if (sendKey.startsWith("http://") || sendKey.startsWith("https://")) {
        scBase = sendKey;
        sendKey = "";
      }
      while (scBase.endsWith("/")) scBase.remove(scBase.length() - 1);
      String scUrl = (scBase.indexOf(".send") > 0) ? scBase : (scBase + "/" + sendKey + ".send");
      http.begin(scUrl);
      http.addHeader("Content-Type", "application/x-www-form-urlencoded");
      String postData = "title=" + urlEncode("短信来自: " + String(sender));
      postData += "&desp=" + urlEncode("**发送者:** " + String(sender) + "\n\n**时间:** " + String(timestamp) + "\n\n**内容:**\n\n" + String(message));
      logPayload("Server酱", postData);
      httpCode = http.POST(postData);
      break;
    }
    
    case PUSH_TYPE_CUSTOM: {
      // 自定义模板
      if (channel.customBody.length() == 0) {
        logCaptureLn("自定义模板为空，跳过");
        return false;
      }
      http.begin(channel.url);
      http.addHeader("Content-Type", "application/json");
      String body = channel.customBody;
      body.replace("{sender}", senderEscaped);
      body.replace("{message}", messageEscaped);
      body.replace("{timestamp}", timestampEscaped);
      logPayload("自定义", body);
      httpCode = http.POST(body);
      break;
    }
    
    case PUSH_TYPE_FEISHU: {
      // 飞书机器人
      String webhookUrl = channel.url;
      String jsonData = "{";
      
      // 如果配置了secret，需要添加签名
      if (channel.key1.length() > 0) {
        // 飞书使用秒级时间戳；签名: base64(hmac-sha256(timestamp + "\n" + secret, secret))
        int64_t ts = time(nullptr);
        String stringToSign = String(ts) + "\n" + channel.key1;
        String sign = hmacSha256Base64(stringToSign, channel.key1);

        jsonData += "\"timestamp\":\"" + String(ts) + "\",";
        jsonData += "\"sign\":\"" + sign + "\",";
      }
      
      // 飞书消息体
      jsonData += "\"msg_type\":\"text\",";
      jsonData += "\"content\":{\"text\":\"";
      jsonData += "短信通知\\n发送者: " + senderEscaped + "\\n内容: " + messageEscaped + "\\n时间: " + timestampEscaped;
      jsonData += "\"}}";
      
      http.begin(webhookUrl);
      http.addHeader("Content-Type", "application/json");
      logPayload("飞书", jsonData);
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
      logPayload("Gotify", jsonData);
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
      String text = "短信通知\n发送者: " + senderEscaped + "\n内容: " + messageEscaped + "\n时间: " + timestampEscaped;
      jsonData += "\"text\":\"" + text + "\"";
      jsonData += "}";
      
      logPayload("Telegram", jsonData);
      httpCode = http.POST(jsonData);
      break;
    }
    
    default:
      logCaptureLn("未知推送类型");
      return false;
  }

  bool ok = (httpCode >= 200 && httpCode < 300);
  if (httpCode > 0) {
    logCaptureF("[%s] 响应码: %d\n", channelName.c_str(), httpCode);
    if (ok) {
      String response = http.getString();
      logPayload("响应", response);
    }
  } else {
    logCaptureF("[%s] HTTP请求失败: %s\n", channelName.c_str(), http.errorToString(httpCode).c_str());
  }
  http.end();
  return ok;
}

// ---- P1-2 有界推送重试队列 ----
// 注：推送/邮件/测试三类慢任务已移到后台 worker 线程执行(见 pushWorkerTask)，loop 不再被其阻塞，
// 故原先用于避让网页的人为节流(slowWorkGraceActive + 各 grace 间隔)整套删除——worker 单任务串行
// 本身就是天然限速，无需再额外延迟。所有队列槽位访问改用 gWorkMux 保护(loop 生产 / worker 消费)。
struct RetryItem {
  bool used;
  uint8_t channelIdx;
  uint8_t attempts;       // 已尝试次数
  uint32_t nextAttemptMs; // 下次重试时间(millis)
  String sender;
  String message;
  String timestamp;
};
static RetryItem retryQueue[PUSH_QUEUE_MAX];  // 全局零初始化 -> used=false

int retryQueueDepth() {
  muxLock(gWorkMux);
  int n = 0;
  for (int i = 0; i < PUSH_QUEUE_MAX; i++) if (retryQueue[i].used) n++;
  muxUnlock(gWorkMux);
  return n;
}

// 调用方须已持有 gWorkMux(或处于无并发的早期阶段)
static void enqueueRetryDelayMs(uint8_t ch, const char* sender, const char* message,
                                const char* timestamp, uint8_t attempts, uint32_t delayMs) {
  int slot = -1;
  for (int i = 0; i < PUSH_QUEUE_MAX; i++) {
    if (!retryQueue[i].used) { slot = i; break; }
  }
  if (slot < 0) {  // 队列满：丢弃尝试次数最多(最接近放弃)的一项，记日志
    int victim = 0;
    for (int i = 1; i < PUSH_QUEUE_MAX; i++) {
      if (retryQueue[i].attempts > retryQueue[victim].attempts) victim = i;
    }
    logCaptureF("重试队列已满，丢弃通道%u的一条待重试消息\n", retryQueue[victim].channelIdx + 1);
    slot = victim;
  }
  retryQueue[slot].used = true;
  retryQueue[slot].channelIdx = ch;
  retryQueue[slot].attempts = attempts;
  retryQueue[slot].sender = sender;
  retryQueue[slot].message = message;
  retryQueue[slot].timestamp = timestamp;
  retryQueue[slot].nextAttemptMs = millis() + delayMs;
}

static void enqueueRetry(uint8_t ch, const char* sender, const char* message,
                         const char* timestamp, uint8_t attempts) {
  uint32_t delaySec = backoffSeconds(attempts ? attempts : 1, PUSH_RETRY_BASE_SEC,
                                     PUSH_RETRY_MAX_SEC, (uint32_t)ch * 7 + attempts);
  muxLock(gWorkMux);
  enqueueRetryDelayMs(ch, sender, message, timestamp, attempts, delaySec * 1000UL);
  muxUnlock(gWorkMux);
}

// 首发不再人为错峰(原 order*PUSH_JOB_GAP_MS)：全部置为立即到期，由 worker 单任务串行逐条发出。
static void enqueueInitialPush(uint8_t ch, const char* sender, const char* message,
                               const char* timestamp) {
  muxLock(gWorkMux);
  enqueueRetryDelayMs(ch, sender, message, timestamp, 0, 0);
  muxUnlock(gWorkMux);
}

// 调用方须已持有 gWorkMux
static void freeRetrySlot(int i) {
  retryQueue[i].used = false;
  retryQueue[i].sender = "";
  retryQueue[i].message = "";
  retryQueue[i].timestamp = "";
}

// 后台 worker 周期调用：每次最多处理一条到期项。锁内挑选+快照通道配置，解锁后再做阻塞网络发送，
// 完成后再加锁更新该槽(重试/释放)。绝不在持锁期间发起网络。
void processRetryQueue() {
  if (WiFi.status() != WL_CONNECTED) return;
  if (ESP.getMaxAllocHeap() < TLS_MIN_FREE_HEAP) return;            // 堆紧张，稍后再试
  uint32_t now = millis();

  int picked = -1;
  uint8_t ch = 0, attempts = 0;
  String sndr, msg, ts;
  PushChannel chCopy;
  bool valid = false;
  muxLock(gWorkMux);
  for (int i = 0; i < PUSH_QUEUE_MAX; i++) {
    if (!retryQueue[i].used) continue;
    if ((int32_t)(now - retryQueue[i].nextAttemptMs) < 0) continue;  // 未到时间
    picked = i;
    ch = retryQueue[i].channelIdx;
    attempts = retryQueue[i].attempts;
    sndr = retryQueue[i].sender;
    msg = retryQueue[i].message;
    ts = retryQueue[i].timestamp;
    if (ch < MAX_PUSH_CHANNELS) { chCopy = config.pushChannels[ch]; valid = isPushChannelValid(chCopy); }
    break;
  }
  if (picked < 0) { muxUnlock(gWorkMux); return; }
  if (!valid) { freeRetrySlot(picked); muxUnlock(gWorkMux); return; }  // 通道已被删除/禁用，放弃
  muxUnlock(gWorkMux);

  logCaptureF("%s 通道%u 第%u次\n", attempts ? "重试推送" : "发送推送", ch + 1, attempts + 1);
  gSlowWorkBusy = true;
  bool ok = sendToChannel(chCopy, sndr.c_str(), msg.c_str(), ts.c_str());
  gSlowWorkBusy = false;

  muxLock(gWorkMux);
  if (retryQueue[picked].used && retryQueue[picked].channelIdx == ch) {  // 槽未被回收复用
    retryQueue[picked].attempts++;
    if (ok) {
      freeRetrySlot(picked);
    } else if (retryQueue[picked].attempts >= PUSH_RETRY_MAX) {
      logCaptureF("通道%u 重试%u次仍失败，放弃\n", ch + 1, retryQueue[picked].attempts);
      freeRetrySlot(picked);
    } else {
      int i = picked;
      uint32_t delaySec = backoffSeconds(retryQueue[i].attempts, PUSH_RETRY_BASE_SEC,
                                         PUSH_RETRY_MAX_SEC, (uint32_t)i * 7 + retryQueue[i].attempts);
      retryQueue[i].nextAttemptMs = now + delaySec * 1000UL;
    }
  }
  muxUnlock(gWorkMux);
}

// ---- C2 测试推送后台队列：避免 /testpush 请求同步等待慢 HTTPS/Webhook ----
struct TestPushJob {
  bool pending;
  bool running;
  bool done;
  bool success;
  unsigned long queuedMs;
  String message;
};

static TestPushJob testPushJobs[MAX_PUSH_CHANNELS];

bool enqueueTestPush(uint8_t ch, String& message) {
  if (ch >= MAX_PUSH_CHANNELS) {
    message = "通道序号无效";
    return false;
  }
  if (WiFi.status() != WL_CONNECTED) {
    message = "WiFi 未连接，暂不能测试推送";
    return false;
  }
  muxLock(gWorkMux);
  bool valid = isPushChannelValid(config.pushChannels[ch]);
  TestPushJob& job = testPushJobs[ch];
  bool busy = (job.pending || job.running);
  if (valid && !busy) {
    job.pending = true;
    job.running = false;
    job.done = false;
    job.success = false;
    job.queuedMs = millis();
    job.message = "测试推送已排队，可继续刷新网页";
    message = job.message;
  }
  muxUnlock(gWorkMux);
  if (!valid) { message = "该通道未启用或配置不完整(请先保存)"; return false; }
  if (busy)   { message = "该通道测试已在后台进行"; return true; }
  logCaptureF("测试推送通道%u已入队\n", ch + 1);
  return true;
}

String testPushStatusJson(uint8_t ch) {
  String j;
  j.reserve(180);
  if (ch >= MAX_PUSH_CHANNELS) {
    return "{\"queued\":false,\"running\":false,\"done\":true,\"success\":false,\"message\":\"通道序号无效\"}";
  }
  muxLock(gWorkMux);                 // worker 可能正在改写本通道 job 状态/message(String)
  TestPushJob& job = testPushJobs[ch];
  bool q = job.pending, r = job.running, d = job.done, s = job.success;
  String msg = job.message.length() ? job.message : "未开始测试";
  muxUnlock(gWorkMux);
  j += "{";
  j += "\"queued\":"; j += (q ? "true" : "false"); j += ",";
  j += "\"running\":"; j += (r ? "true" : "false"); j += ",";
  j += "\"done\":"; j += (d ? "true" : "false"); j += ",";
  j += "\"success\":"; j += (s ? "true" : "false"); j += ",";
  j += "\"message\":\""; j += jsonEscape(msg); j += "\"";
  j += "}";
  return j;
}

// 后台 worker 周期调用：锁内取一个待测通道 + 快照通道配置，解锁后做阻塞发送，再加锁写回结果。
void processTestPushQueue() {
  if (WiFi.status() != WL_CONNECTED) return;
  if (ESP.getMaxAllocHeap() < TLS_MIN_FREE_HEAP) return;

  int picked = -1;
  PushChannel chCopy;
  bool valid = false;
  muxLock(gWorkMux);
  for (int i = 0; i < MAX_PUSH_CHANNELS; i++) {
    if (!testPushJobs[i].pending) continue;
    picked = i;
    testPushJobs[i].pending = false;
    testPushJobs[i].running = true;
    testPushJobs[i].done = false;
    testPushJobs[i].success = false;
    testPushJobs[i].message = "测试推送发送中";
    chCopy = config.pushChannels[i];
    valid = isPushChannelValid(chCopy);
    break;
  }
  muxUnlock(gWorkMux);
  if (picked < 0) return;

  bool ok = false;
  String resultMsg;
  if (!valid) {
    resultMsg = "通道配置已变更或未启用，测试取消";
  } else {
    char ts[24];
    time_t now = time(nullptr);
    struct tm* t = localtime(&now);
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", t);
    logCaptureF("后台测试推送通道%u开始\n", picked + 1);
    gSlowWorkBusy = true;
    ok = sendToChannel(chCopy, "测试", "这是一条来自 SMS Forwarder 的测试推送", ts);
    gSlowWorkBusy = false;
    resultMsg = ok ? "测试推送已发送" : "测试推送失败，请查看日志";
  }

  muxLock(gWorkMux);
  testPushJobs[picked].running = false;
  testPushJobs[picked].done = true;
  testPushJobs[picked].success = ok;
  testPushJobs[picked].message = resultMsg;
  muxUnlock(gWorkMux);
}

// ---- 短信转发邮件队列：避免 SMTP 在收到短信当帧阻塞网页刷新 ----
struct EmailItem {
  String subject;
  String body;
};

static EmailItem emailQueue[EMAIL_QUEUE_MAX];
static int emailHead = 0;
static int emailCount = 0;

int emailQueueDepth() {
  muxLock(gWorkMux);
  int n = emailCount;
  muxUnlock(gWorkMux);
  return n;
}

static void clearEmailSlot(int i) {
  emailQueue[i].subject = "";
  emailQueue[i].body = "";
}

// 生产端(loop/URC/调度器)：入队一封转发邮件。锁内只动槽位，日志在锁外。
static bool enqueueEmailJob(const char* subject, const char* body) {
  muxLock(gWorkMux);
  bool full = (emailCount >= EMAIL_QUEUE_MAX);
  int depth = emailCount;
  if (!full) {
    int tail = (emailHead + emailCount) % EMAIL_QUEUE_MAX;
    emailQueue[tail].subject = subject;
    emailQueue[tail].body = body;
    depth = ++emailCount;
  }
  muxUnlock(gWorkMux);
  if (full) { logCaptureLn("邮件队列已满，丢弃一封短信转发邮件"); return false; }
  logCaptureF("短信转发邮件已入队，当前待发=%d\n", depth);
  return true;
}

bool enqueueEmailNotification(const char* subject, const char* body) {
  return enqueueEmailJob(subject, body);
}

// 消费端(后台 worker)：锁内出队一封(拷出后释放槽)，解锁后做阻塞 SMTP 发送。
void processEmailQueue() {
  if (WiFi.status() != WL_CONNECTED) return;
  if (ESP.getMaxAllocHeap() < TLS_MIN_FREE_HEAP) return;

  muxLock(gWorkMux);
  if (emailCount == 0) { muxUnlock(gWorkMux); return; }
  EmailItem it = emailQueue[emailHead];
  clearEmailSlot(emailHead);
  emailHead = (emailHead + 1) % EMAIL_QUEUE_MAX;
  emailCount--;
  muxUnlock(gWorkMux);

  gSlowWorkBusy = true;   // 邮件已出队、SMTP 在途：让 heapGuard/定时重启避让，避免误判空闲
  sendEmailNotification(it.subject.c_str(), it.body.c_str());
  gSlowWorkBusy = false;
}

// ---- 接收/转发解耦：已收短信先入有界转发队列，由 loop() 异步逐条转发，
//      使慢速 HTTP/SMTP 不阻塞 URC 接收(参照 esp32-sms-bridge "接收不依赖转发")。----
struct ForwardItem { String sender, text, timestamp; uint32_t inboxId; };
static ForwardItem fwdQueue[FWD_QUEUE_MAX];
static int fwdHead = 0, fwdCount = 0;

int forwardQueueDepth() { return fwdCount; }

static void clearFwdSlot(int i) {
  fwdQueue[i].sender = ""; fwdQueue[i].text = ""; fwdQueue[i].timestamp = ""; fwdQueue[i].inboxId = 0;
}

// 实际拆分一条转发任务(推送逐通道入队 + 邮件入队)，并回标收件箱已转发
static void forwardNow(const ForwardItem& it) {
  // 转发规则：决定发往哪些通道 / 是否发邮件 / 是否丢弃(无规则命中=默认全部通道+邮件)
  ForwardDecision fd = evalForwardRules(config.forwardRules, it.sender, it.text);
  if (fd.matched && fd.drop) {
    logCaptureLn(String("转发规则命中：丢弃来自 ") + maskPhone(it.sender));
    return;  // 不转发、不发邮件
  }
  unsigned mask = fd.matched ? fd.chMask : 0xFF;   // 命中按规则掩码；未命中=全部通道
  bool doEmail = fd.matched ? fd.email : true;     // 未命中保持原行为(发邮件)
  if (!config.pushEnabled) mask = 0;               // 推送转发总开关
  if (!config.emailEnabled) doEmail = false;       // 邮件转发总开关
  bool pushed = sendSMSToServer(it.sender.c_str(), it.text.c_str(), it.timestamp.c_str(), mask);
  bool emailQueued = false;
  if (doEmail) {
    // 邮件配置完整才算真发出(与 sendEmailNotification 内部判断一致)
    bool emailComplete = config.smtpServer.length() && config.smtpUser.length() &&
                         config.smtpPass.length() && config.smtpSendTo.length();
    if (emailComplete) {
      String subject = "短信"; subject += it.sender; subject += ","; subject += it.text;
      String body = "来自："; body += it.sender; body += "，时间："; body += it.timestamp;
      body += "，内容："; body += it.text;
      emailQueued = enqueueEmailJob(subject.c_str(), body.c_str());
    }
  }
  if (pushed || emailQueued) inboxMarkForwarded(it.inboxId);  // 至少有动作成功入队才标记"已转发"
}

void enqueueForward(const char* sender, const char* text, const char* timestamp, uint32_t inboxId) {
  if (fwdCount == FWD_QUEUE_MAX) {
    // 队满：先把队首拆分成慢任务队列腾出空间，避免在接收路径里同步跑HTTP/SMTP
    forwardNow(fwdQueue[fwdHead]);
    clearFwdSlot(fwdHead);
    fwdHead = (fwdHead + 1) % FWD_QUEUE_MAX;
    fwdCount--;
  }
  int tail = (fwdHead + fwdCount) % FWD_QUEUE_MAX;
  fwdQueue[tail].sender = sender;
  fwdQueue[tail].text = text;
  fwdQueue[tail].timestamp = timestamp;
  fwdQueue[tail].inboxId = inboxId;
  fwdCount++;
}

// loop() 周期调用：每帧最多转发一条。forwardNow 只做规则判定 + 把任务塞进 worker 的推送/邮件队列
// (无网络)，开销极小、立即执行——真正的慢速 HTTP/SMTP 由后台 worker 发出，不阻塞接收与网页。
// fwdQueue 仅 loop/URC 单线程访问(生产 enqueueForward / 消费此处)，无需加锁。
void processForwardQueue() {
  if (fwdCount == 0) return;
  ForwardItem it = fwdQueue[fwdHead];                 // 复制出来
  clearFwdSlot(fwdHead);
  fwdHead = (fwdHead + 1) % FWD_QUEUE_MAX;
  fwdCount--;
  forwardNow(it);
}

// 将短信拆成逐通道推送任务（失败/断网则继续留在重试队列，不静默丢失）
bool sendSMSToServer(const char* sender, const char* message, const char* timestamp, unsigned chMask) {
  bool hasEnabledChannel = false;
  for (int i = 0; i < MAX_PUSH_CHANNELS; i++) {
    if (isPushChannelValid(config.pushChannels[i])) {
      hasEnabledChannel = true;
      break;
    }
  }
  if (!hasEnabledChannel) {
    logCaptureLn("没有启用的推送通道");
    return false;
  }

  bool wifiUp = (WiFi.status() == WL_CONNECTED);
  if (!wifiUp) logCaptureLn("WiFi未连接，推送任务等待网络恢复");

  logCaptureLn("\n=== 多通道推送入队 ===");
  int dispatched = 0;
  for (int i = 0; i < MAX_PUSH_CHANNELS; i++) {
    if (!isPushChannelValid(config.pushChannels[i])) continue;
    if (!(chMask & (1u << i))) continue;  // 转发规则未选中此通道
    if (!wifiUp) {
      enqueueRetry((uint8_t)i, sender, message, timestamp, 0);  // 断网直接入队
    } else {
      enqueueInitialPush((uint8_t)i, sender, message, timestamp);
    }
    dispatched++;
  }
  logCaptureLn("=== 多通道推送已入队 ===\n");
  return dispatched > 0;  // 至少一个启用通道被规则选中并发送/入队，才算转发动作发生
}

// ---- 后台 worker 任务：专跑推送(HTTP/HTTPS)+邮件(SMTP)+测试推送这三类慢速 WiFi/TLS 工作 ----
// 与 loop()(网页服务/模组AT/短信收发/保号) 并发：worker 在 TLS/socket 阻塞时由 FreeRTOS 让出 CPU，
// loop 即可继续响应网页，从根本上做到"转发/邮件不阻塞收信与网页刷新"。
// 全程单任务串行 => 任一时刻仅一个 TLS 会话，堆占用与改造前持平(仍受 TLS_MIN_FREE_HEAP 预检保护)。
// worker 只碰 WiFi，绝不访问 Serial1/模组(那仍归 loop)；共享状态经 gWorkMux/gLogMux 保护。
static void pushWorkerTask(void* arg) {
  for (;;) {
    bool wifi = (WiFi.status() == WL_CONNECTED);
    if (wifi) {
      processRetryQueue();     // 每次最多一条到期推送
      processEmailQueue();     // 每次最多一封转发邮件
      processTestPushQueue();  // 每次最多一个测试通道
    }
    // 阻塞式发送本身已在网络等待时让出 CPU；无活时小睡，喂看门狗、把 CPU 还给 loop。
    vTaskDelay(pdMS_TO_TICKS(wifi ? 20 : 500));
  }
}

void startPushWorker() {
  xTaskCreate(pushWorkerTask, "pushwk", PUSH_WORKER_STACK, nullptr, PUSH_WORKER_PRIO, nullptr);
  logCaptureLn("推送/邮件后台 worker 已启动");
}
