#ifndef ARDUINO_STRING_SHIM_H
#define ARDUINO_STRING_SHIM_H

// 主机测试用的 Arduino String 最小等价实现（仅覆盖 sms_logic.h 所用 API）。
// 仅用于 g++ 单元测试，绝不参与设备固件编译。
#include <string>
#include <cstring>
#include <cctype>

class String {
 public:
  std::string s;
  String() {}
  String(const char* c) : s(c ? c : "") {}
  String(const std::string& o) : s(o) {}
  String(char c) : s(1, c) {}
  String(int v) : s(std::to_string(v)) {}
  String(unsigned int v) : s(std::to_string(v)) {}
  String(long v) : s(std::to_string(v)) {}
  String(unsigned long v) : s(std::to_string(v)) {}

  unsigned int length() const { return (unsigned int)s.size(); }
  char charAt(unsigned int i) const { return i < s.size() ? s[i] : 0; }
  const char* c_str() const { return s.c_str(); }
  void reserve(unsigned int n) { s.reserve(n); }

  String& operator+=(const char* c) { s += c; return *this; }
  String& operator+=(char c) { s += c; return *this; }
  String& operator+=(const String& o) { s += o.s; return *this; }

  bool operator==(const String& o) const { return s == o.s; }
  bool operator==(const char* c) const { return s == (c ? c : ""); }
  bool operator!=(const String& o) const { return s != o.s; }

  bool startsWith(const char* pre) const {
    size_t n = std::strlen(pre);
    return s.size() >= n && s.compare(0, n, pre) == 0;
  }

  String substring(unsigned int from) const {
    if (from >= s.size()) return String();
    return String(s.substr(from));
  }
  String substring(unsigned int from, unsigned int to) const {
    if (from >= s.size() || to <= from) return String();
    if (to > s.size()) to = (unsigned int)s.size();
    return String(s.substr(from, to - from));
  }

  int indexOf(char ch) const {
    size_t p = s.find(ch);
    return p == std::string::npos ? -1 : (int)p;
  }
  int indexOf(char ch, unsigned int from) const {
    size_t p = s.find(ch, from);
    return p == std::string::npos ? -1 : (int)p;
  }

  void trim() {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) a++;
    while (b > a && std::isspace((unsigned char)s[b - 1])) b--;
    s = s.substr(a, b - a);
  }

  int indexOf(const char* sub) const {
    size_t p = s.find(sub);
    return p == std::string::npos ? -1 : (int)p;
  }
  int indexOf(const String& sub) const { return indexOf(sub.s.c_str()); }
  long toInt() const { try { return std::stol(s); } catch (...) { return 0; } }
};

#endif  // ARDUINO_STRING_SHIM_H
