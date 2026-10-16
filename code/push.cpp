#include "push.h"
#include "web_handlers.h"
#include "config.h"
#include "web_handlers.h"
#include "task_types.h"
#include <HTTPClient.h>
#include <mbedtls/md.h>
#include <base64.h>
#include <sys/time.h>

// 发送邮件通知函数（带重试）
void sendEmailNotification(const char* subject, const char* body) {
  if (config.smtpServer.length() == 0 || config.smtpUser.length() == 0 || 
      config.smtpPass.length() == 0 || config.smtpSendTo.length() == 0) {
    logCaptureLn(String("邮件配置不完整，跳过发送"));
    return;
  }

  if (WiFi.status() != WL_CONNECTED) {
    logCaptureLn(String("WiFi未连接，跳过邮件发送"));
    return;
  }

  auto statusCallback = [](SMTPStatus status) {
    logCaptureLn(String(status.text));
  };

  const int MAX_ATTEMPTS = 3; // 总尝试次数（含首次）
  for (int attempt = 1; attempt <= MAX_ATTEMPTS; attempt++) {
    if (attempt > 1) {
      delay(1000 * (attempt - 1)); // 退避：第2次前等1秒，第3次前等2秒
      logCaptureF("[邮件] 重试 (%d/%d)...\n", attempt, MAX_ATTEMPTS);
    }

    // 清理上一次尝试的残留连接状态，确保每次从干净状态开始
    smtp.stop();

    if (!smtp.connect(config.smtpServer.c_str(), config.smtpPort, statusCallback)) {
      logCaptureLn(String("邮件服务器连接失败"));
      continue;
    }

    if (!smtp.authenticate(config.smtpUser.c_str(), config.smtpPass.c_str(), readymail_auth_password)) {
      // 认证失败属配置错误，重试无意义
      logCaptureLn(String("邮件认证失败（请检查账号与SMTP授权码），停止重试"));
      smtp.stop();
      return;
    }

    SMTPMessage msg;
    String from = "sms notify <"; from += config.smtpUser; from += ">";
    msg.headers.add(rfc822_from, from.c_str());
    String to = "your_email <"; to += config.smtpSendTo; to += ">";
    msg.headers.add(rfc822_to, to.c_str());
    msg.headers.add(rfc822_subject, subject);
    msg.text.body(body);
    msg.timestamp = time(nullptr);

    if (smtp.send(msg)) {
      logCaptureF("[邮件] 发送成功（第 %d/%d 次尝试）\n", attempt, MAX_ATTEMPTS);
      smtp.stop();
      return;
    }
    logCaptureLn(String("邮件发送失败"));
  }

  logCaptureLn(String("邮件多次重试后仍失败，本次通知已丢弃"));
  smtp.stop();
}

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

// 判断业务响应体是否表示成功
// 部分平台无论成败 HTTP 都返回 200，错误信息在响应体中，需要额外校验
static bool isBodySuccess(const PushChannel& channel, const String& resp) {
  switch (channel.type) {
    case PUSH_TYPE_DINGTALK:   return resp.indexOf("\"errcode\":0") >= 0;
    case PUSH_TYPE_FEISHU:     return resp.indexOf("\"code\":0") >= 0;
    case PUSH_TYPE_PUSHPLUS:   return resp.indexOf("\"code\":200") >= 0;
    case PUSH_TYPE_SERVERCHAN: return resp.indexOf("\"code\":0") >= 0;
    case PUSH_TYPE_TELEGRAM:   return resp.indexOf("\"ok\":true") >= 0;
    default:                   return true; // 其余平台以 HTTP 状态码为准
  }
}

// 带重试地执行单个通道的 HTTP 请求（最多 3 次，失败后退避重试）
static bool executeChannelRequest(const PushChannel& channel, const String& url,
                                  bool isGet, const String& contentType, const String& body,
                                  const String& channelName) {
  const int MAX_ATTEMPTS = 3;
  for (int attempt = 1; attempt <= MAX_ATTEMPTS; attempt++) {
    if (attempt > 1) {
      delay(500 * (attempt - 1)); // 退避：第2次前等0.5秒，第3次前等1秒
      logCaptureF("[%s] 重试 (%d/%d)...\n", channelName.c_str(), attempt, MAX_ATTEMPTS);
    }

    HTTPClient http;
    http.begin(url);
    if (!isGet) http.addHeader("Content-Type", contentType);
    int httpCode = isGet ? http.GET() : http.POST(body);

    if (httpCode <= 0) {
      // 连接失败/超时等传输层错误，可重试
      logCaptureF("[%s] HTTP请求失败: %s\n", channelName.c_str(), http.errorToString(httpCode).c_str());
    } else {
      logCaptureF("[%s] 响应码: %d\n", channelName.c_str(), httpCode);
      bool httpOk = (httpCode >= 200 && httpCode < 300);
      String resp = "";
      if (httpOk) resp = http.getString();

      if (httpOk && isBodySuccess(channel, resp)) {
        logCaptureF("[%s] 推送成功（第 %d/%d 次尝试）\n", channelName.c_str(), attempt, MAX_ATTEMPTS);
        if (resp.length() > 0) logCaptureLn(String("响应: " + resp));
        http.end();
        return true;
      }

      if (resp.length() > 0) logCaptureLn(String("响应: " + resp));

      // 明确的 4xx 客户端错误（除 429 限流）多为配置问题，重试无意义
      if (httpCode >= 400 && httpCode < 500 && httpCode != 429) {
        logCaptureF("[%s] 客户端错误 %d，跳过重试\n", channelName.c_str(), httpCode);
        http.end();
        return false;
      }
      logCaptureLn(String("[" + channelName + "] 本次推送未确认成功"));
    }
    http.end();
  }

  logCaptureLn(String("[" + channelName + "] 多次重试后仍失败"));
  return false;
}

// 发送单个推送通道（统一构建请求参数，由 executeChannelRequest 执行并自动重试）
void sendToChannel(const PushChannel& channel, const char* sender, const char* message, const char* timestamp) {
  if (!channel.enabled) return;

  // 对于某些推送方式，URL可以为空（使用默认URL）
  bool needUrl = (channel.type == PUSH_TYPE_POST_JSON || channel.type == PUSH_TYPE_BARK ||
                  channel.type == PUSH_TYPE_GET || channel.type == PUSH_TYPE_DINGTALK ||
                  channel.type == PUSH_TYPE_CUSTOM);
  if (needUrl && channel.url.length() == 0) return;

  String channelName = channel.name.length() > 0 ? channel.name : ("通道" + String(channel.type));
  logCaptureLn(String("发送到推送通道: " + channelName));

  String senderEscaped = jsonEscape(String(sender));
  String messageEscaped = jsonEscape(String(message));
  String timestampEscaped = jsonEscape(String(timestamp));

  String reqUrl = "";
  String reqBody = "";
  String reqContentType = "application/json";
  bool useGet = false;

  switch (channel.type) {
    case PUSH_TYPE_POST_JSON: {
      // 标准POST JSON格式
      reqUrl = channel.url;
      reqBody = "{";
      reqBody += "\"sender\":\"" + senderEscaped + "\",";
      reqBody += "\"message\":\"" + messageEscaped + "\",";
      reqBody += "\"timestamp\":\"" + timestampEscaped + "\"";
      reqBody += "}";
      logCaptureLn(String("POST JSON: " + reqBody));
      break;
    }

    case PUSH_TYPE_BARK: {
      // Bark推送格式
      reqUrl = channel.url;
      reqBody = "{";
      reqBody += "\"title\":\"" + senderEscaped + "\",";
      reqBody += "\"body\":\"" + messageEscaped + "\"";
      reqBody += "}";
      logCaptureLn(String("BARK JSON: " + reqBody));
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
      logCaptureLn(String("GET URL: " + getUrl));
      reqUrl = getUrl;
      useGet = true;
      break;
    }

    case PUSH_TYPE_DINGTALK: {
      // 钉钉机器人（如果配置了secret，需要添加签名）
      String webhookUrl = channel.url;
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
      reqUrl = webhookUrl;
      reqBody = "{\"msgtype\":\"text\",\"text\":{\"content\":\"";
      reqBody += "📱短信通知\\n发送者: " + senderEscaped + "\\n内容: " + messageEscaped + "\\n时间: " + timestampEscaped;
      reqBody += "\"}}";
      logCaptureLn(String("钉钉: " + reqBody));
      break;
    }

    case PUSH_TYPE_PUSHPLUS: {
      // PushPlus
      reqUrl = channel.url.length() > 0 ? channel.url : "http://www.pushplus.plus/send";
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
      reqBody = "{";
      reqBody += "\"token\":\"" + channel.key1 + "\",";
      reqBody += "\"title\":\"短信来自: " + senderEscaped + "\",";
      reqBody += "\"content\":\"<b>发送者:</b> " + senderEscaped + "<br><b>时间:</b> " + timestampEscaped + "<br><b>内容:</b><br>" + messageEscaped + "\",";
      reqBody += "\"channel\":\"" + channelValue + "\"";
      reqBody += "}";
      logCaptureLn(String("PushPlus: " + reqBody));
      break;
    }

    case PUSH_TYPE_SERVERCHAN: {
      // Server酱
      reqUrl = channel.url.length() > 0 ? channel.url : ("https://sctapi.ftqq.com/" + channel.key1 + ".send");
      reqContentType = "application/x-www-form-urlencoded";
      reqBody = "title=" + urlEncode("短信来自: " + String(sender));
      reqBody += "&desp=" + urlEncode("**发送者:** " + String(sender) + "\n\n**时间:** " + String(timestamp) + "\n\n**内容:**\n\n" + String(message));
      logCaptureLn(String("Server酱: " + reqBody));
      break;
    }

    case PUSH_TYPE_CUSTOM: {
      // 自定义模板
      if (channel.customBody.length() == 0) {
        logCaptureLn(String("自定义模板为空，跳过"));
        return;
      }
      reqUrl = channel.url;
      reqBody = channel.customBody;
      reqBody.replace("{sender}", senderEscaped);
      reqBody.replace("{message}", messageEscaped);
      reqBody.replace("{timestamp}", timestampEscaped);
      logCaptureLn(String("自定义: " + reqBody));
      break;
    }

    case PUSH_TYPE_FEISHU: {
      // 飞书机器人
      String jsonData = "{";

      // 如果配置了secret，需要添加签名（飞书使用秒级时间戳）
      if (channel.key1.length() > 0) {
        int64_t ts = time(nullptr);
        // 飞书签名: base64(HMAC-SHA256(key=timestamp + "\n" + secret, msg=""))
        String stringToSign = String(ts) + "\n" + channel.key1;
        uint8_t hmacResult[32];
        mbedtls_md_context_t ctx;
        mbedtls_md_init(&ctx);
        mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 1);
        mbedtls_md_hmac_starts(&ctx, (const unsigned char*)stringToSign.c_str(), stringToSign.length());
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

      reqUrl = channel.url;
      reqBody = jsonData;
      logCaptureLn(String("飞书: " + reqBody));
      break;
    }

    case PUSH_TYPE_GOTIFY: {
      // Gotify 推送
      String gotifyUrl = channel.url;
      // 确保URL以/结尾
      if (!gotifyUrl.endsWith("/")) gotifyUrl += "/";
      gotifyUrl += "message?token=" + channel.key1;

      reqUrl = gotifyUrl;
      reqBody = "{";
      reqBody += "\"title\":\"短信来自: " + senderEscaped + "\",";
      reqBody += "\"message\":\"" + messageEscaped + "\\n\\n时间: " + timestampEscaped + "\",";
      reqBody += "\"priority\":5";
      reqBody += "}";
      logCaptureLn(String("Gotify: " + reqBody));
      break;
    }

    case PUSH_TYPE_TELEGRAM: {
      // Telegram Bot 推送（key1: Chat ID, key2: Bot Token）
      String tgBaseUrl = channel.url.length() > 0 ? channel.url : "https://api.telegram.org";
      if (tgBaseUrl.endsWith("/")) tgBaseUrl.remove(tgBaseUrl.length() - 1);

      reqUrl = tgBaseUrl + "/bot" + channel.key2 + "/sendMessage";

      String text = "📱短信通知\n发送者: " + senderEscaped + "\n内容: " + messageEscaped + "\n时间: " + timestampEscaped;
      reqBody = "{";
      reqBody += "\"chat_id\":\"" + channel.key1 + "\",";
      reqBody += "\"text\":\"" + text + "\"";
      reqBody += "}";

      logCaptureLn(String("Telegram: " + reqBody));
      break;
    }

    default:
      logCaptureLn(String("未知推送类型"));
      return;
  }

  executeChannelRequest(channel, reqUrl, useGet, reqContentType, reqBody, channelName);
}

// 发送短信到所有启用的推送通道
void sendSMSToServer(const char* sender, const char* message, const char* timestamp) {
  if (WiFi.status() != WL_CONNECTED) {
    logCaptureLn(String("WiFi未连接，跳过推送"));
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
    logCaptureLn(String("没有启用的推送通道"));
    return;
  }
  
  logCaptureLn(String("\n=== 开始多通道推送 ==="));
  for (int i = 0; i < MAX_PUSH_CHANNELS; i++) {
    if (isPushChannelValid(config.pushChannels[i])) {
      sendToChannel(config.pushChannels[i], sender, message, timestamp);
      delay(100); // 短暂延迟避免请求过快
    }
  }
  logCaptureLn(String("=== 多通道推送完成 ===\n"));
}

// ==================== 异步推送任务 ====================
// 推送/邮件是慢速阻塞操作（TLS握手+重试可能数十秒），
// 放进独立任务执行，主循环只负责入队，Web 界面不再被卡住。

#define NOTIFY_QUEUE_LEN 8

static QueueHandle_t notifyQueue = nullptr;

static void notifyTask(void* param) {
  NotifyJob job;
  for (;;) {
    if (xQueueReceive(notifyQueue, &job, portMAX_DELAY) != pdTRUE) continue;
    if (job.type == NOTIFY_JOB_SMS_PUSH) {
      sendSMSToServer(job.sender, job.body, job.timestamp);
    } else {
      sendEmailNotification(job.subject, job.body);
    }
  }
}

void initNotifyTask() {
  notifyQueue = xQueueCreate(NOTIFY_QUEUE_LEN, sizeof(NotifyJob));
  xTaskCreatePinnedToCore(notifyTask, "notify", 12288, nullptr, 1, nullptr, 0);
}

// 入队，队列满时丢弃并记录（推送可丢失，主循环不能阻塞）
static bool enqueueNotify(const NotifyJob& job) {
  if (notifyQueue == nullptr) return false;
  if (xQueueSend(notifyQueue, &job, 0) == pdTRUE) return true;
  logCaptureLn(String("⚠️ 推送队列已满，丢弃一条通知"));
  return false;
}

void queueSmsPush(const char* sender, const char* message, const char* timestamp) {
  NotifyJob job;
  job.type = NOTIFY_JOB_SMS_PUSH;
  strlcpy(job.sender, sender, sizeof(job.sender));
  strlcpy(job.timestamp, timestamp, sizeof(job.timestamp));
  strlcpy(job.body, message, sizeof(job.body));
  job.subject[0] = '\0';
  enqueueNotify(job);
}

void queueEmailNotification(const char* subject, const char* body) {
  NotifyJob job;
  job.type = NOTIFY_JOB_EMAIL;
  strlcpy(job.subject, subject, sizeof(job.subject));
  strlcpy(job.body, body, sizeof(job.body));
  job.sender[0] = '\0';
  job.timestamp[0] = '\0';
  enqueueNotify(job);
}

bool waitNotifyQueueEmpty(uint32_t timeoutMs) {
  if (notifyQueue == nullptr) return true;
  unsigned long start = millis();
  while (uxQueueMessagesWaiting(notifyQueue) > 0) {
    if (millis() - start >= timeoutMs) return false;
    delay(100);
  }
  // 队列空不代表最后一条已发完，再等一拍让进行中的请求收尾
  delay(500);
  return true;
}
