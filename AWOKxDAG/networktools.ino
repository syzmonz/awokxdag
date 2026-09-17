// AWOKxDAG Network Tools. Original integration inspired by Evil-M5Project's
// scanHosts, scanPorts, CCTV, printer, SIP OPTIONS and UPnP mapping tools by
// 7h30th3r0n3. See THIRD_PARTY_NOTICES.md. No upstream scanner body is copied.
// One in-flight probe; nonblocking sockets; bounded tables and responses.

constexpr int kNetCapacity = kResultCapacity;
constexpr uint16_t kNetSipLocalPort = 49160;
constexpr size_t kNetResponseBytes = 8192;
constexpr uint32_t kNetConnectMs = 450;
constexpr uint32_t kNetResponseMs = 2500;
const uint16_t kNetPorts[] = {21,22,23,25,53,80,110,139,143,443,445,554,631,1883,3306,3389,8080,8443,9100};
const uint16_t kNetCameraPorts[] = {554,8554,80,8000,8080,8899};
const uint16_t kNetPrinterPorts[] = {9100,631,515};
const char* const kNetMenu[] = {"Connect / Wi-Fi", "Discover Hosts", "TCP Ports", "LAN Cameras", "Printers", "SIP Services", "UPnP Mappings", "Last Results"};
NetHost* netHosts = nullptr;
NetResult* netResults = nullptr;
char* netResponse = nullptr;
bool netOpen = false;
NetJob netJob = NetJob::None;
NetJob netLastJob = NetJob::None;
NetStage netStage = NetStage::Idle;
String netSsid, netPassword, netEdit, netStatus;
bool netEditPassword = false;
int netKeyPage = 0, netMenuPage = 0, netPage = 0, netApPage = 0;
int netHostCount = 0, netResultCount = 0, netSelectedHost = -1, netSelectedResult = 0;
bool netShowHosts = true, netLimited = false, netSubnetLimited = false;
uint32_t netFirst = 0, netLast = 0, netCursor = 0, netDeadline = 0, netLastDraw = 0;
uint32_t netCompleted = 0, netTimeouts = 0, netRefused = 0, netErrors = 0;
uint32_t netSessionIp = 0, netSessionMask = 0;
int netHostCursor = 0, netHostEnd = 0, netPortCursor = 0;
int netSocket = -1;
String netRequest;
size_t netSent = 0, netReceived = 0;
WiFiUDP netUdp;
String netSipId;
int netUpnpStep = 0, netMappingIndex = 0;
NetworkParse::Url netUpnpUrl;
String netUpnpService;
NetSummary netHostSummary, netResultSummary;
bool netHostsLimited = false;
void netSnapshot() {
  NetSummary& summary = netLastJob == NetJob::Hosts ? netHostSummary : netResultSummary;
  summary.job = netLastJob;
  snprintf(summary.status, sizeof(summary.status), "%s", netStatus.c_str());
  summary.checked = netCompleted; summary.timeouts = netTimeouts;
  summary.refused = netRefused; summary.errors = netErrors;
  summary.limited = netLimited; summary.subnetLimited = netSubnetLimited;
  summary.hostsLimited = netHostsLimited;
}


bool networkToolsOpen() { return netOpen; }
uint32_t netIpNumber(IPAddress ip) {
  return (uint32_t(ip[0]) << 24) | (uint32_t(ip[1]) << 16) | (uint32_t(ip[2]) << 8) | ip[3];
}
IPAddress netIpAddress(uint32_t ip) { return IPAddress(ip >> 24, ip >> 16, ip >> 8, ip); }
String netIpText(uint32_t ip) { return netIpAddress(ip).toString(); }
bool netExpired() { return int32_t(millis() - netDeadline) >= 0; }
void netWipe(String& value) {
  for (size_t i = 0; i < value.length(); ++i) value.setCharAt(i, 0);
  value = "";
}
const char* netJobName(NetJob job) {
  switch (job) {
    case NetJob::Join: return "Connect";
    case NetJob::Hosts: return "Hosts";
    case NetJob::Ports: return "TCP ports";
    case NetJob::Cameras: return "Camera services";
    case NetJob::Printers: return "Printer candidates";
    case NetJob::Sip: return "SIP services";
    case NetJob::Upnp: return "UPnP mappings";
    default: return "Network Tools";
  }
}
void netCloseSocket() {
  if (netSocket >= 0) { close(netSocket); netSocket = -1; }
  netRequest = ""; netReceived = netSent = 0;
}
void closeNetworkTools() {
  if (!netOpen) return;
  if (netJob == NetJob::Join)
    recordFirmwareAudit("network", "connect", "cancelled", "leaving Network Tools");
  else if (netJob != NetJob::None)
    netFinish("Cancelled (partial)");
  netCloseSocket(); netUdp.stop();
  netJob = NetJob::None; netStage = NetStage::Idle;
  WiFi.setAutoReconnect(false);
  shutdownWifi();
  netWipe(netPassword); netWipe(netEdit); netSsid = "";
  free(netHosts); free(netResults); free(netResponse);
  netHosts = nullptr; netResults = nullptr; netResponse = nullptr;
  netOpen = false;
}
// Preserve readable large labels when they fit; long IP/service rows and
// connection controls use the small font instead of overflowing Touch buttons.
void netButton(int x, int y, int w, int h, const String& label) {
  if (label.length() * 12 <= size_t(w - 12)) drawButton(x, y, w, h, label);
  else drawSmallButton(x, y, w, h, clipped(label, (w - 12) / 6), kAccent);
}
void netFooter(const char* left, const char* right) {
  netButton(6, 284, 106, 30, left);
  netButton(128, 284, 106, 30, right);
}
void netText(int y, const String& value) {
  display.setTextSize(1); display.setTextColor(kMuted, kBackground);
  display.setCursor(6, y);
#ifdef AWOK_MINI_DISPLAY
  display.print(value);
#else
  display.print(clipped(value, 37));
#endif
}
void drawNetworkMenu() {
  currentView = View::kNetworkMenu;
  display.fillScreen(kBackground);
  drawHeader("NETWORK TOOLS", WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : "connect to your test network");
  for (int row = 0; row < 6; ++row) {
    const int item = netMenuPage * 6 + row;
    if (item >= 8) break;
    netButton(20, 50 + row * 32, 200, 30, kNetMenu[item]);
  }
  if (netMenuPage) netText(130, clipped(netStatus, 37));
  drawThreeButtonFooter("Back", "< Prev", "Next >");
}
void openNetworkTools() {
  signalMonitorActive = false;
  if (netOpen) { drawNetworkMenu(); return; }
  releaseBleMemory();
  netHosts = static_cast<NetHost*>(calloc(kNetCapacity, sizeof(NetHost)));
  netResults = static_cast<NetResult*>(calloc(kNetCapacity, sizeof(NetResult)));
  netResponse = static_cast<char*>(malloc(kNetResponseBytes + 1));
  if (!netHosts || !netResults || !netResponse) {
    free(netHosts); free(netResults); free(netResponse);
    netHosts = nullptr; netResults = nullptr; netResponse = nullptr;
    showRadioError("Network Tools: low memory"); return;
  }
  netOpen = true; netHostCount = netResultCount = 0; netMenuPage = netPage = 0;
  netSelectedHost = -1; netJob = netLastJob = NetJob::None;
  netHostSummary = NetSummary{}; netResultSummary = NetSummary{}; netHostsLimited = false;
  netLimited = netSubnetLimited = false; netStatus = "";
  netSsid = selectedWifi.ssid;
  drawNetworkMenu();
}
void drawNetworkSetup() {
  currentView = View::kNetworkSetup;
  display.fillScreen(kBackground);
  drawHeader("CONNECT WI-FI", "credentials kept in RAM only");
  netButton(20, 50, 200, 30, "SSID: " + clipped(netSsid, 22));
  netButton(20, 90, 200, 30, netPassword.length() ? "Password: ********" : "Password: empty");
  netButton(20, 130, 200, 30, "Choose scanned AP");
  netButton(20, 170, 200, 30, netJob == NetJob::Join ? "Connecting..." : "Join network");
  netText(220, clipped(netStatus, 37));
  if (WiFi.status() == WL_CONNECTED) netText(240, "IP: " + WiFi.localIP().toString());
  netFooter("Back", netJob == NetJob::Join ? "Cancel" : "Disconnect");
}
void drawNetworkEditor() {
  currentView = View::kNetworkEdit;
  display.fillScreen(kBackground);
  drawHeader(netEditPassword ? "WI-FI PASSWORD" : "NETWORK SSID", "select characters; Done to keep");
  String preview;
  if (netEditPassword) { for (size_t i = 0; i < netEdit.length(); ++i) preview += '*'; }
  else preview = netEdit;
  netText(50, clipped(preview, 37));
  netText(65, String(netEdit.length()) + (netEditPassword ? "/63 bytes" : "/32 bytes"));
  // Printable ASCII, 12 keys per page, with native Mini focus targets.
  for (int i = 0; i < 12; ++i) {
    const int code = 32 + netKeyPage * 12 + i;
    if (code > 126) break;
    const String label = code == 32 ? String("Space") : String(char(code));
    drawSmallButton(4 + (i % 3) * 78, 88 + (i / 3) * 40, 74, 34, label, kAccent);
  }
  drawFiveButtonFooter("Back", "Prev", "Next", "Del", "Done");
}
void drawNetworkAps() {
  currentView = View::kNetworkAps;
  display.fillScreen(kBackground);
  drawHeader("CHOOSE NETWORK", "hidden names can be entered manually");
  const int pages = max(1, (wifiCount + 5) / 6);
  netApPage = (netApPage + pages) % pages;
  for (int row = 0; row < 6; ++row) {
    int index = netApPage * 6 + row;
    if (index >= wifiCount) break;
    netButton(10, 50 + row * 32, 220, 30, clipped(wifiEntries[index].ssid.length() ? wifiEntries[index].ssid : "<hidden>", 26));
  }
  drawFourButtonFooter("Back", "Prev", "Next", "Scan");
}
void drawNetworkResults() {
  currentView = View::kNetworkResults;
  display.fillScreen(kBackground);
  int count = netShowHosts ? netHostCount : netResultCount;
  const int pages = max(1, (count + 5) / 6);
  netPage = (netPage + pages) % pages;
  drawHeader(netShowHosts ? "LAN HOSTS" : "NETWORK RESULTS", String(count) + " found | " + String(netPage + 1) + "/" + String(pages));
  for (int row = 0; row < 6; ++row) {
    const int index = netPage * 6 + row;
    if (index >= count) break;
    String label = netShowHosts ? netIpText(netHosts[index].ip) : netIpText(netResults[index].ip) + ":" + String(netResults[index].port) + " " + netResults[index].kind;
    netButton(6, 50 + row * 30, 228, 28, clipped(label, 32));
  }
  const NetSummary& summary = netShowHosts ? netHostSummary : netResultSummary;
  const bool running = netJob != NetJob::None;
  netText(236, clipped(netStatus, 37));
  netText(250, String(running ? netCompleted : summary.checked) + " checked; " + String(running ? netTimeouts : summary.timeouts) + " timeout; " + String(running ? netErrors : summary.errors) + " err");
  netText(263, (running ? netLimited : summary.limited) ? "Result cap reached; export is partial" : (running ? netSubnetLimited || netHostsLimited : summary.subnetLimited || summary.hostsLimited) ? "Host discovery limited; partial scope" : "");
  drawFiveButtonFooter("Back", "Prev", "Next", "Save", netJob == NetJob::None ? (netShowHosts ? "Results" : "Hosts") : "Stop");
}
void drawNetworkHost() {
  currentView = View::kNetworkHost;
  display.fillScreen(kBackground);
  drawHeader("INSPECT HOST", netIpText(netHosts[netSelectedHost].ip));
  netText(50, "MAC: " + macToString(netHosts[netSelectedHost].mac));
  const char* labels[] = {"TCP Ports", "Camera Services", "Printer Services", "SIP Services"};
  for (int i = 0; i < 4; ++i) netButton(20, 80 + i * 40, 200, 32, labels[i]);
  netFooter("Back", "Back");
}
void drawNetworkDetail() {
  currentView = View::kNetworkDetail;
  NetResult& item = netResults[netSelectedResult];
  display.fillScreen(kBackground);
  drawHeader(item.kind, netIpText(item.ip) + ":" + String(item.port));
  const String detail(item.detail);
  for (unsigned i = 0; i < detail.length(); i += 36) netText(55 + (i / 36) * 20, detail.substring(i, i + 36));
  netText(210, "Service evidence; identity unverified.");
  netFooter("Back", "Back");
}
bool netSaveCsv() {
  if (!sdReady) return false;
  if (netJob != NetJob::None && (netShowHosts == (netJob == NetJob::Hosts))) netSnapshot();
  const NetSummary& summary = netShowHosts ? netHostSummary : netResultSummary;
  const char* path = netShowHosts ? "/awokxdag/latest_lan_hosts.csv" : "/awokxdag/latest_network_services.csv";
  const String temp = String(path) + ".tmp";
  SD.remove(temp.c_str()); File file = SD.open(temp.c_str(), FILE_WRITE);
  if (!file) return false;
  file.println("uptime_ms,scan,status,network_ssid,local_ip,ip,mac,port,kind,evidence,result_limit,subnet_limit,checked,timeouts,refused,errors,host_discovery_limited");
  const int count = netShowHosts ? netHostCount : netResultCount;
  // Keep an explicit summary row even when nothing responded.
  for (int i = -1; i < count; ++i) {
    file.print(millis()); file.print(',');
    file.print(csvField(netJobName(summary.job))); file.print(',');
    file.print(csvField(summary.status)); file.print(','); file.print(csvField(summary.ssid)); file.print(',');
    file.print(netIpText(summary.ip)); file.print(',');
    if (i < 0) file.print(",,,summary,");
    else if (netShowHosts) {
      file.print(netIpText(netHosts[i].ip)); file.print(','); file.print(macToString(netHosts[i].mac));
      file.print(",,ARP,ARP cache observation");
    } else {
      const NetResult& item = netResults[i]; file.print(netIpText(item.ip)); file.print(",,");
      file.print(item.port); file.print(','); file.print(csvField(item.kind)); file.print(','); file.print(csvField(item.detail));
    }
    file.print(','); file.print(summary.limited ? "true" : "false"); file.print(',');
    file.print(summary.subnetLimited ? "true" : "false"); file.print(','); file.print(summary.checked); file.print(',');
    file.print(summary.timeouts); file.print(','); file.print(summary.refused); file.print(','); file.print(summary.errors); file.print(','); file.print(summary.hostsLimited ? "true" : "false"); file.println();
  }
  bool ok = !file.getWriteError(); file.close();
  if (!ok) { SD.remove(temp.c_str()); return false; }
  SD.remove(path); return SD.rename(temp.c_str(), path);
}
void netAdd(uint32_t ip, uint16_t port, const char* kind, const String& evidence) {
  for (int i = 0; i < netResultCount; ++i)
    if (netResults[i].ip == ip && netResults[i].port == port && !strcmp(netResults[i].kind, kind) && evidence == netResults[i].detail) return;
  if (netResultCount >= kNetCapacity) { netLimited = true; return; }
  NetResult& out = netResults[netResultCount++]; out.ip = ip; out.port = port;
  snprintf(out.kind, sizeof(out.kind), "%s", kind);
  String clean = evidence; clean.replace('\r', ' '); clean.replace('\n', ' ');
  snprintf(out.detail, sizeof(out.detail), "%s", clean.c_str());
}
void netFinish(const String& message) {
  const NetJob completed = netJob;
  netCloseSocket(); netUdp.stop(); netJob = NetJob::None; netStage = NetStage::Idle;
  netStatus = message;
  if (completed == NetJob::Hosts) netHostsLimited = netLimited || netSubnetLimited || message.startsWith("Cancelled") || message.startsWith("Connection");
  netSnapshot();
  recordFirmwareAudit("network", netJobName(completed), message.c_str(), String(netCompleted) + " probes; " + String(netResultCount) + " service results");
  const bool saved = netSaveCsv();
  netStatus += saved ? " | SD saved" : sdReady ? " | SD write failed" : " | no SD";
  netSnapshot();
  drawNetworkResults();
}
void netDisconnect() {
  netCloseSocket(); netUdp.stop(); netJob = NetJob::None; netStage = NetStage::Idle;
  WiFi.setAutoReconnect(false); shutdownWifi(); netWipe(netPassword); netWipe(netEdit);
  netHostCount = netResultCount = 0; netSelectedHost = -1;
  netHostSummary = NetSummary{}; netResultSummary = NetSummary{};
  netHostsLimited = false; netStatus = "Disconnected";
}
void netJoin() {
  if (!netSsid.length() || netSsid.length() > 32 || (netPassword.length() && netPassword.length() < 8)) {
    netStatus = "SSID 1-32; WPA key 8-63 or empty"; drawNetworkSetup(); return;
  }
  netHostCount = netResultCount = 0; netSelectedHost = -1;
  netHostSummary = NetSummary{}; netResultSummary = NetSummary{};
  netLastJob = NetJob::None; netHostsLimited = false;
  WiFi.persistent(false);
  if (!ensureWifiStation()) { netStatus = "Wi-Fi initialization failed"; drawNetworkSetup(); return; }
  WiFi.setAutoReconnect(false); esp_wifi_set_storage(WIFI_STORAGE_RAM);
  WiFi.disconnect(false, false);
  WiFi.begin(netSsid.c_str(), netPassword.c_str()); netWipe(netPassword);
  netJob = NetJob::Join; netDeadline = millis() + 20000; netStatus = "Connecting...";
  recordFirmwareAudit("network", "connect", "started", "user-selected network");
  drawNetworkSetup();
}
err_t netArpOnTcpip(tcpip_api_call_data* base) {
  NetArpCall* query = reinterpret_cast<NetArpCall*>(base);
  esp_netif_t* iface = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
  if (!iface) return ERR_IF;
  netif* ni = static_cast<netif*>(esp_netif_get_netif_impl(iface));
  if (!ni) return ERR_IF;
  ip4_addr_t address; address.addr = htonl(query->ip);
  if (query->send) return etharp_request(ni, &address);
  eth_addr* mac = nullptr; const ip4_addr_t* found = nullptr;
  query->found = etharp_find_addr(ni, &address, &mac, &found) >= 0 && mac;
  if (query->found) memcpy(query->mac, mac->addr, 6);
  return ERR_OK;
}
void netStartJob(NetJob job, int host) {
  if (netJob != NetJob::None) return;
  if (WiFi.status() != WL_CONNECTED) { netStatus = "Connect before scanning"; drawNetworkSetup(); return; }
  netSessionIp = netIpNumber(WiFi.localIP()); netSessionMask = netIpNumber(WiFi.subnetMask());
  if (!NetworkParse::range(netSessionIp, netSessionMask, netFirst, netLast)) {
    netStatus = "Unsupported subnet (/31, /32 or mask)"; drawNetworkSetup(); return;
  }
  if (job != NetJob::Hosts && job != NetJob::Upnp && netHostCount &&
      (netHostSummary.ip != netSessionIp || netHostSummary.mask != netSessionMask ||
       strcmp(netHostSummary.ssid, WiFi.SSID().c_str()))) {
    netHostCount = 0; netSelectedHost = -1;
  }
  if (job != NetJob::Hosts && job != NetJob::Upnp && !netHostCount) {
    netStatus = "Run Discover Hosts first"; netShowHosts = true; drawNetworkResults(); return;
  }
  NetSummary& summary = job == NetJob::Hosts ? netHostSummary : netResultSummary;
  summary = NetSummary{}; summary.ip = netSessionIp; summary.mask = netSessionMask;
  snprintf(summary.ssid, sizeof(summary.ssid), "%s", WiFi.SSID().c_str());
  netJob = netLastJob = job; netCompleted = netTimeouts = netRefused = netErrors = 0;
  netPage = 0; netLimited = false; netResultCount = 0;
  if (job == NetJob::Hosts) { netResultSummary = NetSummary{}; netHostsLimited = false; }
  else netSubnetLimited = netHostSummary.subnetLimited;
  netShowHosts = job == NetJob::Hosts;
  netStatus = String("Scanning ") + netJobName(job);
  if (job == NetJob::Hosts) {
    netHostCount = 0; netSelectedHost = -1; netSubnetLimited = netLast - netFirst + 1 > 1024;
    if (netSubnetLimited) {
      netFirst = max(netFirst, (netSessionIp & 0xffffff00U) + 1);
      netLast = min(netLast, (netSessionIp & 0xffffff00U) + 254);
    }
    netCursor = netFirst; netStage = NetStage::ArpSend;
  } else if (job == NetJob::Upnp) {
    netSubnetLimited = false; netMappingIndex = 0; netUpnpStep = 0;
    if (!netUdp.begin(0)) { netFinish("UDP initialization failed"); return; }
    const char* request = "M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nMAN: \"ssdp:discover\"\r\nMX: 2\r\nST: urn:schemas-upnp-org:device:InternetGatewayDevice:1\r\n\r\n";
    if (!netUdp.beginPacket(IPAddress(239,255,255,250), 1900) || netUdp.write((const uint8_t*)request, strlen(request)) != strlen(request) || !netUdp.endPacket()) {
      netFinish("SSDP send failed"); return;
    }
    netStage = NetStage::SsdpWait; netDeadline = millis() + 3500;
  } else {
    netHostCursor = host >= 0 ? host : 0; netHostEnd = host >= 0 ? host + 1 : netHostCount;
    netPortCursor = 0; netStage = NetStage::Probe;
    if (job == NetJob::Sip && !netUdp.begin(kNetSipLocalPort)) { netFinish("UDP initialization failed"); return; }
  }
  recordFirmwareAudit("network", netJobName(job), "started", host >= 0 ? netIpText(netHosts[host].ip) : "connected LAN");
  netSnapshot();
  drawNetworkResults();
}
uint16_t netCurrentPort() {
  if (netJob == NetJob::Ports) return kNetPorts[netPortCursor];
  if (netJob == NetJob::Cameras) return kNetCameraPorts[netPortCursor];
  if (netJob == NetJob::Printers) return kNetPrinterPorts[netPortCursor];
  return 5060;
}
void netNextProbe() {
  netCloseSocket(); ++netCompleted;
  const int count = netJob == NetJob::Ports ? sizeof(kNetPorts)/sizeof(kNetPorts[0]) : netJob == NetJob::Cameras ? sizeof(kNetCameraPorts)/sizeof(kNetCameraPorts[0]) : netJob == NetJob::Printers ? 3 : 1;
  if (++netPortCursor >= count) { netPortCursor = 0; ++netHostCursor; }
  if (netHostCursor >= netHostEnd) netFinish("Complete (timeouts are inconclusive)");
  else netStage = NetStage::Probe;
}
void netTcpStart(uint32_t ip, uint16_t port, const String& request) {
  netCloseSocket();
  netRequest = request; netResponse[0] = 0;
  netSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (netSocket < 0 || fcntl(netSocket, F_SETFL, O_NONBLOCK) < 0) {
    ++netErrors;
    if (netJob == NetJob::Upnp) netFinish("Socket allocation failed"); else netNextProbe();
    return;
  }
  sockaddr_in address = {}; address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(ip); address.sin_port = htons(port);
  int result = connect(netSocket, reinterpret_cast<sockaddr*>(&address), sizeof(address));
  if (result < 0 && errno != EINPROGRESS && errno != EWOULDBLOCK) {
    if (errno == ECONNREFUSED) ++netRefused; else ++netErrors;
    if (netJob == NetJob::Upnp) netFinish("Router connection failed"); else netNextProbe();
    return;
  }
  netStage = NetStage::Connecting; netDeadline = millis() + kNetConnectMs;
}
String netHttpRequest(const char* method, const NetworkParse::Url& url, const String& body, const String& action) {
  String request = String(method) + " " + url.path + " HTTP/1.1\r\nHost: " + netIpText(url.ip) + ":" + String(url.port) + "\r\nConnection: close\r\n";
  if (body.length()) {
    request += "Content-Type: text/xml; charset=\"utf-8\"\r\nContent-Length: " + String(body.length()) + "\r\n";
    if (action.length()) request += "SOAPAction: \"" + action + "\"\r\n";
  }
  return request + "\r\n" + body;
}
void netRequestMapping() {
  const String body = "<?xml version=\"1.0\"?><s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body><u:GetGenericPortMappingEntry xmlns:u=\"" + netUpnpService + "\"><NewPortMappingIndex>" + String(netMappingIndex) + "</NewPortMappingIndex></u:GetGenericPortMappingEntry></s:Body></s:Envelope>";
  netTcpStart(netUpnpUrl.ip, netUpnpUrl.port, netHttpRequest("POST", netUpnpUrl, body, netUpnpService + "#GetGenericPortMappingEntry"));
}
bool netResolveControl(const char* value, const NetworkParse::Url& base, NetworkParse::Url& out) {
  String decoded(value); decoded.replace("&amp;", "&");
  if (decoded.startsWith("http://")) {
    if (!NetworkParse::url(decoded.c_str(), out)) return false;
  } else {
    if (decoded.indexOf(":") >= 0 || decoded.startsWith("//")) return false;
    String path(base.path);
    if (decoded.startsWith("/")) path = decoded;
    else path = path.substring(0, path.lastIndexOf('/') + 1) + decoded;
    const String absolute = "http://" + netIpText(base.ip) + ":" + String(base.port) + path;
    if (!NetworkParse::url(absolute.c_str(), out)) return false;
  }
  // The mapping viewer only follows literal-IP URLs on the connected gateway.
  return out.ip == netIpNumber(WiFi.gatewayIP());
}
void netUpnpResponse() {
  char* body = nullptr; size_t size = 0;
  if (!NetworkParse::httpBody(netResponse, netReceived, body, size)) { ++netErrors; netFinish("Invalid/truncated router response"); return; }
  int code = 0; sscanf(netResponse, "HTTP/%*s %d", &code);
  if (netUpnpStep == 1) {
    if (code != 200) { netFinish("Router description HTTP " + String(code)); return; }
    NetworkParse::Url base = netUpnpUrl;
    char value[320];
    if (NetworkParse::xml(body, "URLBase", value, sizeof(value))) {
      if (!NetworkParse::url(value, base) || base.ip != netIpNumber(WiFi.gatewayIP())) {
        netFinish("Unsupported router URLBase"); return;
      }
    }
    const char* cursor = body;
    // Heap workspace avoids adding a large temporary to the Arduino loop stack.
    char* section = static_cast<char*>(malloc(2048));
    if (!section) { netFinish("Low memory parsing router"); return; }
    bool found = false;
    while (NetworkParse::block(cursor, "service", section, 2048)) {
      char service[100];
      if (!NetworkParse::xml(section, "serviceType", service, sizeof(service))) continue;
      if (strcmp(service, "urn:schemas-upnp-org:service:WANIPConnection:1") &&
          strcmp(service, "urn:schemas-upnp-org:service:WANIPConnection:2") &&
          strcmp(service, "urn:schemas-upnp-org:service:WANPPPConnection:1")) continue;
      if (!NetworkParse::xml(section, "controlURL", value, sizeof(value)) ||
          !netResolveControl(value, base, netUpnpUrl)) continue;
      netUpnpService = service; found = true; break;
    }
    free(section);
    if (!found) { netFinish("No supported IGD control service"); return; }
    netUpnpStep = 2; netRequestMapping(); return;
  }
  char value[128];
  if (NetworkParse::xml(body, "errorCode", value, sizeof(value))) {
    if (!strcmp(value, "713")) netFinish("Complete: end of mapping table");
    else netFinish("Router SOAP error " + String(value));
    return;
  }
  if (code != 200) { netFinish("Mapping HTTP " + String(code)); return; }
  char internal[24], external[12], internalPort[12], protocol[12], enabled[12];
  if (!NetworkParse::xml(body, "NewInternalClient", internal, sizeof(internal)) ||
      !NetworkParse::xml(body, "NewExternalPort", external, sizeof(external)) ||
      !NetworkParse::xml(body, "NewInternalPort", internalPort, sizeof(internalPort)) ||
      !NetworkParse::xml(body, "NewProtocol", protocol, sizeof(protocol)) ||
      !NetworkParse::xml(body, "NewEnabled", enabled, sizeof(enabled))) {
    ++netErrors; netFinish("Incomplete mapping response"); return;
  }
  char* end; unsigned long port = strtoul(external, &end, 10);
  if (!*external || *end || port > 65535 || !port) { netFinish("Invalid mapping port"); return; }
  String detail = String(protocol) + " external " + external + " -> " + internal + ":" + internalPort + " enabled=" + enabled;
  if (NetworkParse::xml(body, "NewPortMappingDescription", value, sizeof(value))) detail += " " + String(value);
  netAdd(netIpNumber(WiFi.gatewayIP()), port, "UPnP mapping", detail);
  ++netCompleted;
  if (++netMappingIndex >= min(64, kNetCapacity)) { netLimited = true; netFinish("Mapping limit reached (partial)"); return; }
  netRequestMapping();
}
void netReadProbeResponse() {
  if (netJob == NetJob::Upnp) { netUpnpResponse(); return; }
  const uint32_t ip = netHosts[netHostCursor].ip; const uint16_t port = netCurrentPort();
  if (netJob == NetJob::Cameras && (port == 554 || port == 8554)) {
    int code = 0;
    if (sscanf(netResponse, "RTSP/1.0 %d", &code) == 1 && code >= 100 && code <= 599) {
      char server[90] = {};
      NetworkParse::header(netResponse, "Server", server, sizeof(server));
      netAdd(ip, port, "RTSP", "RTSP status " + String(code) + "; camera candidate " + server);
    }
  } else if (netJob == NetJob::Cameras) {
    char* body = nullptr; size_t size = 0;
    if (NetworkParse::httpBody(netResponse, netReceived, body, size)) {
      if (strstr(body, "onvif.org") && (strstr(body, "GetDeviceInformationResponse") || strstr(body, "NotAuthorized"))) {
        char maker[60] = {}, model[60] = {};
        NetworkParse::xml(body, "Manufacturer", maker, sizeof(maker));
        NetworkParse::xml(body, "Model", model, sizeof(model));
        netAdd(ip, port, "ONVIF", *maker || *model ? String(maker) + " " + model : "ONVIF authentication required");
      }
    } else ++netErrors;
  }
  netNextProbe();
}
void netBeginProbe() {
  uint32_t ip = netHosts[netHostCursor].ip; uint16_t port = netCurrentPort();
  netStatus = String(netJobName(netJob)) + " " + netIpText(ip) + ":" + String(port);
  if (netJob == NetJob::Sip) {
    netSipId = String(esp_random(), HEX) + "@awokxdag";
    String request = "OPTIONS sip:" + netIpText(ip) + " SIP/2.0\r\nVia: SIP/2.0/UDP " + WiFi.localIP().toString() + ":" + String(kNetSipLocalPort) + ";branch=z9hG4bK" + String(esp_random(), HEX) + ";rport\r\nFrom: <sip:scan@awokxdag>;tag=1\r\nTo: <sip:" + netIpText(ip) + ">\r\nCall-ID: " + netSipId + "\r\nCSeq: 1 OPTIONS\r\nMax-Forwards: 1\r\nContent-Length: 0\r\n\r\n";
    if (!netUdp.beginPacket(netIpAddress(ip), 5060) || netUdp.write((const uint8_t*)request.c_str(), request.length()) != request.length() || !netUdp.endPacket()) {
      ++netErrors; netNextProbe(); return;
    }
    netStage = NetStage::SipWait; netDeadline = millis() + 700; return;
  }
  String request;
  if (netJob == NetJob::Cameras) {
    if (port == 554 || port == 8554) request = "OPTIONS rtsp://" + netIpText(ip) + ":" + String(port) + "/ RTSP/1.0\r\nCSeq: 1\r\nUser-Agent: AxD\r\n\r\n";
    else {
      NetworkParse::Url url; url.ip = ip; url.port = port;
      strcpy(url.path, "/onvif/device_service");
      const String body = "<?xml version=\"1.0\"?><s:Envelope xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\"><s:Body><GetDeviceInformation xmlns=\"http://www.onvif.org/ver10/device/wsdl\"/></s:Body></s:Envelope>";
      request = netHttpRequest("POST", url, body, "http://www.onvif.org/ver10/device/wsdl/GetDeviceInformation");
      request.replace("text/xml; charset=\"utf-8\"", "application/soap+xml; charset=\"utf-8\"; action=\"http://www.onvif.org/ver10/device/wsdl/GetDeviceInformation\"");
    }
  }
  netTcpStart(ip, port, request);
}
void netUpdateTcp() {
  if (netSocket < 0) return;
  if (netStage == NetStage::Connecting) {
    fd_set writes, errors; FD_ZERO(&writes); FD_ZERO(&errors);
    FD_SET(netSocket, &writes); FD_SET(netSocket, &errors); timeval zero = {};
    const int ready = select(netSocket + 1, nullptr, &writes, &errors, &zero);
    if (ready < 0) {
      ++netErrors;
      if (netJob == NetJob::Upnp) netFinish("Socket error"); else netNextProbe();
      return;
    }
    if (ready) {
      int error = 0; socklen_t length = sizeof(error);
      if (getsockopt(netSocket, SOL_SOCKET, SO_ERROR, &error, &length) < 0) error = errno;
      if (error) {
        if (error == ECONNREFUSED) ++netRefused; else ++netErrors;
        if (netJob == NetJob::Upnp) netFinish("Router connection failed"); else netNextProbe();
        return;
      }
      if (!netRequest.length()) {
        const uint16_t port = netCurrentPort();
        const char* kind = netJob == NetJob::Ports ? "TCP open" : port == 9100 ? "Raw print?" : port == 631 ? "IPP?" : "LPD?";
        netAdd(netHosts[netHostCursor].ip, port, kind, netJob == NetJob::Ports ? "TCP connection accepted; service not verified" : "Printer candidate by open port; no print job sent");
        netNextProbe(); return;
      }
      netStage = NetStage::Sending; netDeadline = millis() + kNetResponseMs;
    }
  }
  if (netStage == NetStage::Sending) {
    int written = send(netSocket, netRequest.c_str() + netSent, min(size_t(1024), netRequest.length() - netSent), 0);
    if (written > 0) netSent += written;
    else if (written < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
      ++netErrors;
      if (netJob == NetJob::Upnp) netFinish("Router send failed"); else netNextProbe();
      return;
    }
    if (netSent == netRequest.length()) { netStage = NetStage::Reading; netDeadline = millis() + kNetResponseMs; }
  }
  if (netStage == NetStage::Reading) {
    const int received = recv(netSocket, netResponse + netReceived, min(size_t(1024), kNetResponseBytes - netReceived), 0);
    if (received > 0) {
      netReceived += received; netResponse[netReceived] = 0;
      if (memchr(netResponse, 0, netReceived)) {
        ++netErrors;
        if (netJob == NetJob::Upnp) netFinish("Invalid router response"); else netNextProbe();
        return;
      }
      // RTSP OPTIONS needs only the response headers; servers may keep it open.
      if (netJob == NetJob::Cameras && (netCurrentPort() == 554 || netCurrentPort() == 8554) && strstr(netResponse, "\r\n\r\n")) { netReadProbeResponse(); return; }
      // HTTP Content-Length allows completion without waiting for peer close.
      char value[32]; char* body = strstr(netResponse, "\r\n\r\n");
      char encoding[32];
      if (body && !NetworkParse::header(netResponse, "Transfer-Encoding", encoding, sizeof(encoding)) && NetworkParse::header(netResponse, "Content-Length", value, sizeof(value))) {
        char* end; unsigned long len = strtoul(value, &end, 10);
        if (*value && !*end && len <= netReceived - size_t(body + 4 - netResponse)) { netReadProbeResponse(); return; }
      }
      if (netReceived == kNetResponseBytes) {
        ++netErrors;
        if (netJob == NetJob::Upnp) netFinish("Router response exceeds 8 KiB"); else netNextProbe();
        return;
      }
    } else if (received == 0) { netReadProbeResponse(); return; }
    else if (errno != EAGAIN && errno != EWOULDBLOCK) {
      ++netErrors;
      if (netJob == NetJob::Upnp) netFinish("Router receive failed"); else netNextProbe();
      return;
    }
  }
  if (netExpired()) {
    ++netTimeouts;
    if (netJob == NetJob::Upnp) netFinish("Router timeout (partial)"); else netNextProbe();
  }
}
void updateNetworkTools() {
  if (!netOpen || netJob == NetJob::None) return;
  if (netJob == NetJob::Join) {
    if (WiFi.status() == WL_CONNECTED) {
      netJob = NetJob::None; netStatus = "Connected: " + WiFi.localIP().toString();
      recordFirmwareAudit("network", "connect", "success", WiFi.localIP().toString());
      drawNetworkSetup();
    } else if (netExpired()) {
      netDisconnect(); netStatus = "Connection timed out; re-enter key";
      recordFirmwareAudit("network", "connect", "timeout", ""); drawNetworkSetup();
    }
    return;
  }
  if (WiFi.status() != WL_CONNECTED || netIpNumber(WiFi.localIP()) != netSessionIp || netIpNumber(WiFi.subnetMask()) != netSessionMask) {
    netFinish("Connection changed/lost (partial)");
    netHostCount = 0; netSelectedHost = -1; return;
  }
  if (netStage == NetStage::ArpSend) {
    if (netCursor == netSessionIp) ++netCursor;
    if (netCursor > netLast) { netFinish("Complete; silent hosts may be absent"); return; }
    NetArpCall query = {}; query.ip = netCursor; query.send = true;
    if (tcpip_api_call(netArpOnTcpip, &query.call) != ERR_OK) ++netErrors;
    netDeadline = millis() + 200; netStage = NetStage::ArpWait;
    netStatus = "ARP " + netIpText(netCursor) + " / " + String(netLast - netFirst + 1);
  } else if (netStage == NetStage::ArpWait && netExpired()) {
    NetArpCall query = {}; query.ip = netCursor;
    if (tcpip_api_call(netArpOnTcpip, &query.call) != ERR_OK) ++netErrors;
    else if (query.found) {
      if (netHostCount < kNetCapacity) {
        NetHost& host = netHosts[netHostCount++]; host.ip = netCursor; memcpy(host.mac, query.mac, 6);
      } else netLimited = true;
    } else ++netTimeouts;
    ++netCompleted; ++netCursor; netStage = NetStage::ArpSend;
  } else if (netStage == NetStage::Probe) netBeginProbe();
  else if (netStage == NetStage::SsdpWait || netStage == NetStage::SipWait) {
    int size = netUdp.parsePacket();
    if (size > 0) {
      uint32_t sender = netIpNumber(netUdp.remoteIP());
      const uint16_t senderPort = netUdp.remotePort();
      int got = netUdp.read(netResponse, kNetResponseBytes);
      netUdp.clear();
      if (got > 0 && size <= int(kNetResponseBytes) && !memchr(netResponse, 0, got)) {
        netResponse[got] = 0;
        if (netStage == NetStage::SsdpWait && sender == netIpNumber(WiFi.gatewayIP())) {
          char location[320]; NetworkParse::Url url;
          if (!strncmp(netResponse, "HTTP/1.1 200", 12) && NetworkParse::header(netResponse, "LOCATION", location, sizeof(location)) && NetworkParse::url(location, url) && url.ip == sender) {
            netUdp.stop(); netUpnpUrl = url; netUpnpStep = 1;
            netTcpStart(url.ip, url.port, netHttpRequest("GET", url, "", ""));
          }
        } else if (netStage == NetStage::SipWait && sender == netHosts[netHostCursor].ip && senderPort == 5060) {
          int code = 0; char id[96], cseq[40];
          if (sscanf(netResponse, "SIP/2.0 %d", &code) == 1 && code >= 100 && code <= 699 && NetworkParse::header(netResponse, "Call-ID", id, sizeof(id)) && netSipId == id && NetworkParse::header(netResponse, "CSeq", cseq, sizeof(cseq)) && !strcmp(cseq, "1 OPTIONS")) {
            char server[100] = {}; NetworkParse::header(netResponse, "Server", server, sizeof(server));
            netAdd(sender, 5060, "SIP", "OPTIONS status " + String(code) + " " + server);
            netNextProbe();
          }
        }
      }
    }
    if ((netStage == NetStage::SsdpWait || netStage == NetStage::SipWait) && netExpired()) {
      ++netTimeouts;
      if (netJob == NetJob::Upnp) netFinish("No supported gateway SSDP response"); else netNextProbe();
    }
  } else netUpdateTcp();
  if (currentView == View::kNetworkResults && millis() - netLastDraw >= 600) { netLastDraw = millis(); drawNetworkResults(); }
}
void handleNetworkTouch(int x, int y) {
  if (currentView == View::kNetworkEdit) {
    if (y >= kFooterTop) {
      if (x < 48) { netWipe(netEdit); drawNetworkSetup(); }
      else if (x < 96) { netKeyPage = (netKeyPage + 7) % 8; drawNetworkEditor(); }
      else if (x < 144) { netKeyPage = (netKeyPage + 1) % 8; drawNetworkEditor(); }
      else if (x < 192) { if (netEdit.length()) netEdit.remove(netEdit.length() - 1); drawNetworkEditor(); }
      else {
        if (netEditPassword) { netWipe(netPassword); netPassword = netEdit; }
        else { if (netSsid != netEdit) netWipe(netPassword); netSsid = netEdit; }
        netWipe(netEdit); drawNetworkSetup();
      }
    } else if (y >= 88 && y < 248) {
      int col = (x - 4) / 78, row = (y - 88) / 40;
      int code = 32 + netKeyPage * 12 + row * 3 + col;
      if (x >= 4 && col < 3 && (y - 88) % 40 < 34 && code <= 126 && netEdit.length() < (netEditPassword ? 63U : 32U)) netEdit += char(code);
      drawNetworkEditor();
    }
    return;
  }
  if (currentView == View::kNetworkSetup) {
    if (netJob == NetJob::Join) {
      if (y >= kFooterTop) { netDisconnect(); drawNetworkSetup(); }
      return;
    }
    if (y >= kFooterTop) {
      if (x < 120) drawNetworkMenu(); else { netDisconnect(); drawNetworkSetup(); }
    } else if ((y >= 50 && y < 80) || (y >= 90 && y < 120)) {
      netEditPassword = y >= 90; netEdit = netEditPassword ? netPassword : netSsid;
      netKeyPage = 2; drawNetworkEditor();
    } else if (y >= 130 && y < 160) { netApPage = 0; drawNetworkAps(); }
    else if (y >= 170 && y < 200) netJoin();
    return;
  }
  if (currentView == View::kNetworkAps) {
    if (y >= kFooterTop) {
      if (x < 60) drawNetworkSetup();
      else if (x < 120) { --netApPage; drawNetworkAps(); }
      else if (x < 180) { ++netApPage; drawNetworkAps(); }
      else {
        netDisconnect(); scanWifi();
        if (lastWifiScanOk) drawNetworkAps();
        else { netStatus = "AP scan failed"; drawNetworkSetup(); }
      }
    } else if (y >= 50 && y < 242 && (y - 50) % 32 < 30) {
      int index = netApPage * 6 + (y - 50) / 32;
      if (index < wifiCount) {
        if (netSsid != wifiEntries[index].ssid) netWipe(netPassword);
        netSsid = wifiEntries[index].ssid; netStatus = "Enter key, then Join"; drawNetworkSetup();
      }
    }
    return;
  }
  if (currentView == View::kNetworkMenu) {
    if (y >= kFooterTop) {
      if (x < 80) { closeNetworkTools(); drawReconMenu(); }
      else { netMenuPage = 1 - netMenuPage; drawNetworkMenu(); }
    } else if (y >= 50 && y < 242 && (y - 50) % 32 < 30) {
      int item = netMenuPage * 6 + (y - 50) / 32;
      if (item == 0) drawNetworkSetup();
      else if (item == 1) netStartJob(NetJob::Hosts, -1);
      else if (item == 2) netStartJob(NetJob::Ports, -1);
      else if (item == 3) netStartJob(NetJob::Cameras, -1);
      else if (item == 4) netStartJob(NetJob::Printers, -1);
      else if (item == 5) netStartJob(NetJob::Sip, -1);
      else if (item == 6) netStartJob(NetJob::Upnp, -1);
      else if (item == 7) drawNetworkResults();
    }
    return;
  }
  if (currentView == View::kNetworkResults) {
    if (y >= kFooterTop) {
      if (x < 48) {
        if (netJob != NetJob::None) netFinish("Cancelled (partial)");
        drawNetworkMenu();
      } else if (x < 96) { --netPage; drawNetworkResults(); }
      else if (x < 144) { ++netPage; drawNetworkResults(); }
      else if (x < 192) {
        netStatus = netSaveCsv() ? "Snapshot saved" : "SD save failed / missing"; drawNetworkResults();
      } else if (netJob != NetJob::None) netFinish("Cancelled (partial)");
      else { netShowHosts = !netShowHosts; netPage = 0; netStatus = netShowHosts ? netHostSummary.status : netResultSummary.status; drawNetworkResults(); }
    } else if (netJob == NetJob::None && y >= 50 && y < 230 && (y - 50) % 30 < 28) {
      int index = netPage * 6 + (y - 50) / 30;
      if (netShowHosts && index < netHostCount) { netSelectedHost = index; drawNetworkHost(); }
      else if (!netShowHosts && index < netResultCount) { netSelectedResult = index; drawNetworkDetail(); }
    }
    return;
  }
  if (currentView == View::kNetworkHost) {
    if (y >= kFooterTop) drawNetworkResults();
    else if (y >= 80 && y < 240 && (y - 80) % 40 < 32) {
      const NetJob jobs[] = {NetJob::Ports, NetJob::Cameras, NetJob::Printers, NetJob::Sip};
      netStartJob(jobs[(y - 80) / 40], netSelectedHost);
    }
    return;
  }
  if (currentView == View::kNetworkDetail && y >= kFooterTop) drawNetworkResults();
}
void handleNetworkSerial() {
  // Existing serial shortcuts cannot switch radios out from under a LAN job.
  // Serial h always exits; passwords are entered through the masked device UI.
  if (Serial.available()) noteActivity();
  while (Serial.available()) {
    const char ch = Serial.read();
    if (ch == 'h' || ch == 'H') { closeNetworkTools(); drawHome(); return; }
  }
}
