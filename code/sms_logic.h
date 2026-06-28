#ifndef SMS_LOGIC_H
#define SMS_LOGIC_H

// 纯逻辑工具集：仅依赖 Arduino String + 标准 C，可在设备端与主机测试两端编译。
// 设备端 String 为 Arduino String；主机测试用 test/arduino_string_shim.h 提供等价子集，
// 并定义 SMS_LOGIC_NO_ARDUINO 以跳过 <Arduino.h>。
// 这里集中放置原先分散在 push.cpp / sms_process.cpp 的可移植逻辑，避免重复实现。
#ifndef SMS_LOGIC_NO_ARDUINO
#include <Arduino.h>
#endif
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

// ---- JSON 字符串转义（含其它控制字符 \u00XX，避免非法 JSON） ----
inline String jsonEscape(const String& str) {
  String result;
  result.reserve(str.length() + 8);
  for (unsigned int i = 0; i < str.length(); i++) {
    char c = str.charAt(i);
    switch (c) {
      case '"':  result += "\\\""; break;
      case '\\': result += "\\\\"; break;
      case '\n': result += "\\n";  break;
      case '\r': result += "\\r";  break;
      case '\t': result += "\\t";  break;
      default:
        if ((unsigned char)c < 0x20) {
          char buf[8];
          snprintf(buf, sizeof(buf), "\\u%04x", (unsigned char)c);
          result += buf;
        } else {
          result += c;
        }
    }
  }
  return result;
}

// ---- URL 编码（空格->+，非字母数字->%XX） ----
inline String urlEncode(const String& str) {
  String encoded;
  encoded.reserve(str.length() * 3);
  for (unsigned int i = 0; i < str.length(); i++) {
    char c = str.charAt(i);
    if (c == ' ') {
      encoded += '+';
    } else if (isalnum((unsigned char)c)) {
      encoded += c;
    } else {
      char lo = c & 0xf;
      char hi = (c >> 4) & 0xf;
      char code1 = lo < 10 ? (lo + '0') : (lo - 10 + 'A');
      char code0 = hi < 10 ? (hi + '0') : (hi - 10 + 'A');
      encoded += '%';
      encoded += code0;
      encoded += code1;
    }
  }
  return encoded;
}

// ---- 号码归一：去掉 +86 国家码前缀，便于黑名单/管理员比较 ----
inline String stripCountryCode(const String& num) {
  if (num.startsWith("+86")) return num.substring(3);
  return num;
}

// ---- 号码合法性校验：可选前导 +，其余全为数字，总长 3..20。
//      用于管理员 SMS 命令的目标号码,拒绝畸形输入。 ----
inline bool isValidPhoneNumber(const String& num) {
  int n = (int)num.length();
  if (n < 3 || n > 20) return false;
  unsigned int i = 0;
  if (num.charAt(0) == '+') {
    if (n < 4) return false;
    i = 1;
  }
  for (; i < num.length(); i++) {
    char c = num.charAt(i);
    if (c < '0' || c > '9') return false;
  }
  return true;
}

// ---- 号码脱敏：保留头尾，中间以 **** 替代（短号码原样返回） ----
inline String maskPhone(const String& phone) {
  int n = phone.length();
  if (n <= 4) return phone;
  int head = n >= 8 ? 3 : 1;
  int tail = n >= 8 ? 4 : 2;
  if (head + tail >= n) { head = n / 3; tail = n / 3; }
  String out;
  out.reserve(head + tail + 4);
  for (int i = 0; i < head; i++) out += phone.charAt(i);
  out += "****";
  for (int i = n - tail; i < n; i++) out += phone.charAt(i);
  return out;
}

// ---- 黑名单匹配：list 为换行分隔，支持 +86 归一（双向去国家码比较） ----
inline bool numberMatchesBlacklist(const String& list, const String& sender) {
  if (list.length() == 0) return false;
  String s1 = sender;
  String s2 = stripCountryCode(sender);
  int len = (int)list.length();
  int start = 0;
  while (start <= len) {
    int end = list.indexOf('\n', start);
    if (end == -1) end = len;
    String line = list.substring(start, end);
    line.trim();
    if (line.length() > 0 &&
        (line == s1 || line == s2 || stripCountryCode(line) == s2)) {
      return true;
    }
    start = end + 1;
  }
  return false;
}

// ---- FNV-1a 32 位哈希（短信去重幂等标识） ----
inline uint32_t fnv1a32(const String& s) {
  uint32_t h = 2166136261u;
  for (unsigned int i = 0; i < s.length(); i++) {
    h ^= (unsigned char)s.charAt(i);
    h *= 16777619u;
  }
  return h;
}

// ---- 绝对时间是否有效（Unix 秒，>= 2023-11，过滤未对时的垃圾 RTC 值） ----
inline bool epochIsValid(uint32_t epoch) { return epoch >= 1700000000u; }

// ---- 保号是否到期：now/last 为 Unix 秒。仅在 now 有效时判断；
//      无有效基准则视为到期（触发一次以建立基准）。 ----
inline bool keepAliveDue(uint32_t lastTs, uint32_t now, uint32_t intervalDays) {
  if (!epochIsValid(now)) return false;
  if (intervalDays == 0) return false;
  if (!epochIsValid(lastTs)) return true;
  return (now - lastTs) >= intervalDays * 86400u;
}

// ---- 验证码提取：返回首段 4-8 位独立数字串，无则空 ----
inline String extractOtp(const String& text) {
  int n = (int)text.length();
  int i = 0;
  while (i < n) {
    if (isdigit((unsigned char)text.charAt(i))) {
      int j = i;
      while (j < n && isdigit((unsigned char)text.charAt(j))) j++;
      int len = j - i;
      if (len >= 4 && len <= 8) return text.substring(i, j);
      i = j;
    } else {
      i++;
    }
  }
  return String();
}

// ---- IMEI 提取：从 AT 响应中取首段 15 位连续数字，跳过命令回显/OK/ERROR 等杂项。 ----
inline String extractImei(const String& text) {
  int n = (int)text.length();
  int i = 0;
  while (i < n) {
    if (isdigit((unsigned char)text.charAt(i))) {
      int j = i;
      while (j < n && isdigit((unsigned char)text.charAt(j))) j++;
      if (j - i == 15) return text.substring(i, j);
      i = j;
    } else {
      i++;
    }
  }
  return String();
}

// ---- HTML 文本转义（防存储型 XSS：短信内容在网页展示前转义） ----
inline String htmlEscape(const String& str) {
  String out;
  out.reserve(str.length() + 8);
  for (unsigned int i = 0; i < str.length(); i++) {
    char c = str.charAt(i);
    switch (c) {
      case '&': out += "&amp;";  break;
      case '<': out += "&lt;";   break;
      case '>': out += "&gt;";   break;
      case '"': out += "&quot;"; break;
      case '\'': out += "&#39;"; break;
      default: out += c;
    }
  }
  return out;
}

// ---- 短信正文日志预览：verbose 时原样；否则只回长度，避免完整正文(含验证码)留存于
//      网页可见的日志环形缓冲。满足"敏感短信正文默认不入普通日志"。 ----
inline String bodyPreview(const String& text, bool verbose) {
  if (verbose) return text;
  String out = "[";
  out += String(text.length());
  out += " chars]";
  return out;
}

// ---- 解析 ATI 响应：前三条"非空且非回显/非OK"的行依次为 厂商/型号/固件版本 ----
//      modem.cpp 初始化与网页诊断查询共用，行为与原内联解析一致。 ----
inline void parseATI(const String& resp, String& mfg, String& model, String& ver) {
  int lineStart = 0, lineNum = 0, n = (int)resp.length();
  for (int i = 0; i < n; i++) {
    if (resp.charAt(i) == '\n' || i == n - 1) {
      String line = resp.substring(lineStart, i);
      line.trim();
      if (line.length() > 0 && line != "ATI" && line != "OK") {
        lineNum++;
        if (lineNum == 1) mfg = line;
        else if (lineNum == 2) model = line;
        else if (lineNum == 3) ver = line;
      }
      lineStart = i + 1;
    }
  }
}

// ---- 指数退避（含确定性抖动）：返回第 attempt 次重试的延迟秒数 ----
// base * 2^(attempt-1)，封顶 maxSec；jitterSeed 派生 0..(step/4) 抖动。
inline uint32_t backoffSeconds(uint32_t attempt, uint32_t baseSec,
                               uint32_t maxSec, uint32_t jitterSeed) {
  if (attempt == 0) attempt = 1;
  uint32_t step = baseSec;
  for (uint32_t i = 1; i < attempt && step < maxSec; i++) {
    step <<= 1;
  }
  if (step > maxSec) step = maxSec;
  uint32_t jitter = step / 4;
  uint32_t add = jitter ? (jitterSeed % (jitter + 1)) : 0;
  return step + add;
}

// ---- 模板流式扫描：识别 %[A-Z0-9_]{2,}% 占位符，安全跳过 CSS 中的裸 % ----
// emit(ptr,len): 输出静态或动态片段；resolve(name,out): 已知占位符填 out 并返回 true。
// 未知 token 原样保留（其首个 % 进入静态片段）。
template <typename EmitFn, typename ResolveFn>
inline void streamTemplate(const char* tpl, EmitFn emit, ResolveFn resolve) {
  const char* seg = tpl;
  const char* p = tpl;
  while (*p) {
    if (*p == '%') {
      const char* q = p + 1;
      while (*q && ((*q >= 'A' && *q <= 'Z') || (*q >= '0' && *q <= '9') || *q == '_')) q++;
      size_t nl = (size_t)(q - (p + 1));
      if (nl >= 2 && nl < 40 && *q == '%') {
        char nbuf[40];
        memcpy(nbuf, p + 1, nl);
        nbuf[nl] = 0;
        String name(nbuf);
        String val;
        if (resolve(name, val)) {
          if (p > seg) emit(seg, (size_t)(p - seg));
          emit(val.c_str(), (size_t)val.length());
          p = q + 1;
          seg = p;
          continue;
        }
      }
    }
    p++;
  }
  if (p > seg) emit(seg, (size_t)(p - seg));
}

// ---- 转发规则引擎(正则)：决定一条短信发往哪些通道 / 丢弃 ----
// 规则按行，每行 4 段以 \t 分隔：  type \t pattern \t action \t enabled
//   type   : kw(正文子串) | re(正文正则) | from(发件人正则)
//   pattern: 匹配文本
//   action : 逗号分隔，元素 ∈ { email, 1..5(通道,1-based), drop }
//   enabled: 1 启用 / 0 停用
// 自上而下，第一条命中即返回；无命中 matched=false(调用方走默认=全部启用通道)。
struct ForwardDecision {
  bool matched;     // 是否命中某条规则
  bool drop;        // 命中且动作含 drop
  unsigned chMask;  // 命中时要发的通道位掩码(bit i = 通道 i, 0-based)
  bool email;       // 命中时是否发邮件
};

// ---- 轻量正则匹配(替代 std::regex，省 ~267KB flash) ----
// 支持子集: . ^ $ * + ? {n} {n,} {n,m}  \d \D \w \W \s \S  [..] [^..] [a-z]  顶层 |(交替)  大小写不敏感
// 不支持: 分组 ()、组内交替、反向引用、前后瞻。组内交替请用顶层 | 改写(如 (10086|10010) → ^10086|^10010)。
// ponytail: 自写紧凑回溯匹配——现成轻量库多缺 {n,m} 或 |，而 std::regex 太占 flash。仅按收到的短信
//   偶发调用、模式/文本短(<=160 字)，回溯足够；steps 上限防极端模式爆栈。等价性由 test/run_tests.cpp 守。
inline bool reClass(char cls, char c) {
  bool d = (c >= '0' && c <= '9');
  bool w = d || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
  bool s = (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v');
  switch (cls) {
    case 'd': return d;   case 'D': return !d;
    case 'w': return w;   case 'W': return !w;
    case 's': return s;   case 'S': return !s;
    default:  return false;
  }
}
inline char reLower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

// 原子(单个可量化单元)在 pattern[pi..] 占的字符数
inline int reAtomLen(const String& p, int pi) {
  int n = (int)p.length();
  if (pi >= n) return 0;
  char c = p.charAt(pi);
  if (c == '\\') return (pi + 1 < n) ? 2 : 1;
  if (c == '[') {
    int j = pi + 1;
    if (j < n && p.charAt(j) == '^') j++;
    if (j < n && p.charAt(j) == ']') j++;   // 紧跟的 ] 视为字面
    while (j < n && p.charAt(j) != ']') j++;
    return (j < n) ? (j - pi + 1) : -1;   // 未闭合 [ 视为非法
  }
  return 1;
}

// 单字符 c 是否匹配 pattern[pi..pi+alen) 这个原子(大小写不敏感)
inline bool reAtomMatches(const String& p, int pi, int alen, char c) {
  char pc = p.charAt(pi);
  if (pc == '.') return true;
  if (pc == '\\') {
    char nx = p.charAt(pi + 1);
    if (nx=='d'||nx=='D'||nx=='w'||nx=='W'||nx=='s'||nx=='S') return reClass(nx, c);
    return reLower(nx) == reLower(c);       // 转义字面，如 \. \\ \+
  }
  if (pc == '[') {
    int end = pi + alen - 1;                // 指向闭合 ]
    int j = pi + 1;
    bool neg = false;
    if (j < end && p.charAt(j) == '^') { neg = true; j++; }
    bool in = false;
    while (j < end) {
      char a = p.charAt(j);
      if (a == '\\' && j + 1 < end) {
        char nx = p.charAt(j + 1);
        if (nx=='d'||nx=='D'||nx=='w'||nx=='W'||nx=='s'||nx=='S') { if (reClass(nx, c)) in = true; }
        else if (reLower(nx) == reLower(c)) in = true;
        j += 2; continue;
      }
      if (j + 2 < end && p.charAt(j + 1) == '-') {   // 范围 a-z
        if (reLower(c) >= reLower(a) && reLower(c) <= reLower(p.charAt(j + 2))) in = true;
        j += 3; continue;
      }
      if (reLower(a) == reLower(c)) in = true;
      j++;
    }
    return neg ? !in : in;
  }
  return reLower(pc) == reLower(c);          // 普通字面
}

inline bool reMatchHere(const String& p, int pi, const String& s, int si, int& steps);

// 贪婪匹配原子 minN..maxN 次后回溯尝试剩余模式
inline bool reMatchQuant(const String& p, int pi, int alen, int nextPi,
                         const String& s, int si, int minN, int maxN, int& steps) {
  int count = 0;
  while (count < maxN && si + count < (int)s.length() &&
         reAtomMatches(p, pi, alen, s.charAt(si + count))) count++;
  while (count >= minN) {
    if (reMatchHere(p, nextPi, s, si + count, steps)) return true;
    if (count == 0) break;
    count--;
  }
  return false;
}

inline bool reMatchHere(const String& p, int pi, const String& s, int si, int& steps) {
  if (++steps > 20000) return false;         // 防极端回溯爆栈
  int plen = (int)p.length();
  if (pi >= plen) return true;               // 模式耗尽 = 命中
  if (p.charAt(pi) == '$' && pi + 1 == plen) return si == (int)s.length();
  int alen = reAtomLen(p, pi);
  if (alen <= 0) return false;            // 非法原子(如未闭合 [)：整体不匹配
  int qi = pi + alen;
  char q = (qi < plen) ? p.charAt(qi) : 0;
  if (q == '*' || q == '+' || q == '?') {
    int minN = (q == '+') ? 1 : 0;
    int maxN = (q == '?') ? 1 : 1000000;
    return reMatchQuant(p, pi, alen, qi + 1, s, si, minN, maxN, steps);
  }
  if (q == '{') {
    int close = p.indexOf('}', qi);
    if (close > qi) {
      String spec = p.substring(qi + 1, close);
      int comma = spec.indexOf(',');
      int minN, maxN;
      if (comma < 0) { minN = maxN = spec.toInt(); }
      else {
        minN = spec.substring(0, comma).toInt();
        String hi = spec.substring(comma + 1); hi.trim();
        maxN = hi.length() ? hi.toInt() : 1000000;
      }
      return reMatchQuant(p, pi, alen, close + 1, s, si, minN, maxN, steps);
    }
  }
  if (si < (int)s.length() && reAtomMatches(p, pi, alen, s.charAt(si)))
    return reMatchHere(p, qi, s, si + 1, steps);
  return false;
}

// 在 text 中搜索单个(无顶层|)子模式；^ 锚定开头
inline bool reSearchOne(const String& p, const String& s) {
  int steps = 0;
  if (p.length() && p.charAt(0) == '^') return reMatchHere(p, 1, s, 0, steps);
  for (int i = 0; i <= (int)s.length(); i++) {
    steps = 0;
    if (reMatchHere(p, 0, s, i, steps)) return true;
  }
  return false;
}

// 与 open 处 '(' 配对的 ')'（支持嵌套；跳过转义与 []）；无则 -1
inline int reFindGroupClose(const String& p, int open) {
  int n = (int)p.length(), depth = 0;
  for (int i = open; i < n; i++) {
    char c = p.charAt(i);
    if (c == '\\') { i++; continue; }
    if (c == '[') { i++; if (i<n&&p.charAt(i)=='^') i++; if (i<n&&p.charAt(i)==']') i++; while (i<n&&p.charAt(i)!=']') i++; continue; }
    if (c == '(') depth++;
    else if (c == ')') { if (--depth == 0) return i; }
  }
  return -1;
}
// s 从 from 起第一个"顶层" |（跳过 \x 转义、[..] 字符集、(..) 分组）；无则 -1
inline int reTopBar(const String& s, int from) {
  int n = (int)s.length();
  for (int i = from; i < n; i++) {
    char c = s.charAt(i);
    if (c == '\\') { i++; continue; }
    if (c == '[') { i++; if (i<n&&s.charAt(i)=='^') i++; if (i<n&&s.charAt(i)==']') i++; while (i<n&&s.charAt(i)!=']') i++; continue; }
    if (c == '(') { int cl = reFindGroupClose(s, i); if (cl < 0) return -1; i = cl; continue; }
    if (c == '|') return i;
  }
  return -1;
}
// 第一个有效 '('（非 \( 、非 [] 内）；无则 -1
inline int reFindGroupOpen(const String& p) {
  int n = (int)p.length();
  for (int i = 0; i < n; i++) {
    char c = p.charAt(i);
    if (c == '\\') { i++; continue; }
    if (c == '[') { i++; if (i<n&&p.charAt(i)=='^') i++; if (i<n&&p.charAt(i)==']') i++; while (i<n&&p.charAt(i)!=']') i++; continue; }
    if (c == '(') return i;
  }
  return -1;
}
// 顶层 | 拆分(子模式无分组)，任一支 reSearchOne 命中即匹配
inline bool matchTopAlt(const String& p, const String& s) {
  int start = 0, n = (int)p.length();
  while (start <= n) {
    int bar = reTopBar(p, start);
    String sub = (bar < 0) ? p.substring(start) : p.substring(start, bar);
    if (sub.length() && reSearchOne(sub, s)) return true;
    if (bar < 0) break;
    start = bar + 1;
  }
  return false;
}
// 转发规则正则匹配(接口不变)：先把 (a|b) 分组按候选 BFS 展开为无括号子模式(上限防组合爆炸)，
// 再对每个候选做顶层 | + 回溯匹配。非法/超限模式安全不命中，绝不崩溃。
inline bool ruleRegexMatch(const String& pattern, const String& text) {
  String queue[64];
  int qn = 0, head = 0;
  queue[qn++] = pattern;
  while (head < qn) {
    String pat = queue[head++];
    int open = reFindGroupOpen(pat);
    if (open < 0) { if (matchTopAlt(pat, text)) return true; continue; }
    int close = reFindGroupClose(pat, open);
    if (close < 0) { if (matchTopAlt(pat, text)) return true; continue; }  // '(' 不配对：当普通字符
    String pre = pat.substring(0, open);
    String body = pat.substring(open + 1, close);
    String post = pat.substring(close + 1);
    int bs = 0, bn = (int)body.length();
    while (bs <= bn) {                            // body 按顶层 | 拆成候选，pre+opt+post 入队
      int bar = reTopBar(body, bs);
      String opt = (bar < 0) ? body.substring(bs) : body.substring(bs, bar);
      if (qn < 64) { String cand = pre; cand += opt; cand += post; queue[qn++] = cand; }
      if (bar < 0) break;
      bs = bar + 1;
    }
  }
  return false;
}

inline ForwardDecision evalForwardRules(const String& rules, const String& sender, const String& body) {
  ForwardDecision d; d.matched = false; d.drop = false; d.chMask = 0; d.email = false;
  int start = 0, n = (int)rules.length();
  while (start < n) {
    int nl = rules.indexOf('\n', start);
    int end = (nl < 0) ? n : nl;
    String line = rules.substring(start, end);
    start = (nl < 0) ? n : nl + 1;
    line.trim();
    if (line.length() == 0) continue;
    int t1 = line.indexOf('\t'); if (t1 < 0) continue;
    int t2 = line.indexOf('\t', t1 + 1); if (t2 < 0) continue;
    int t3 = line.indexOf('\t', t2 + 1);
    String type = line.substring(0, t1);
    String pat  = line.substring(t1 + 1, t2);
    String action = (t3 < 0) ? line.substring(t2 + 1) : line.substring(t2 + 1, t3);
    String en   = (t3 < 0) ? String("1") : line.substring(t3 + 1);
    en.trim();
    if (en == "0") continue;            // 停用
    if (pat.length() == 0) continue;
    bool hit = false;
    if (type == "kw")        hit = body.indexOf(pat) >= 0;
    else if (type == "re")   hit = ruleRegexMatch(pat, body);
    else if (type == "from") hit = ruleRegexMatch(pat, sender);
    if (!hit) continue;
    d.matched = true;
    int as = 0, an = (int)action.length();
    while (as < an) {
      int comma = action.indexOf(',', as);
      int ae = (comma < 0) ? an : comma;
      String tok = action.substring(as, ae); tok.trim();
      as = (comma < 0) ? an : comma + 1;
      if (tok == "drop")       d.drop = true;
      else if (tok == "email") d.email = true;
      else { long ch = tok.toInt(); if (ch >= 1 && ch <= 5) d.chMask |= (1u << (ch - 1)); }  // 通道 1..5 = MAX_PUSH_CHANNELS
    }
    return d;
  }
  return d;
}

#endif  // SMS_LOGIC_H
