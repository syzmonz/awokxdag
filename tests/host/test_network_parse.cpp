// Host tests for AWOKxDAG/network_parse.h. Compile with ASan/UBSan via run.sh.
#include "network_parse.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

static int failures = 0;

#define CHECK(cond)                                                            \
  do {                                                                         \
    if (!(cond)) {                                                             \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
      ++failures;                                                              \
    }                                                                          \
  } while (0)

#define CHECK_EQ(a, b) CHECK((a) == (b))

static void test_ipv4() {
  uint32_t ip = 0;
  CHECK(NetworkParse::ipv4("192.168.1.2", ip) && ip == 0xc0a80102u);
  CHECK(NetworkParse::ipv4("0.0.0.0", ip) && ip == 0);
  CHECK(NetworkParse::ipv4("255.255.255.255", ip) && ip == 0xffffffffu);
  CHECK(!NetworkParse::ipv4("", ip));
  CHECK(!NetworkParse::ipv4("192.168.1", ip));
  CHECK(!NetworkParse::ipv4("192.168.1.2.3", ip));
  CHECK(!NetworkParse::ipv4("192.168.1.256", ip));
  CHECK(!NetworkParse::ipv4("192.168.1.2a", ip));
  CHECK(!NetworkParse::ipv4("192.168.-1.2", ip));
  CHECK(!NetworkParse::ipv4(" 192.168.1.2", ip));
}

static void test_range() {
  uint32_t first = 0, last = 0;
  CHECK(NetworkParse::range(0xc0a80105u, 0xffffff00u, first, last));
  CHECK_EQ(first, 0xc0a80101u);
  CHECK_EQ(last, 0xc0a801feu);
  CHECK(NetworkParse::range(0xc0a80102u, 0xfffffffcu, first, last));  // /30
  CHECK_EQ(first, 0xc0a80101u);
  CHECK_EQ(last, 0xc0a80102u);
  CHECK(!NetworkParse::range(0xc0a80101u, 0xfffffffeu, first, last));  // /31
  CHECK(!NetworkParse::range(0xc0a80101u, 0xffffffffu, first, last));  // /32
  CHECK(!NetworkParse::range(0xc0a80101u, 0, first, last));
  CHECK(!NetworkParse::range(0xc0a80101u, 0xffffff01u, first, last));  // hole
  CHECK(!NetworkParse::range(0xc0a80100u, 0xffffff00u, first, last));  // network
  CHECK(!NetworkParse::range(0xc0a801ffu, 0xffffff00u, first, last));  // broadcast
}

static void test_header() {
  char out[64];
  const char* http =
      "HTTP/1.1 200 OK\r\n"
      "Content-Type: text/xml\r\n"
      "LOCATION: http://192.168.1.1:80/rootDesc.xml\r\n"
      "\r\nbody";
  CHECK(NetworkParse::header(http, "LOCATION", out, sizeof(out)));
  CHECK(std::strcmp(out, "http://192.168.1.1:80/rootDesc.xml") == 0);
  CHECK(NetworkParse::header(http, "content-type", out, sizeof(out)));
  CHECK(std::strcmp(out, "text/xml") == 0);
  CHECK(!NetworkParse::header(http, "Missing", out, sizeof(out)));
  CHECK(!NetworkParse::header("no-crlf", "Host", out, sizeof(out)));
  const char* injected = "HTTP/1.1 200 OK\r\nX-Foo: a\r\nX-Bar: b\r\n\r\n";
  CHECK(NetworkParse::header(injected, "X-Bar", out, sizeof(out)));
  CHECK(std::strcmp(out, "b") == 0);
}

static void test_xml() {
  char out[64];
  const char* xml =
      "<root><s:Manufacturer>Acme</s:Manufacturer>"
      "<Model>Cam1</Model></root>";
  CHECK(NetworkParse::xml(xml, "Manufacturer", out, sizeof(out)));
  CHECK(std::strcmp(out, "Acme") == 0);
  CHECK(NetworkParse::xml(xml, "Model", out, sizeof(out)));
  CHECK(std::strcmp(out, "Cam1") == 0);
  CHECK(!NetworkParse::xml(xml, "Missing", out, sizeof(out)));
  CHECK(!NetworkParse::xml("<x/>", "x", out, sizeof(out)));
  CHECK(!NetworkParse::xml("<x>unclosed", "x", out, sizeof(out)));
  CHECK(!NetworkParse::xml("<x>oops</y>", "x", out, sizeof(out)));
}

static void test_block() {
  const char* body =
      "<serviceList>"
      "<service><serviceType>urn:schemas-upnp-org:service:WANIPConnection:1"
      "</serviceType><controlURL>/ctl</controlURL></service>"
      "</serviceList>";
  const char* cursor = body;
  char section[512];
  CHECK(NetworkParse::block(cursor, "service", section, sizeof(section)));
  char value[80];
  CHECK(NetworkParse::xml(section, "serviceType", value, sizeof(value)));
  CHECK(std::strstr(value, "WANIPConnection:1") != nullptr);
  CHECK(!NetworkParse::block(cursor, "service", section, sizeof(section)));
}

static void test_url() {
  NetworkParse::Url url;
  CHECK(NetworkParse::url("http://192.168.1.1/rootDesc.xml", url));
  CHECK_EQ(url.ip, 0xc0a80101u);
  CHECK_EQ(url.port, 80);
  CHECK(std::strcmp(url.path, "/rootDesc.xml") == 0);
  CHECK(NetworkParse::url("http://10.0.0.1:8080/foo", url));
  CHECK_EQ(url.port, 8080);
  CHECK(!NetworkParse::url("https://192.168.1.1/", url));
  CHECK(!NetworkParse::url("http://example.com/", url));
  CHECK(!NetworkParse::url("http://192.168.1.1:0/", url));
  CHECK(!NetworkParse::url("http://192.168.1.1:65536/", url));
  CHECK(!NetworkParse::url("javascript:alert(1)", url));
  CHECK(!NetworkParse::url("http://192.168.1.1/foo#frag", url));
}

static void test_http_body_length() {
  char data[128];
  const char* msg = "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhelloXXXX";
  std::memcpy(data, msg, std::strlen(msg) + 1);
  char* body = nullptr;
  size_t size = 0;
  CHECK(NetworkParse::httpBody(data, std::strlen(msg), body, size));
  CHECK_EQ(size, 5u);
  CHECK(std::strcmp(body, "hello") == 0);
}

static void test_http_body_chunked() {
  char data[256];
  const char* msg =
      "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
      "5\r\nhello\r\n0\r\n\r\n";
  std::memcpy(data, msg, std::strlen(msg) + 1);
  char* body = nullptr;
  size_t size = 0;
  CHECK(NetworkParse::httpBody(data, std::strlen(msg), body, size));
  CHECK_EQ(size, 5u);
  CHECK(std::strcmp(body, "hello") == 0);
}

static void test_http_body_rejects() {
  char data[128];
  const char* truncated = "HTTP/1.1 200 OK\r\nContent-Length: 20\r\n\r\nshort";
  std::memcpy(data, truncated, std::strlen(truncated) + 1);
  char* body = nullptr;
  size_t size = 0;
  CHECK(!NetworkParse::httpBody(data, std::strlen(truncated), body, size));

  const char* gzip = "HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip\r\n\r\nxxxx";
  std::memcpy(data, gzip, std::strlen(gzip) + 1);
  CHECK(!NetworkParse::httpBody(data, std::strlen(gzip), body, size));

  const char* bad_chunk =
      "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
      "5\r\nhel";
  char small[64];
  std::memcpy(small, bad_chunk, std::strlen(bad_chunk) + 1);
  CHECK(!NetworkParse::httpBody(small, std::strlen(bad_chunk), body, size));
}

static void test_flipper() {
  CHECK(std::strcmp(NetworkParse::flipper("3081"), "Flipper-like service") == 0);
  CHECK(std::strcmp(NetworkParse::flipper("3082"), "Flipper-like service") == 0);
  CHECK(std::strcmp(NetworkParse::flipper("3083"), "Flipper-like service") == 0);
  CHECK(std::strcmp(NetworkParse::flipper(
                        "00003081-0000-1000-8000-00805f9b34fb"),
                    "Flipper-like service") == 0);
  CHECK(std::strcmp(NetworkParse::flipper(
                        "180f;3081;1800"),
                    "Flipper-like service") == 0);
  CHECK(std::strcmp(NetworkParse::flipper("180f"), "") == 0);
  CHECK(std::strcmp(NetworkParse::flipper(""), "") == 0);
  CHECK(std::strcmp(NetworkParse::flipper("3084"), "") == 0);
}

static void test_copy_bounds() {
  char out[4];
  CHECK(!NetworkParse::copy("abcd", 4, out, sizeof(out)));
  CHECK(NetworkParse::copy("abc", 3, out, sizeof(out)));
  CHECK(std::strcmp(out, "abc") == 0);
}

int main() {
  test_ipv4();
  test_range();
  test_header();
  test_xml();
  test_block();
  test_url();
  test_http_body_length();
  test_http_body_chunked();
  test_http_body_rejects();
  test_flipper();
  test_copy_bounds();
  if (failures) {
    std::fprintf(stderr, "%d check(s) failed\n", failures);
    return 1;
  }
  std::puts("test_network_parse: ok");
  return 0;
}
