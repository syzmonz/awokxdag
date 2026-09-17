#pragma once
// Pure bounded parsers shared with host tests. IP integers use network notation
// (192.168.1.2 == 0xc0a80102), independent of the CPU byte order.
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <ctype.h>
#include <stdio.h>

namespace NetworkParse {
inline bool range(uint32_t ip, uint32_t mask, uint32_t& first, uint32_t& last) {
  const uint32_t inv = ~mask;
  if (!mask || (inv & (inv + 1)) || inv < 3) return false;
  first = (ip & mask) + 1;
  last = (ip | inv) - 1;
  return ip >= first && ip <= last;
}
inline bool ipv4(const char* text, uint32_t& ip) {
  ip = 0;
  for (int i = 0; i < 4; ++i) {
    if (!isdigit(static_cast<unsigned char>(*text))) return false;
    unsigned octet = 0, digits = 0;
    while (isdigit(static_cast<unsigned char>(*text))) {
      octet = octet * 10 + (*text++ - '0');
      if (++digits > 3 || octet > 255) return false;
    }
    ip = (ip << 8) | octet;
    if (i < 3 && *text++ != '.') return false;
  }
  return !*text;
}
inline bool copy(const char* begin, size_t n, char* out, size_t cap) {
  if (!cap || n >= cap) return false;
  memcpy(out, begin, n); out[n] = 0; return true;
}
// HTTP/SSDP header names are case insensitive. Never accept an embedded CR/LF.
inline bool header(const char* text, const char* key, char* out, size_t cap) {
  const size_t keyLen = strlen(key);
  const char* line = strstr(text, "\r\n");
  while (line && line[2]) {
    line += 2;
    const char* end = strstr(line, "\r\n");
    if (!end || end == line) break;
    if (size_t(end - line) > keyLen && line[keyLen] == ':' &&
        !strncasecmp(line, key, keyLen)) {
      const char* begin = line + keyLen + 1;
      while (begin < end && (*begin == ' ' || *begin == '\t')) ++begin;
      while (end > begin && (end[-1] == ' ' || end[-1] == '\t')) --end;
      return copy(begin, end - begin, out, cap);
    }
    line = end;
  }
  return false;
}
// Extract a leaf element by local name, accepting namespace prefixes. No XML
// entity expansion, DTDs or recursion; oversized/unclosed values fail closed.
inline bool xml(const char* text, const char* key, char* out, size_t cap) {
  const char* p = text;
  while ((p = strchr(p, '<'))) {
    ++p;
    if (*p == '/' || *p == '!' || *p == '?') continue;
    const char* name = p;
    while (*p && *p != '>' && *p != '/' && !isspace((unsigned char)*p)) ++p;
    const char* local = name;
    for (const char* q = name; q < p; ++q) if (*q == ':') local = q + 1;
    if (size_t(p - local) != strlen(key) || strncmp(local, key, p - local)) continue;
    const char* start = strchr(p, '>');
    if (!start || start[-1] == '/') return false;
    const char* end = strchr(++start, '<');
    if (!end || end[1] != '/') return false;
    const char* close = end + 2;
    if (strncmp(close, name, p - name)) return false;
    close += p - name;
    while (isspace((unsigned char)*close)) ++close;
    if (*close != '>') return false;
    while (start < end && isspace((unsigned char)*start)) ++start;
    while (end > start && isspace((unsigned char)end[-1])) --end;
    return copy(start, end - start, out, cap);
  }
  return false;
}
// Return one container's contents and advance past its matching close tag.
// UPnP service containers do not nest other service containers.
inline bool block(const char*& cursor, const char* key, char* out, size_t cap) {
  const char* p = cursor;
  while ((p = strchr(p, '<'))) {
    const char* name = ++p;
    if (*p == '/' || *p == '!' || *p == '?') continue;
    while (*p && *p != '>' && *p != '/' && !isspace((unsigned char)*p)) ++p;
    const char* local = name;
    for (const char* q = name; q < p; ++q) if (*q == ':') local = q + 1;
    if (size_t(p - local) != strlen(key) || strncmp(local, key, p - local)) continue;
    char closing[96];
    if (size_t(p - name) > sizeof(closing) - 4) return false;
    snprintf(closing, sizeof(closing), "</%.*s>", int(p - name), name);
    const char* start = strchr(p, '>');
    if (!start) return false;
    const char* end = strstr(++start, closing);
    if (!end) return false;
    cursor = end + strlen(closing);
    return copy(start, end - start, out, cap);
  }
  return false;
}
struct Url { uint32_t ip = 0; uint16_t port = 80; char path[256] = {}; };
inline bool url(const char* value, Url& out) {
  if (strncmp(value, "http://", 7)) return false;
  const char* host = value + 7;
  const char* path = strchr(host, '/');
  const char* end = path ? path : host + strlen(host);
  const char* colon = static_cast<const char*>(memchr(host, ':', end - host));
  char address[16];
  if (!copy(host, (colon ? colon : end) - host, address, sizeof(address)) ||
      !ipv4(address, out.ip)) return false;
  out.port = 80;
  if (colon) {
    unsigned port = 0;
    if (++colon == end) return false;
    for (; colon < end; ++colon) {
      if (!isdigit((unsigned char)*colon)) return false;
      port = port * 10 + *colon - '0';
      if (port > 65535) return false;
    }
    if (!port) return false;
    out.port = port;
  }
  const char* suffix = path ? path : "/";
  for (const char* q = suffix; *q; ++q)
    if ((unsigned char)*q < 33 || *q == '#') return false;
  return copy(suffix, strlen(suffix), out.path, sizeof(out.path));
}
// Decode a fully received HTTP message in place, rejecting truncated bodies.
inline bool httpBody(char* data, size_t length, char*& body, size_t& size) {
  char* split = strstr(data, "\r\n\r\n");
  if (!split) return false;
  body = split + 4; size = length - (body - data);
  char value[64];
  if (header(data, "Transfer-Encoding", value, sizeof(value))) {
    if (strcasecmp(value, "chunked")) return false;
    char* src = body; char* dst = body; char* end = data + length;
    while (src < end) {
      char* eol = strstr(src, "\r\n");
      if (!eol || eol >= end || eol == src) return false;
      size_t chunk = 0; const char* p = src;
      for (; p < eol && *p != ';'; ++p) {
        if (!isxdigit((unsigned char)*p) || chunk > length / 16) return false;
        chunk = chunk * 16 + (isdigit((unsigned char)*p) ? *p - '0' : tolower(*p) - 'a' + 10);
      }
      if (p == src) return false;
      src = eol + 2;
      if (chunk == 0) {
        if (src + 2 > end || src[0] != '\r' || src[1] != '\n') return false;
        size = dst - body; *dst = 0; return true;
      }
      if (chunk > size_t(end - src) || size_t(end - src) - chunk < 2 ||
          src[chunk] != '\r' || src[chunk + 1] != '\n') return false;
      memmove(dst, src, chunk); dst += chunk; src += chunk + 2;
    }
    return false;
  }
  if (header(data, "Content-Length", value, sizeof(value))) {
    char* end; unsigned long expected = strtoul(value, &end, 10);
    if (!*value || *end || expected > size) return false;
    size = expected;
  }
  body[size] = 0;
  return true;
}
// UUIDs are exact semicolon-separated values; names/MACs are not identities.
inline const char* flipper(const char* uuids) {
  static const char* const kFull[] = {
      "00003081-0000-1000-8000-00805f9b34fb",
      "00003082-0000-1000-8000-00805f9b34fb",
      "00003083-0000-1000-8000-00805f9b34fb"};
  static const char* const kShort[] = {"3081", "3082", "3083"};
  const char* p = uuids;
  while (*p) {
    const char* end = strchr(p, ';');
    size_t n = end ? size_t(end - p) : strlen(p);
    for (int i = 0; i < 3; ++i) {
      if ((n == 36 && !strncasecmp(p, kFull[i], n)) ||
          (n == 4 && !strncasecmp(p, kShort[i], n)))
        return "Flipper-like service";
    }
    if (!end) break;
    p = end + 1;
  }
  return "";
}
}  // namespace NetworkParse
