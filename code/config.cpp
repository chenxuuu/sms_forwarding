#include "config.h"
#include "web_handlers.h"

// 保存配置到NVS
void saveConfig() {
  preferences.begin("sms_config", false);
  preferences.putString("wifiSsid", config.wifiSsid);
  preferences.putString("wifiPass", config.wifiPass);
  preferences.putString("smtpServer", config.smtpServer);
  preferences.putInt("smtpPort", config.smtpPort);
  preferences.putString("smtpUser", config.smtpUser);
  preferences.putString("smtpPass", config.smtpPass);
  preferences.putString("smtpSendTo", config.smtpSendTo);
  preferences.putString("adminPhone", config.adminPhone);
  preferences.putString("webUser", config.webUser);
  preferences.putString("webPass", config.webPass);
  preferences.putString("numBlkList", config.numberBlackList);
  preferences.putString("fwdRules", config.forwardRules);
  preferences.putBool("emailEn", config.emailEnabled);
  preferences.putBool("pushEn", config.pushEnabled);

  // 保号定时任务
  preferences.putBool("kaEn", config.kaEnabled);
  preferences.putInt("kaDays", config.kaIntervalDays);
  preferences.putUChar("kaAct", config.kaAction);
  preferences.putString("kaTarget", config.kaTarget);
  preferences.putUInt("kaLast", config.kaLastTime);
  preferences.putInt("tzMin", config.tzOffsetMin);
  preferences.putString("ntpSrv", config.ntpServer);
  preferences.putBool("rbEn", config.rebootEnabled);
  preferences.putInt("rbHour", config.rebootHour);
  preferences.putBool("hbEn", config.hbEnabled);
  preferences.putInt("hbHour", config.hbHour);
  // SIM / 蜂窝数据
  preferences.putBool("dataEn", config.dataEnabled);
  preferences.putString("apn", config.apn);
  preferences.putString("opPlmn", config.operatorPlmn);
  preferences.putString("phoneNum", config.phoneNumber);

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
  logCaptureLn("配置已保存");
}

// 从NVS加载配置
void loadConfig() {
  preferences.begin("sms_config", true);
  config.wifiSsid = preferences.getString("wifiSsid", "");
  config.wifiPass = preferences.getString("wifiPass", "");
  config.smtpServer = preferences.getString("smtpServer", "");
  config.smtpPort = preferences.getInt("smtpPort", 465);
  config.smtpUser = preferences.getString("smtpUser", "");
  config.smtpPass = preferences.getString("smtpPass", "");
  config.smtpSendTo = preferences.getString("smtpSendTo", "");
  config.adminPhone = preferences.getString("adminPhone", "");
  config.webUser = preferences.getString("webUser", DEFAULT_WEB_USER);
  config.webPass = preferences.getString("webPass", DEFAULT_WEB_PASS);
  config.numberBlackList = preferences.getString("numBlkList", "");
  config.forwardRules = preferences.getString("fwdRules", "");
  config.emailEnabled = preferences.getBool("emailEn", true);
  config.pushEnabled = preferences.getBool("pushEn", true);

  // 保号定时任务（带默认值，旧配置升级零迁移）
  config.kaEnabled = preferences.getBool("kaEn", false);
  config.kaIntervalDays = preferences.getInt("kaDays", 175);
  config.kaAction = preferences.getUChar("kaAct", KA_ACTION_PING);
  config.kaTarget = preferences.getString("kaTarget", "");
  config.kaLastTime = preferences.getUInt("kaLast", 0);
  config.tzOffsetMin = preferences.getInt("tzMin", 480);
  config.ntpServer = preferences.getString("ntpSrv", "ntp.aliyun.com");
  config.rebootEnabled = preferences.getBool("rbEn", false);
  config.rebootHour = preferences.getInt("rbHour", 4);
  config.hbEnabled = preferences.getBool("hbEn", false);
  config.hbHour = preferences.getInt("hbHour", 9);
  // SIM / 蜂窝数据（默认禁用流量，零迁移）
  config.dataEnabled = preferences.getBool("dataEn", false);
  config.apn = preferences.getString("apn", "");
  config.operatorPlmn = preferences.getString("opPlmn", "");
  config.phoneNumber = preferences.getString("phoneNum", "");
  // 模组身份信息缓存：不是用户配置，仅用于首页启动后立即显示上次成功查询值。
  modemImei = preferences.getString("modemImei", "");
  modemIccid = preferences.getString("modemIccid", "");

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
    logCaptureLn("已迁移旧HTTP配置到推送通道1");
  }
  
  preferences.end();
  logCaptureLn("配置已加载");
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
      return ch.key1.length() > 0;  // 靠 key1（token）
    case PUSH_TYPE_SERVERCHAN:
      return ch.key1.length() > 0 || ch.url.indexOf(".send") > 0 ||
             (ch.url.length() > 0 && !ch.url.startsWith("http://") && !ch.url.startsWith("https://"));  // SendKey/完整URL/误填到URL框均可
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
