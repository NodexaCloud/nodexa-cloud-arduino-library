#pragma once

#include <HTTPClient.h>

namespace Nodexa {
namespace detail {

class BoundedBody : public Stream {
 public:
  BoundedBody(String& body, size_t limit) : _body(body), _limit(limit) {}

  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }
  size_t write(uint8_t byte) override { return write(&byte, 1); }
  size_t write(const uint8_t* bytes, size_t length) override {
    if (length > _limit - _body.length() ||
        !_body.concat(reinterpret_cast<const char*>(bytes), length)) {
      setWriteError();
      return 0;
    }
    return length;
  }

 private:
  String& _body;
  size_t _limit;
};

inline bool readHttpBody(HTTPClient& http, String& body, size_t limit) {
  body.remove(0);
  const int declaredSize = http.getSize();
  if (declaredSize == 0) return true;
  if (declaredSize > 0 &&
      (static_cast<size_t>(declaredSize) > limit || !body.reserve(declaredSize))) {
    return false;
  }
  BoundedBody stream(body, limit);
  const int received = http.writeToStream(&stream);
  if (received >= 0 &&
      (declaredSize < 0 || received == declaredSize)) return true;
  body = static_cast<const char*>(nullptr);
  return false;
}

// cJSON uses recursion. Bound depth before it can consume the RTOS task stack.
inline bool jsonWithinDepth(const char* json) {
  constexpr unsigned kMaxDepth = 24;
  unsigned depth = 0;
  bool inString = false;
  bool escaped = false;
  if (!json) return false;
  for (; *json; ++json) {
    const char ch = *json;
    if (inString) {
      if (escaped) {
        escaped = false;
      } else if (ch == '\\') {
        escaped = true;
      } else if (ch == '"') {
        inString = false;
      }
    } else if (ch == '"') {
      inString = true;
    } else if (ch == '[' || ch == '{') {
      if (++depth > kMaxDepth) return false;
    } else if (ch == ']' || ch == '}') {
      if (!depth) return false;
      --depth;
    }
  }
  return !inString && depth == 0;
}

}  // namespace detail
}  // namespace Nodexa
