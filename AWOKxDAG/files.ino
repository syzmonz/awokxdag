#include <atomic>

// AWOKxDAG — capture / SD file manager (compiled as part of the sketch; see
// awok_common.h)
//
// Lists the files under /awokxdag with sizes, paginated so every file is
// reachable, and lets you delete one behind a two-tap confirm. Reached from the
// Status screen.

constexpr int kMaxFileRows = 64;

struct FileRow {
  String name;
  uint32_t size;
};

FileRow fileRows[kMaxFileRows];
int fileRowCount = 0;
int fileSelected = -1;  // absolute index, -1 = none
bool fileConfirmDelete = false;
uint64_t fileTotalBytes = 0;
int filePage = 0;

String fileBaseName(const String& n) {
  const int slash = n.lastIndexOf('/');
  return slash >= 0 ? n.substring(slash + 1) : n;
}

String fileFullPath(const String& n) {
  if (n.startsWith("/")) return n;
  return String(kSdDirectory) + "/" + n;
}

int filePageCount() {
  if (fileRowCount == 0) return 1;
  return (fileRowCount + kVisibleRows - 1) / kVisibleRows;
}

void drawFilesManager() {
  currentView = View::kFiles;
  const int pages = filePageCount();
  if (filePage >= pages) filePage = 0;
  display.fillScreen(kBackground);
  drawHeader("CAPTURES",
             String(fileRowCount) + " files | " +
                 String(static_cast<uint32_t>(fileTotalBytes / 1024)) +
                 " KB | pg " + String(filePage + 1) + "/" + String(pages));
  display.setTextSize(1);
  const int start = filePage * kVisibleRows;
#ifdef AWOK_MINI_DISPLAY
  display.selectableRows(min(kVisibleRows, fileRowCount - start));
#endif
  for (int row = 0; row < kVisibleRows; ++row) {
    const int idx = start + row;
    if (idx >= fileRowCount) break;
    const int y = 48 + row * 22;
    display.setTextColor(idx == fileSelected ? kAccent : ILI9341_WHITE,
                         kBackground);
    display.setCursor(5, y);
    display.print(clipped(fileBaseName(fileRows[idx].name), 26));
    display.setTextColor(kMuted, kBackground);
    display.setCursor(5, y + 11);
    if (fileRows[idx].size >= 1024) {
      display.printf("%lu KB",
                     static_cast<unsigned long>(fileRows[idx].size / 1024));
    } else {
      display.printf("%lu B", static_cast<unsigned long>(fileRows[idx].size));
    }
  }
  if (fileRowCount == 0) {
    display.setTextColor(kMuted, kBackground);
    display.setCursor(40, 140);
    display.print(sdReady ? "No files in /awokxdag" : "SD not mounted");
  }
  const char* action =
      fileSelected >= 0 ? (fileConfirmDelete ? "OK!" : "Del") : "Resc";
  drawFourButtonFooter("Back", "< Prev", "Next >", action);
}

bool scanSdFiles() {
  fileRowCount = 0;
  fileSelected = -1;
  fileConfirmDelete = false;
  fileTotalBytes = 0;
  if (!ensureSdCard()) return false;
  File dir = SD.open(kSdDirectory);
  if (!dir || !dir.isDirectory()) return false;
  File entry = dir.openNextFile();
  while (entry && fileRowCount < kMaxFileRows) {
    if (!entry.isDirectory()) {
      fileRows[fileRowCount].name = String(entry.name());
      fileRows[fileRowCount].size = entry.size();
      fileTotalBytes += entry.size();
      ++fileRowCount;
    }
    entry = dir.openNextFile();
  }
  dir.close();
  return true;
}

void openFilesManager() {
  filePage = 0;
  scanSdFiles();
  drawFilesManager();
}

std::atomic<bool> g_fileStreamAborted{false};

void linkAbortFileStream() {
  g_fileStreamAborted = true;
}

static bool encodeBase64Chunk(const uint8_t* in, size_t inLen, char* out, size_t maxOut) {
  static const char alphabet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  const size_t outLen = ((inLen + 2) / 3) * 4;
  if (maxOut <= outLen) return false;
  size_t o = 0;
  for (size_t i = 0; i < inLen; i += 3) {
    const uint32_t a = in[i];
    const uint32_t b = (i + 1 < inLen) ? in[i + 1] : 0;
    const uint32_t c = (i + 2 < inLen) ? in[i + 2] : 0;
    const uint32_t triple = (a << 16) | (b << 8) | c;
    out[o++] = alphabet[(triple >> 18) & 63];
    out[o++] = alphabet[(triple >> 12) & 63];
    out[o++] = (i + 1 < inLen) ? alphabet[(triple >> 6) & 63] : '=';
    out[o++] = (i + 2 < inLen) ? alphabet[triple & 63] : '=';
  }
  out[o] = '\0';
  return true;
}

void linkStreamFileList() {
  scanSdFiles();
  const int n = fileRowCount;
#ifdef AWOK_HEADLESS
  for (int i = 0; i < n; ++i) {
    char line[96];
    const String base = fileBaseName(fileRows[i].name);
    snprintf(line, sizeof(line), "$FILELIST,%u,%u,%lu,%s",
             i, n, static_cast<unsigned long>(fileRows[i].size), base.c_str());
    Serial.println(line);
    bridgeNotifyResult(kSourceFiles, reinterpret_cast<const uint8_t*>(line), strlen(line));
    delay(20);
  }
  char endLine[64];
  snprintf(endLine, sizeof(endLine), "$FILELIST_END,%u,%lu",
           n, static_cast<unsigned long>(fileTotalBytes));
  Serial.println(endLine);
  bridgeNotifyResult(kSourceFiles, reinterpret_cast<const uint8_t*>(endLine), strlen(endLine));
#else
  if (!linkEnsureEspNow()) return;
  esp_wifi_set_channel(kLinkChannel, WIFI_SECOND_CHAN_NONE);
  for (int i = 0; i < n; ++i) {
    char line[96];
    const String base = fileBaseName(fileRows[i].name);
    snprintf(line, sizeof(line), "$FILELIST,%u,%u,%lu,%s",
             i, n, static_cast<unsigned long>(fileRows[i].size), base.c_str());
    Serial.println(line);
    if (linkEspNowReady) {
      AxdFileEntryMsg msg;
      msg.index = static_cast<uint8_t>(i);
      msg.count = static_cast<uint8_t>(n);
      msg.size = fileRows[i].size;
      strncpy(msg.name, base.c_str(), sizeof(msg.name) - 1);
      esp_now_send(kLinkBroadcastAddr, reinterpret_cast<uint8_t*>(&msg), sizeof(msg));
    }
    delay(30);
  }
  char endLine[64];
  snprintf(endLine, sizeof(endLine), "$FILELIST_END,%u,%lu",
           n, static_cast<unsigned long>(fileTotalBytes));
  Serial.println(endLine);
  if (linkEspNowReady) {
    AxdFileEntryMsg endMsg;
    endMsg.index = 255;  // end sentinel
    endMsg.count = static_cast<uint8_t>(n);
    endMsg.size = static_cast<uint32_t>(fileTotalBytes);
    esp_now_send(kLinkBroadcastAddr, reinterpret_cast<uint8_t*>(&endMsg), sizeof(endMsg));
  }
#endif
  Serial.printf("[files] streamed %d file(s) listing\n", n);
}

void linkStreamFileData(uint8_t index) {
  g_fileStreamAborted = false;
  if (!ensureSdCard()) {
    const char* err = "$FILEERR,SD_UNAVAILABLE";
    Serial.println(err);
#ifdef AWOK_HEADLESS
    bridgeNotifyResult(kSourceFiles, reinterpret_cast<const uint8_t*>(err), strlen(err));
#else
    if (linkEnsureEspNow()) {
      esp_wifi_set_channel(kLinkChannel, WIFI_SECOND_CHAN_NONE);
      AxdFileDoneMsg done;
      done.status = 1;
      esp_now_send(kLinkBroadcastAddr, reinterpret_cast<uint8_t*>(&done), sizeof(done));
    }
#endif
    return;
  }

  if (fileRowCount == 0 || index >= fileRowCount) {
    scanSdFiles();
  }
  if (index >= fileRowCount) {
    const char* err = "$FILEERR,FILE_NOT_FOUND";
    Serial.println(err);
#ifdef AWOK_HEADLESS
    bridgeNotifyResult(kSourceFiles, reinterpret_cast<const uint8_t*>(err), strlen(err));
#else
    if (linkEnsureEspNow()) {
      esp_wifi_set_channel(kLinkChannel, WIFI_SECOND_CHAN_NONE);
      AxdFileDoneMsg done;
      done.status = 1;
      esp_now_send(kLinkBroadcastAddr, reinterpret_cast<uint8_t*>(&done), sizeof(done));
    }
#endif
    return;
  }

  const String path = fileFullPath(fileRows[index].name);
  const String base = fileBaseName(fileRows[index].name);
  // A live wardrive keeps its writer open; publish buffered bytes before reading.
  if (path == g_wardriveCsvPath) flushWardriveCsv();
  File file = SD.open(path.c_str(), FILE_READ);
  if (!file) {
    char err[64];
    snprintf(err, sizeof(err), "$FILEERR,CANNOT_OPEN,%s", base.c_str());
    Serial.println(err);
#ifdef AWOK_HEADLESS
    bridgeNotifyResult(kSourceFiles, reinterpret_cast<const uint8_t*>(err), strlen(err));
#else
    if (linkEnsureEspNow()) {
      esp_wifi_set_channel(kLinkChannel, WIFI_SECOND_CHAN_NONE);
      AxdFileDoneMsg done;
      done.status = 1;
      esp_now_send(kLinkBroadcastAddr, reinterpret_cast<uint8_t*>(&done), sizeof(done));
    }
#endif
    return;
  }

  const uint32_t fileSize = file.size();
  constexpr size_t kRawChunkSize = 96;
  const uint16_t totalChunks = (fileSize == 0) ? 1 : static_cast<uint16_t>((fileSize + kRawChunkSize - 1) / kRawChunkSize);

  Serial.printf("[files] streaming %s (%lu bytes, %u chunks)\n",
                base.c_str(), static_cast<unsigned long>(fileSize), totalChunks);

#ifndef AWOK_HEADLESS
  if (!linkEnsureEspNow()) {
    file.close();
    return;
  }
  esp_wifi_set_channel(kLinkChannel, WIFI_SECOND_CHAN_NONE);
#endif

  uint8_t rawBuf[kRawChunkSize];
  char b64Buf[136];
  uint16_t seq = 0;

  if (fileSize == 0) {
    char line[64];
    snprintf(line, sizeof(line), "$FILEDATA,1,1,");
    Serial.println(line);
#ifdef AWOK_HEADLESS
    bridgeNotifyResult(kSourceFiles, reinterpret_cast<const uint8_t*>(line), strlen(line));
#else
    if (linkEspNowReady) {
      AxdFileDataMsg dmsg;
      dmsg.chunkSeq = 1;
      dmsg.totalChunks = 1;
      dmsg.dataLen = 0;
      esp_now_send(kLinkBroadcastAddr, reinterpret_cast<uint8_t*>(&dmsg), sizeof(dmsg));
    }
#endif
  } else {
    while (file.available() && !g_fileStreamAborted) {
      seq++;
      const size_t bytesRead = file.read(rawBuf, sizeof(rawBuf));
      if (bytesRead == 0) break;
      encodeBase64Chunk(rawBuf, bytesRead, b64Buf, sizeof(b64Buf));

      char line[180];
      snprintf(line, sizeof(line), "$FILEDATA,%u,%u,%s", seq, totalChunks, b64Buf);
      Serial.println(line);

#ifdef AWOK_HEADLESS
      bridgeNotifyResult(kSourceFiles, reinterpret_cast<const uint8_t*>(line), strlen(line));
#else
      if (linkEspNowReady) {
        AxdFileDataMsg dmsg;
        dmsg.chunkSeq = seq;
        dmsg.totalChunks = totalChunks;
        dmsg.dataLen = static_cast<uint8_t>(strlen(b64Buf));
        strncpy(dmsg.data, b64Buf, sizeof(dmsg.data) - 1);
        esp_now_send(kLinkBroadcastAddr, reinterpret_cast<uint8_t*>(&dmsg), sizeof(dmsg));
      }
#endif
      delay(30);
    }
  }

  file.close();
  delay(30);

  char doneLine[80];
  if (g_fileStreamAborted) {
    snprintf(doneLine, sizeof(doneLine), "$FILEABORT,%s", base.c_str());
  } else {
    snprintf(doneLine, sizeof(doneLine), "$FILEDONE,%s,%lu",
             base.c_str(), static_cast<unsigned long>(fileSize));
  }
  Serial.println(doneLine);

#ifdef AWOK_HEADLESS
  bridgeNotifyResult(kSourceFiles, reinterpret_cast<const uint8_t*>(doneLine), strlen(doneLine));
#else
  if (linkEspNowReady) {
    AxdFileDoneMsg done;
    done.status = g_fileStreamAborted ? 2 : 0;
    done.totalBytes = fileSize;
    strncpy(done.name, base.c_str(), sizeof(done.name) - 1);
    esp_now_send(kLinkBroadcastAddr, reinterpret_cast<uint8_t*>(&done), sizeof(done));
  }
#endif
}

// Only an ACK from the browser advances the SD reader. Receiving at the bridge
// or successfully queueing a BLE notification alone does not count as delivery.
static std::atomic<uint32_t> fileReliableToken{0};
static std::atomic<uint32_t> fileReliableWaitingSeq{0};

bool linkReliableFileActive() { return fileReliableToken.load() != 0; }

void linkReceiveFileAck(uint32_t token, uint32_t seq) {
  if (token == 0 || token != fileReliableToken.load()) return;
  uint32_t expected = seq;
  fileReliableWaitingSeq.compare_exchange_strong(expected, 0);
}

static bool fileSendReliable(AxdFileChunkMsg& chunk) {
  fileReliableWaitingSeq.store(chunk.seq);
  for (int attempt = 0; attempt < kFileChunkAttempts && !g_fileStreamAborted; ++attempt) {
    esp_wifi_set_channel(kLinkChannel, WIFI_SECOND_CHAN_NONE);
    const esp_err_t sent = esp_now_send(kLinkBroadcastAddr, reinterpret_cast<uint8_t*>(&chunk), sizeof(chunk));
    if (sent != ESP_OK) {
      Serial.printf("[files] ESP-NOW enqueue failed: seq=%lu error=%d\n",
                    static_cast<unsigned long>(chunk.seq), static_cast<int>(sent));
    }
    const uint32_t started = millis();
    while (millis() - started < kFileChunkRetryMs && !g_fileStreamAborted) {
      if (fileReliableWaitingSeq.load() == 0) return true;
      delay(1);  // ACKs arrive in onLinkRecv, independent of the blocked main loop
    }
    Serial.printf("[files] retry seq=%lu attempt=%d\n",
                  static_cast<unsigned long>(chunk.seq), attempt + 1);
  }
  return false;
}

void linkStreamFileReliable(uint8_t index, uint32_t token) {
  g_fileStreamAborted = false;
  fileReliableToken.store(token);
  AxdFileChunkMsg chunk;
  chunk.token = token;
  chunk.seq = 1;
  File file;
  String base;
  const char* error = nullptr;
  char errorDetail[96] = {};
  if (!ensureSdCard()) error = "SD unavailable";
  if (!error && (fileRowCount == 0 || index >= fileRowCount)) scanSdFiles();
  if (!error && index >= fileRowCount) error = "File not found";
  if (!error) {
    const String path = fileFullPath(fileRows[index].name);
    base = fileBaseName(fileRows[index].name);
    if (path == g_wardriveCsvPath) flushWardriveCsv();
    file = SD.open(path.c_str(), FILE_READ);
    if (!file) error = "Cannot open file";
  }
  if (!error) {
    chunk.totalBytes = file.size();
    Serial.printf("[files] reliable download: %s, %lu bytes, token=%lu\n",
                  base.c_str(), static_cast<unsigned long>(chunk.totalBytes),
                  static_cast<unsigned long>(token));
    uint32_t remaining = chunk.totalBytes;
    uint8_t raw[kFileChunkBytes];
    do {
      const size_t wanted = remaining < sizeof(raw) ? remaining : sizeof(raw);
      const size_t got = wanted ? file.read(raw, wanted) : 0;
      if (got != wanted) { error = "SD read failed"; break; }
      encodeBase64Chunk(raw, got, chunk.data, sizeof(chunk.data));
      if (!fileSendReliable(chunk)) {
        snprintf(errorDetail, sizeof(errorDetail), "No browser ACK for chunk %lu after %d attempts",
                 static_cast<unsigned long>(chunk.seq), kFileChunkAttempts);
        error = errorDetail;
        break;
      }
      remaining -= got;
      ++chunk.seq;
    } while (remaining && !g_fileStreamAborted);
    file.close();
  }
  if (!g_fileStreamAborted) {
    chunk.kind = error ? 2 : 1;
    strncpy(chunk.data, error ? error : base.c_str(), sizeof(chunk.data) - 1);
    // Use a distinct sequence for an error after a timed-out data chunk, so a
    // delayed data ACK cannot acknowledge the error/completion message.
    if (error) ++chunk.seq;
    fileSendReliable(chunk);
  }
  fileReliableToken.store(0);
  fileReliableWaitingSeq.store(0);
}

void linkDeleteFile(uint8_t index) {
  if (!ensureSdCard() || index >= fileRowCount) {
    const char* err = "$FILEDELETE,failed";
    Serial.println(err);
#ifdef AWOK_HEADLESS
    bridgeNotifyResult(kSourceFiles, reinterpret_cast<const uint8_t*>(err), strlen(err));
#else
    if (linkEnsureEspNow()) {
      esp_wifi_set_channel(kLinkChannel, WIFI_SECOND_CHAN_NONE);
      AxdFileDoneMsg done;
      done.status = 1;
      esp_now_send(kLinkBroadcastAddr, reinterpret_cast<uint8_t*>(&done), sizeof(done));
    }
#endif
    return;
  }
  const String path = fileFullPath(fileRows[index].name);
  const String base = fileBaseName(fileRows[index].name);
  const bool removed = SD.remove(path.c_str());
  Serial.printf("[files] remote delete %s: %s\n", base.c_str(), removed ? "success" : "failed");
  recordFirmwareAudit("storage", "remote_file_delete",
                      removed ? "success" : "failed", "path=" + path);
  char line[80];
  snprintf(line, sizeof(line), "$FILEDELETE,%s,%s", removed ? "ok" : "failed", base.c_str());
  Serial.println(line);
#ifdef AWOK_HEADLESS
  bridgeNotifyResult(kSourceFiles, reinterpret_cast<const uint8_t*>(line), strlen(line));
#else
  if (linkEnsureEspNow()) {
    esp_wifi_set_channel(kLinkChannel, WIFI_SECOND_CHAN_NONE);
    AxdFileDoneMsg done;
    done.status = removed ? 0 : 1;
    strncpy(done.name, base.c_str(), sizeof(done.name) - 1);
    esp_now_send(kLinkBroadcastAddr, reinterpret_cast<uint8_t*>(&done), sizeof(done));
  }
#endif
  linkStreamFileList();
}

void handleFilesTouch(int x, int y) {
  if (y < kFooterTop) {
    if (y >= 48) {
      const int row = (y - 48) / 22;
      const int idx = filePage * kVisibleRows + row;
      if (row >= 0 && row < kVisibleRows && idx < fileRowCount) {
        fileSelected = idx;
        fileConfirmDelete = false;
        drawFilesManager();
      }
    }
    return;
  }
  const int pages = filePageCount();
  if (x < 60) {
    drawStatus();
  } else if (x < 120) {
    filePage = (filePage - 1 + pages) % pages;
    fileSelected = -1;
    fileConfirmDelete = false;
    drawFilesManager();
  } else if (x < 180) {
    filePage = (filePage + 1) % pages;
    fileSelected = -1;
    fileConfirmDelete = false;
    drawFilesManager();
  } else if (fileSelected >= 0) {
    if (!fileConfirmDelete) {
      fileConfirmDelete = true;
      drawFilesManager();
    } else {
      const String path = fileFullPath(fileRows[fileSelected].name);
      const bool removed = SD.remove(path.c_str());
      Serial.printf("[files] %s %s\n", removed ? "deleted" : "delete failed",
                    fileRows[fileSelected].name.c_str());
      recordFirmwareAudit("storage", "file_delete",
                          removed ? "success" : "failed", "path=" + path);
      openFilesManager();
    }
  } else {
    openFilesManager();  // rescan when nothing is selected
  }
}
