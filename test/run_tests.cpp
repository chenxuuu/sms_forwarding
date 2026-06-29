// 主机侧纯逻辑单元测试（无 Arduino/pdulib 依赖）。
// 构建运行: g++ -std=c++17 -O2 test/run_tests.cpp -o test/run_tests && ./test/run_tests
//
// 覆盖 code/sms_logic.h 中可移植逻辑：JSON/URL/HTML 转义、号码脱敏与黑名单匹配、
// 去重哈希、保号到期判定、验证码提取、退避计算、%TOKEN% 模板扫描器（含 CSS 裸 % 用例）。
// 受 Arduino/pdulib 依赖的部分（PDU 解码、AT 状态机、WebServer 流式）无法主机测，已在报告标注。

#define SMS_LOGIC_NO_ARDUINO 1
#include "arduino_string_shim.h"
#include "../code/sms_logic.h"

#include <cstdio>
#include <string>

static int g_pass = 0, g_fail = 0;

static void check(bool cond, const char* name, const std::string& got = "") {
  if (cond) {
    g_pass++;
  } else {
    g_fail++;
    std::printf("  FAIL: %s   got=[%s]\n", name, got.c_str());
  }
}
static void eq(const String& got, const char* want, const char* name) {
  check(std::string(got.c_str()) == want, name, got.c_str());
}

// 用于 streamTemplate 测试的 resolver
static bool resolver(const String& name, String& out) {
  if (name == "IP") { out = "192.168.1.5"; return true; }
  if (name == "WIFI_SSID") { out = "home"; return true; }
  if (name == "PUSH_COUNT") { out = "3"; return true; }
  return false;  // 未知 token 原样保留
}

static std::string runTemplate(const char* tpl) {
  std::string buf;
  streamTemplate(tpl,
                 [&](const char* p, size_t n) { buf.append(p, n); },
                 resolver);
  return buf;
}

int main() {
  // ---- jsonEscape ----
  eq(jsonEscape(String("a\"b\\c")), "a\\\"b\\\\c", "jsonEscape quotes/backslash");
  eq(jsonEscape(String("line1\nline2\t")), "line1\\nline2\\t", "jsonEscape nl/tab");
  eq(jsonEscape(String("ctrl\x01")), "ctrl\\u0001", "jsonEscape control char");
  eq(jsonEscape(String("中文ok")), "中文ok", "jsonEscape utf8 passthrough");

  // ---- urlEncode ----
  eq(urlEncode(String("a b")), "a+b", "urlEncode space");
  eq(urlEncode(String("a/b?c=d")), "a%2Fb%3Fc%3Dd", "urlEncode reserved");
  eq(urlEncode(String("Az09")), "Az09", "urlEncode alnum");

  // ---- htmlEscape ----
  eq(htmlEscape(String("<script>&\"'")), "&lt;script&gt;&amp;&quot;&#39;", "htmlEscape");

  // ---- stripCountryCode / maskPhone ----
  eq(stripCountryCode(String("+8613800138000")), "13800138000", "stripCC +86");
  eq(stripCountryCode(String("13800138000")), "13800138000", "stripCC none");
  eq(maskPhone(String("13800138000")), "138****8000", "maskPhone 11-digit");
  eq(maskPhone(String("123")), "123", "maskPhone short passthrough");

  // ---- numberMatchesBlacklist ----
  check(numberMatchesBlacklist(String("10086\n13800138000"), String("13800138000")),
        "blacklist direct match");
  check(numberMatchesBlacklist(String("13800138000"), String("+8613800138000")),
        "blacklist +86 normalize");
  check(!numberMatchesBlacklist(String("10086"), String("13800138000")),
        "blacklist no match");
  check(!numberMatchesBlacklist(String(""), String("13800138000")),
        "blacklist empty list");

  // ---- fnv1a32 dedup ----
  check(fnv1a32(String("abc")) == fnv1a32(String("abc")), "fnv1a deterministic");
  check(fnv1a32(String("abc")) != fnv1a32(String("abd")), "fnv1a distinct");

  // ---- keepAliveDue (giffgaff 175d) ----
  const uint32_t NOW = 1750000000u;          // 有效现今时间
  check(keepAliveDue(NOW - 176u * 86400u, NOW, 175), "keepAlive due after 176d");
  check(!keepAliveDue(NOW - 100u * 86400u, NOW, 175), "keepAlive not due at 100d");
  check(!keepAliveDue(0, 12345, 175), "keepAlive invalid now -> not due");
  check(keepAliveDue(0, NOW, 175), "keepAlive no baseline -> due");
  check(!keepAliveDue(NOW - 200u * 86400u, NOW, 0), "keepAlive interval 0 -> off");

  // ---- formatEpochLocal ----
  eq(formatEpochLocal(1750000000u, 480), "2025-06-15 23:06:40 (UTC+8)", "formatEpochLocal UTC+8");
  eq(formatEpochLocal(1750000000u, 0), "2025-06-15 15:06:40 (UTC)", "formatEpochLocal UTC");
  eq(formatEpochLocal(12345u, 480), "时间未同步", "formatEpochLocal invalid epoch");

  // ---- extractOtp ----
  eq(extractOtp(String("您的验证码是 482913，请勿泄露")), "482913", "otp extract 6-digit");
  eq(extractOtp(String("code 12 then 8765")), "8765", "otp skip short, take 4-digit");
  eq(extractOtp(String("no code here")), "", "otp none");
  eq(extractOtp(String("id 1234567890 too long")), "", "otp reject >8 digits");

  // ---- extractImei ----
  eq(extractImei(String("AT+CGSN=1\r\n+CGSN: 867200123456789\r\n\r\nOK\r\n")),
     "867200123456789", "imei extract CGSN=1");
  eq(extractImei(String("AT+GSN=1\r\n+GSN: 867200123456789\r\n\r\nOK\r\n")),
     "867200123456789", "imei extract GSN=1");
  eq(extractImei(String("AT+GSN\r\nOK\r\n")), "", "imei reject no digits");
  eq(extractImei(String("code 123456 then serial 12345678901234")), "",
     "imei reject non-15 digit runs");

  // ---- isValidPhoneNumber ----
  check(isValidPhoneNumber(String("13800138000")), "phone valid 11-digit");
  check(isValidPhoneNumber(String("+8613800138000")), "phone valid +86");
  check(!isValidPhoneNumber(String("12")), "phone too short");
  check(!isValidPhoneNumber(String("138-0013")), "phone reject dash");
  check(!isValidPhoneNumber(String("138a0013000")), "phone reject letter");
  check(!isValidPhoneNumber(String("123456789012345678901")), "phone too long");

  // ---- bodyPreview (脱敏) ----
  eq(bodyPreview(String("482913 is your code"), false), "[19 chars]", "bodyPreview masked");
  eq(bodyPreview(String("482913 is your code"), true), "482913 is your code", "bodyPreview verbose");

  // ---- backoffSeconds ----
  check(backoffSeconds(1, 5, 300, 0) == 5, "backoff attempt1 base");
  check(backoffSeconds(2, 5, 300, 0) == 10, "backoff attempt2 doubled");
  check(backoffSeconds(3, 5, 300, 0) == 20, "backoff attempt3");
  check(backoffSeconds(20, 5, 300, 0) == 300, "backoff capped at max");
  {
    uint32_t v = backoffSeconds(3, 5, 300, 999);  // step=20, jitter 0..5
    check(v >= 20 && v <= 25, "backoff jitter in range");
  }

  // ---- parseATI (ATI 三行解析；modem 初始化与网页诊断共用) ----
  {
    String mfg, model, ver;
    parseATI(String("ATI\r\nMOBILETEK\r\nML307R\r\nV1.0\r\nOK\r\n"), mfg, model, ver);
    eq(mfg, "MOBILETEK", "parseATI manufacturer");
    eq(model, "ML307R", "parseATI model");
    eq(ver, "V1.0", "parseATI version");
    String m2, model2, v2;
    parseATI(String("ATI\r\nx\r\nML307Y\r\ny\r\nOK\r\n"), m2, model2, v2);
    eq(model2, "ML307Y", "parseATI model ML307Y");
    String m3, model3, v3;
    parseATI(String("ATI\r\nMOBILETEK\r\nML307R\r\nV1.0"), m3, model3, v3);
    eq(v3, "V1.0", "parseATI no trailing newline");
  }

  // ---- streamTemplate ----
  check(runTemplate("<b>%IP%</b>") == "<b>192.168.1.5</b>", "tmpl single token");
  check(runTemplate("ssid=%WIFI_SSID% n=%PUSH_COUNT%") == "ssid=home n=3", "tmpl two tokens");
  check(runTemplate("%IP%") == "192.168.1.5", "tmpl token at start");
  // CSS 裸 % 不能被误判为占位符
  check(runTemplate(".x{width:100%;}%IP%") == ".x{width:100%;}192.168.1.5",
        "tmpl CSS bare percent safe");
  // 未知 token 原样保留
  check(runTemplate("a %UNKNOWN_X% b") == "a %UNKNOWN_X% b", "tmpl unknown token verbatim");
  // 相邻 token
  check(runTemplate("%IP%%PUSH_COUNT%") == "192.168.1.53", "tmpl adjacent tokens");

  // ---- evalForwardRules (转发规则引擎) ----
  {
    ForwardDecision d = evalForwardRules(String("kw\t验证码\temail\t1"), String("10086"), String("您的验证码是 482913"));
    check(d.matched && d.email && d.chMask == 0 && !d.drop, "rule kw match -> email only");
    d = evalForwardRules(String("re\t\\d{6}\t1\t1"), String("x"), String("code 482913"));
    check(d.matched && d.chMask == 0x1 && !d.email, "rule re body -> channel1");
    d = evalForwardRules(String("from\t^(10086|10010)\tdrop\t1"), String("10086"), String("ad"));
    check(d.matched && d.drop, "rule from regex -> drop");
    d = evalForwardRules(String("kw\t银行\temail\t1"), String("10086"), String("普通短信"));
    check(!d.matched, "rule no match -> default");
    d = evalForwardRules(String("kw\t验证码\temail\t0"), String("x"), String("验证码 1"));
    check(!d.matched, "rule disabled skipped");
    d = evalForwardRules(String("kw\t验证码\t1\t1\nkw\t验证码\t2\t1"), String("x"), String("验证码"));
    check(d.matched && d.chMask == 0x1, "rule first-match wins");
    d = evalForwardRules(String("kw\tfoo\t1,3,email\t1"), String("x"), String("foo bar"));
    check(d.matched && d.email && d.chMask == 0x5, "rule multi-action mask");
    d = evalForwardRules(String("re\t[unclosed\t1\t1"), String("x"), String("anything"));
    check(!d.matched, "rule invalid regex safe");
  }

  // ---- ruleRegexMatch (轻量正则替代 std::regex；等价性守护) ----
  check(ruleRegexMatch(String("\\d{4,6}"), String("code 1234 end")), "re {4,6} match 4");
  check(!ruleRegexMatch(String("\\d{4,6}"), String("only 12 here")), "re {4,6} too few digits");
  check(ruleRegexMatch(String("\\d{6}"), String("otp 482913")), "re {6} exact");
  check(ruleRegexMatch(String("\\d{4,}"), String("123456789")), "re {n,} open upper");
  check(ruleRegexMatch(String("^(10086|10010)"), String("10086 ad")), "re group-alt a");
  check(ruleRegexMatch(String("^(10086|10010)"), String("10010 ad")), "re group-alt b");
  check(!ruleRegexMatch(String("^(10086|10010)"), String("95588")), "re group-alt none");
  check(!ruleRegexMatch(String("^(10086|10010)"), String("x10086")), "re ^ anchor blocks");
  check(ruleRegexMatch(String("(验证|code)"), String("your code here")), "re cn/en group-alt");
  check(ruleRegexMatch(String("验证码|余额"), String("您的余额为")), "re top-level alt utf8");
  check(ruleRegexMatch(String("^1[3-9]\\d{9}$"), String("13800138000")), "re phone full match");
  check(!ruleRegexMatch(String("^1[3-9]\\d{9}$"), String("12345")), "re phone reject short");
  check(!ruleRegexMatch(String("^1[3-9]\\d{9}$"), String("13800138000x")), "re phone $ anchor");
  check(ruleRegexMatch(String("abc"), String("xxABCyy")), "re icase literal");
  check(ruleRegexMatch(String("a.*z"), String("a__z")), "re dot-star");
  check(ruleRegexMatch(String("colou?r"), String("color")), "re optional absent");
  check(ruleRegexMatch(String("colou?r"), String("colour")), "re optional present");
  check(ruleRegexMatch(String("[^0-9]"), String("a1")), "re negclass letter");
  check(!ruleRegexMatch(String("[^0-9]"), String("123")), "re negclass all-digit");
  check(!ruleRegexMatch(String("(unmatched"), String("no paren here")), "re unmatched paren safe");
  check(!ruleRegexMatch(String("[bad"), String("anything")), "re unclosed class safe");

  std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
  return g_fail == 0 ? 0 : 1;
}
