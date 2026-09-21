// AWOKxDAG — touch + serial input dispatch (compiled as part of the sketch; see awok_common.h)

bool readTouch(int& screenX, int& screenY) {
#ifdef AWOK_MINI_DISPLAY
  return digitalRead(AwokPins::kButtonCenter) == LOW &&
         display.selection(screenX, screenY);
#else
  if (!touch.touched()) return false;
  TS_Point point = touch.getPoint();
  if (point.z < AwokTouchCalibration::kPressureMin) return false;
  screenX = constrain(map(point.x, AwokTouchCalibration::kXMin,
                          AwokTouchCalibration::kXMax, 0, kScreenWidth - 1),
                      0, kScreenWidth - 1);
  screenY = constrain(map(point.y, AwokTouchCalibration::kYMin,
                          AwokTouchCalibration::kYMax, 0, kScreenHeight - 1),
                      0, kScreenHeight - 1);
  return true;
#endif
}

#ifdef AWOK_MINI_DISPLAY
bool miniAnyButtonDown() {
  return digitalRead(AwokPins::kButtonLeft) == LOW ||
         digitalRead(AwokPins::kButtonCenter) == LOW ||
         digitalRead(AwokPins::kButtonUp) == LOW ||
         digitalRead(AwokPins::kButtonRight) == LOW ||
         digitalRead(AwokPins::kButtonDown) == LOW;
}

void updateMiniJoystick() {
  static uint32_t nextMove = 0;
  static int lastDirection = 0;
  const uint32_t now = millis();
  if (backlightDimmed) {
    if (miniAnyButtonDown()) noteActivity();
    return;
  }
  if (currentView == View::kScreenTest) {
    if (digitalRead(AwokPins::kButtonLeft) == LOW &&
        static_cast<int32_t>(now - nextMove) >= 0) {
      nextMove = now + 300;
      noteActivity();
      stopScreenTest();
    } else if ((digitalRead(AwokPins::kButtonRight) == LOW ||
                digitalRead(AwokPins::kButtonDown) == LOW) &&
               static_cast<int32_t>(now - nextMove) >= 0) {
      nextMove = now + 300;
      noteActivity();
      screenTestAdvance();
    }
    return;
  }
  // Holding center never moves selection into another control.
  if (digitalRead(AwokPins::kButtonCenter) == LOW) return;
  const int direction = digitalRead(AwokPins::kButtonLeft) == LOW ? -2 :
      digitalRead(AwokPins::kButtonRight) == LOW ? 2 :
      digitalRead(AwokPins::kButtonUp) == LOW ? -1 :
      digitalRead(AwokPins::kButtonDown) == LOW ? 1 : 0;
  if (!direction) { lastDirection = 0; return; }
  const bool first = direction != lastDirection;
  if (first || static_cast<int32_t>(now - nextMove) >= 0) {
    display.navigate(direction == 2 ? 1 : direction == -2 ? -1 : direction,
                     abs(direction) == 2);
    nextMove = now + (first ? 350 : 150);
    lastDirection = direction;
  }
}
#endif

// A held contact is one press, even if it spans multiple redraws. In
// particular, Delete -> OK must require releasing and pressing again.
bool consumeTouchPress(bool pressed, uint32_t now) {
  constexpr uint32_t kTouchDebounceMs = 120;
  static bool wasPressed = false;
  const bool newPress = pressed && !wasPressed;
  wasPressed = pressed;
  if (!newPress || now - lastTouchMs < kTouchDebounceMs) return false;
  lastTouchMs = now;
  return true;
}

void handleTouch() {
  if (scanInProgress) return;
  int x = 0;
  int y = 0;
#ifdef AWOK_MINI_DISPLAY
  const bool pressed = currentView == View::kScreenTest
                           ? digitalRead(AwokPins::kButtonCenter) == LOW
                           : readTouch(x, y);
  if (currentView == View::kScreenTest) {
    x = 120;
    y = 8;
  }
#else
  const bool pressed = readTouch(x, y);
#endif
  if (backlightDimmed) {
    if (consumeTouchPress(pressed, millis())) noteActivity();
    return;
  }
  if (!consumeTouchPress(pressed, millis())) return;
  noteActivity();
  Serial.printf("[touch] x=%d y=%d\n", x, y);
  if (currentView == View::kScreenTest) {
    handleScreenTestTouch(x, y);
    return;
  }
  if (networkToolsOpen()) {
    handleNetworkTouch(x, y);
    return;
  }
  if (currentView == View::kHome) {
    if (homePage == 1) {
      homePage = 0;  // any tap on the About page returns to the tiles
      drawHome();
      return;
    }
    if (y >= 44 && y < 84) {
      drawReconMenu();
    } else if (y >= 88 && y < 128) {
      drawAttacksMenu();
    } else if (y >= 132 && y < 172) {
      drawMonitorMenu();
    } else if (y >= 176 && y < 216) {
      drawGps();
    } else if (y >= 220 && y < 260) {
      drawStatus();
    } else if (y >= kFooterTop) {
      homePage = 1;  // footer opens the About page (page 2)
      drawHome();
    }
    return;
  }
  if (currentView == View::kStatus) {
    handleStatusTouch(x, y);
    return;
  }
  if (currentView == View::kSettings) {
    handleSettingsTouch(x, y);
    return;
  }
  if (currentView == View::kFiles) {
    handleFilesTouch(x, y);
    return;
  }
  if (currentView == View::kRecon) {
    if (y < kFooterTop) {
      const int start = reconPage * kMenuPerPage;
      for (int row = 0; row < kMenuPerPage; ++row) {
        const int index = start + row;
        if (index >= kReconItemCount) break;
        const int by = kMenuFirstY + row * kMenuRowPitch;
        if (y >= by && y < by + kMenuRowHeight) {
          launchReconItem(index);
          return;
        }
      }
      return;
    }
    const int pages = reconPageCount();
    if (pages <= 1) {
      drawHome();
    } else if (x < 80) {
      drawHome();
    } else if (x < 160) {
      reconPage = (reconPage - 1 + pages) % pages;
      drawReconMenu();
    } else {
      reconPage = (reconPage + 1) % pages;
      drawReconMenu();
    }
    return;
  }
  if (currentView == View::kMonitor) {
    if (y < kFooterTop) {
      const int start = monitorPage * kMenuPerPage;
      for (int row = 0; row < kMenuPerPage; ++row) {
        const int index = start + row;
        if (index >= kMonitorItemCount) break;
        const int by = kMenuFirstY + row * kMenuRowPitch;
        if (y >= by && y < by + kMenuRowHeight) {
          launchMonitorItem(index);
          return;
        }
      }
      return;
    }
    const int pages = monitorPageCount();
    if (pages <= 1) {
      drawHome();
    } else if (x < 80) {
      drawHome();
    } else if (x < 160) {
      monitorPage = (monitorPage - 1 + pages) % pages;
      drawMonitorMenu();
    } else {
      monitorPage = (monitorPage + 1) % pages;
      drawMonitorMenu();
    }
    return;
  }
  if (currentView == View::kWifi && y >= 44 && y < 264) {
    const int row = (y - 44) / 22;
    const int index = wifiPage * kVisibleRows + row;
    if (row < kVisibleRows && index < wifiCount) {
      openWifiAudit(wifiEntries[index], View::kWifi);
    }
    return;
  }
  if (currentView == View::kSaved && y >= 44 && y < 264) {
    const int index = (y - 44) / 22;
    if (index < savedCount) openWifiAudit(savedEntries[index], View::kSaved);
    return;
  }
  if (currentView == View::kBle && y >= 44 && y < 264) {
    const int index = bleResultIndex((y - 44) / 22);
    if (index >= 0) {
      openBleDetail(bleEntries[index]);
    }
    return;
  }
  if (currentView == View::kDeauthSelect && y >= 44 && y < 264) {
    const int index = (y - 44) / 22;
    if (index < min(wifiCount, kVisibleRows)) {
      toggleDeauthTarget(wifiEntries[index]);
      drawDeauthSelect();
    }
    return;
  }
  if (currentView == View::kAttacks && y < kFooterTop) {
    if (y >= 44 && y < 82) {
      startBeaconFlood();
    } else if (y >= 86 && y < 124) {
      startEvilPortalPlain();
    } else if (y >= 128 && y < 166) {
      startEvilTwin();
    } else if (y >= 170 && y < 208) {
      startProbeLure();
    }
    return;
  }
  if (y < kFooterTop) return;
  if (currentView == View::kPacketMon) {
    stopPacketMon();
    drawReconMenu();
    return;
  }
  if (currentView == View::kCameraScan) {
    if (x < kScreenWidth / 2) {
      stopCameraScan();
      drawReconMenu();
    } else {
      cameraCount = 0;
      drawCameraScan();
    }
    return;
  }
  if (currentView == View::kLocator) {
    if (x < 80) {
      stopLocator();
      drawWifiAudit();
    } else if (x < 160) {
      startLocator();  // reset the hunt
    } else {
      stopLocator();
      startFleetHunt();
    }
    return;
  }
  if (currentView == View::kFleetHunt) {
    if (x < 80) {
      stopFleetHunt();
      drawWifiAudit();
    } else if (x < 160) {
      startFleetHunt();  // reset the hunt
    } else {
      stopFleetHunt();
      startLocator();
    }
    return;
  }
  if (currentView == View::kWpsScan) {
    stopWpsScan();
    drawReconMenu();
    return;
  }
  if (currentView == View::kRogueWatch) {
    stopRogueWatch();
    drawMonitorMenu();
    return;
  }
  if (currentView == View::kHiddenReveal) {
    stopHiddenReveal();
    drawReconMenu();
    return;
  }
  if (currentView == View::kSecurityAudit) {
    if (x < kScreenWidth / 2) {
      stopSecurityAudit();
      drawReconMenu();
    } else {
      lastAuditCsvOk = exportSecurityAuditToSd();
      drawSecurityAudit();
    }
    return;
  }
  if (currentView == View::kTrackerScan) {
    if (x < kScreenWidth / 2) {
      stopTrackerScan();
      drawReconMenu();
    } else {
      lastTrackerCsvOk = exportTrackersToSd();
      drawTrackerScan();
    }
    return;
  }
  if (currentView == View::kBleIntel) {
    if (x < kScreenWidth / 2) {
      stopBleIntel();
      drawReconMenu();
    } else {
      lastBleIntelCsvOk = exportBleIntelToSd();
      drawBleIntel();
    }
    return;
  }
  if (currentView == View::kSpectrogram) {
    if (y >= kFooterTop) {
      const int zone = x / (kScreenWidth / 4);
      if (zone <= 0) {
        stopSpectrogram();
        drawReconMenu();
      } else if (zone == 1) {
        spectrogramLockStep(-1);
      } else if (zone == 2) {
        spectrogramLockStep(1);
      } else {
        spectrogramCycleBand();
      }
    } else if (y < 56 && x >= 168) {
      spectrogramCycleBand();
    } else if (y >= 58 && y <= 120) {
      const int total = specTotalChannels();
      const float span = (total > 1) ? (float)(total - 1) : 1.0f;
      int idx = (int)((float)(x - 8) * span / 223.0f + 0.5f);
      spectrogramLockToIndex(specBandBase() + idx);
    }
    return;
  }
  if (currentView == View::kHarvester) {
    if (x < kScreenWidth / 2) {
      stopHarvester();
      drawReconMenu();
    } else {
      resetHarvester();
      drawHarvester();
    }
    return;
  }
  if (currentView == View::kProbeIntel) {
    if (x < kScreenWidth / 2) {
      stopProbeIntel();
      drawReconMenu();
    } else {
      lastProbeIntelCsvOk = exportProbeIntelToSd();
      drawProbeIntel();
    }
    return;
  }
  if (currentView == View::kTopologyMap) {
    if (x < kScreenWidth / 2) {
      stopTopologyMap();
      drawReconMenu();
    } else {
      lastTopologyCsvOk = exportTopologyToSd();
      drawTopologyMap();
    }
    return;
  }
  if (currentView == View::kKarmaWatch) {
    if (x < kScreenWidth / 2) {
      stopKarmaWatch();
      drawMonitorMenu();
    } else {
      resetKarmaWatch();
      drawKarmaWatch();
    }
    return;
  }
  if (currentView == View::kBeaconWatch) {
    if (x < kScreenWidth / 2) {
      stopBeaconWatch();
      drawMonitorMenu();
    } else {
      resetBeaconWatch();
      drawBeaconWatch();
    }
    return;
  }
  if (currentView == View::kAuthFlood) {
    if (x < kScreenWidth / 2) {
      stopAuthFlood();
      drawMonitorMenu();
    } else {
      resetAuthFlood();
      drawAuthFlood();
    }
    return;
  }
  if (currentView == View::kAdvancedWatch) {
    if (x < kScreenWidth / 2) {
      stopAdvancedWatch();
      drawMonitorMenu();
    } else {
      resetAdvancedWatch();
      drawAdvancedWatch();
    }
    return;
  }
  if (currentView == View::kDeauthMonitor) {
    if (x < kScreenWidth / 2) {
      stopDeauthMonitor();
      drawHome();
    } else {
      deauthFrameCount = 0;
      disassocFrameCount = 0;
      deauthEventsSinceDraw = 0;
      haveDeauthHit = false;
      drawDeauthMonitor();
    }
    return;
  }
  if (currentView == View::kDeauthAttack) {
    if (x < kScreenWidth / 2) {
      stopDeauthAttack();
      if (deauthAttackReturnView == View::kDeauthSelect) {
        drawDeauthSelect();
      } else {
        drawWifiAudit();
      }
    } else {
      if (deauthAttackActive) {
        stopDeauthAttack();
        drawDeauthAttack();
      } else if (deauthTargetCount > 0) {
        startDeauthAttack();
      }
    }
    return;
  }
  if (currentView == View::kHandshake) {
    if (x < kScreenWidth / 2) {
      stopHandshakeCapture();
      drawWifiAudit();
    } else {
      handshakePulseEnabled = !handshakePulseEnabled;
      recordFirmwareAudit(
          "active_test", "handshake_deauth_pulse", "success",
          handshakePulseEnabled ? "enabled" : "disabled");
      drawHandshake();
    }
    return;
  }
  if (currentView == View::kClientSniffer) {
    if (x < kScreenWidth / 2) {
      stopClientSniffer();
      drawHome();
    } else {
      lastClientCsvOk = exportClientsToSd();
      drawClientSniffer();
    }
    return;
  }
  if (currentView == View::kAttacks) {
    drawHome();
    return;
  }
  if (currentView == View::kGps) {
    if (x < 60) {
      drawHome();
    } else if (x < 120) {
      cycleGpsBaud();
      drawGps();
    } else if (x < 180) {
      startWardrive();
    } else {
      openLinkWardrive();
    }
    return;
  }
  if (currentView == View::kWardrive) {
    if (x < kScreenWidth / 2) {
      stopWardrive();
      drawGps();
    } else {
      stopWardrive();
      drawHome();
    }
    return;
  }
  if (currentView == View::kLinkWardrive) {
    // Fleet screens render over the Link view and take touch first.
    if (fleetActive || fleetListening) {
      const bool joining = fleetListening && !fleetActive;
      if (fleetCoordinator && !joining) {  // Home / Leave / Go|Stop
        if (x < 80) {
          drawHome();  // fleet keeps running in the background
        } else if (x < 160) {
          fleetLeave();
          drawLinkWardrive();
        } else {
          if (fleetWardriveOn) fleetStopWardrive(); else fleetStartWardrive();
          drawLinkWardrive();
        }
      } else {  // member / joining: Leave / Home
        if (x < kScreenWidth / 2) {
          fleetLeave();
          drawLinkWardrive();
        } else {
          drawHome();
        }
      }
      return;
    }
    if (fleetMenuOpen) {  // Back / Start / Join
      if (x < 80) {
        fleetMenuOpen = false;
        drawLinkWardrive();
      } else if (x < 160) {
        fleetStartWardrive();
        drawLinkWardrive();
      } else {
        fleetArm();
        drawLinkWardrive();
      }
      return;
    }
    if (linkWardriveActive) {
      stopLinkWardrive();
      if (x < kScreenWidth / 2) {
        drawLinkWardrive();
      } else {
        drawHome();
      }
      return;
    }
    if (linkState == kLinkDiscovering) {
      linkCancelPairing();
      drawLinkWardrive();
    } else if (linkState == kLinkAwaitConfirm) {
      if (x < kScreenWidth / 2) {
        linkCancelPairing();
        drawLinkWardrive();
      } else {
        linkConfirm();
      }
    } else if (linkState == kLinkReady) {
      if (x < 80) {
        drawGps();
      } else if (x < 160) {
        linkUnpair();
        drawLinkWardrive();
      } else {
        startLinkWardrive();
      }
    } else {  // kLinkOff — unpaired idle
      if (x < 80) {
        drawGps();
      } else if (x < 160) {
        fleetMenuOpen = true;   // Fleet supersedes the legacy 1:1 pair flow
        drawLinkWardrive();
      } else {
        startLinkWardrive();
      }
    }
    return;
  }
  if (currentView == View::kBeaconFlood) {
    if (x < kScreenWidth / 2) {
      stopBeaconFlood();
      drawAttacksMenu();
    } else if (beaconFloodActive) {
      stopBeaconFlood();
      drawBeaconFlood();
    } else {
      startBeaconFlood();
    }
    return;
  }
  if (currentView == View::kBleSpamWatch) {
    if (x < kScreenWidth / 2) {
      stopBleDetect();
      drawHome();
    } else {
      bleDetectTotal = 0;
      bleDetectSpam = 0;
      bleDetectPeakRate = 0;
      bleDetectRate = 0;
      bleDetectAlert = false;
      bleDetectLastVendor = 0;
      drawBleDetect();
    }
    return;
  }
  if (currentView == View::kProbeLure) {
    if (x < kScreenWidth / 2) {
      stopProbeLure();
      drawAttacksMenu();
    } else if (probeLureActive) {
      stopProbeLure();
      drawProbeLure();
    } else {
      startProbeLure();
    }
    return;
  }
  if (currentView == View::kEvilPortal) {
    if (x < kScreenWidth / 2) {
      stopEvilPortal();
      drawAttacksMenu();
    } else {
      portalCredsCount = 0;
      lastPortalCred = "";
      const bool removed = !SD.exists(kPortalCredsPath) ||
                           SD.remove(kPortalCredsPath);
      recordFirmwareAudit("storage", "portal_submission_log_clear",
                          removed ? "success" : "failed",
                          String("path=") + kPortalCredsPath);
      portalLogReady = openPortalLog();
      drawEvilPortal();
    }
    return;
  }
  if (currentView == View::kDeauthSelect) {
    if (x < 80) {
      drawWifiResults();
    } else if (x < 160) {
      deauthTargetCount = 0;
      drawDeauthSelect();
    } else if (deauthTargetCount > 0) {
      deauthAttackReturnView = View::kDeauthSelect;
      openDeauthAttackView();
    }
    return;
  }
  if (currentView == View::kWifi) {
    const int pages = max(1, (wifiCount + kVisibleRows - 1) / kVisibleRows);
    if (pages > 1) {
      if (x < 48) {
        drawHome();
      } else if (x < 96) {
        wifiPage = (wifiPage - 1 + pages) % pages;
        drawWifiResults();
      } else if (x < 144) {
        wifiPage = (wifiPage + 1) % pages;
        drawWifiResults();
      } else if (x < 192) {
        drawDeauthSelect();
      } else {
        startWifiScanContinuous();
      }
    } else {
      if (x < 80) {
        drawHome();
      } else if (x < 160) {
        drawDeauthSelect();
      } else {
        startWifiScanContinuous();
      }
    }
    return;
  }
  if (currentView == View::kWifiMonitor) {
    if (x < 80) {
      signalMonitorActive = false;
      drawWifiAudit();
    } else if (x < 160) {
      beginWifiSignalMonitor();
    } else {
      signalMonitorActive = false;
      startLocator();
    }
    return;
  }
  if (currentView == View::kBle) {
    const int pages = blePageCount();
    if (pages > 1) {
      if (x < 60) {
        drawHome();
      } else if (x < 120) {
        blePage = (blePage - 1 + pages) % pages;
        drawBleResults();
      } else if (x < 180) {
        blePage = (blePage + 1) % pages;
        drawBleResults();
      } else {
        scanBle();
      }
    } else if (x < kScreenWidth / 2) {
      drawHome();
    } else {
      scanBle();
    }
    return;
  }
  if (currentView == View::kBleDetail) {
    if (x < kScreenWidth / 2) {
      drawBleResults();
    } else {
      scanBle();
    }
    return;
  }
  if (currentView == View::kWifiAudit) {
    if (x < 48) {
      if (auditReturnView == View::kSaved) {
        drawSavedNetworks();
      } else {
        drawWifiResults();
      }
    } else if (x < 96) {
      beginWifiSignalMonitor();
    } else if (x >= 192) {
      grabHandshake();
    } else if (x >= 144) {
      openDeauthAttackSingle();
    } else {
      const bool wasSaved = isSaved(selectedWifi);
      if (toggleSavedNetwork(selectedWifi)) {
        if (lastSavedSdWriteOk) {
          auditStatus = wasSaved ? "Removed; SD mirror updated."
                                 : "Saved to device + SD card.";
        } else if (sdReady) {
          auditStatus = wasSaved ? "Removed; SD export failed."
                                 : "Saved; SD export failed.";
        } else {
          auditStatus = wasSaved ? "Removed; SD card unavailable."
                                 : "Saved; insert SD to export.";
        }
      } else {
        auditStatus = savedCount >= kMaxSaved ? "Saved list is full."
                                              : "Storage write failed.";
      }
      drawWifiAudit();
    }
    return;
  }
  if (x < kScreenWidth / 2) {
    drawHome();
  } else if (currentView == View::kChannels) {
    scanWifiForChannelMap();
  } else if (currentView == View::kSaved) {
    scanWifi();
  }
}

bool toolBlocksSerialShortcuts() {
  return deauthAttackActive || deauthMonitorActive || handshakeCaptureActive ||
         clientSnifferActive || beaconFloodActive || evilPortalActive ||
         wardriveActive || pktmonActive || wpsScanActive || rogueWatchActive ||
         hiddenRevealActive || cameraActive || bleDetectActive ||
         probeLureActive || securityAuditActive || trackerScanActive ||
         bleIntelActive || spectrogramActive || harvesterActive || probeIntelActive || karmaWatchActive ||
         beaconWatchActive || authFloodActive || advancedWatchActive ||
         locatorActive || fleetHuntActive || topologyActive || linkWardriveActive ||
         linkState == kLinkDiscovering || linkState == kLinkAwaitConfirm;
}

void stopActiveTools() {
  stopWifiScanContinuous();
  if (deauthAttackActive) stopDeauthAttack();
  if (deauthMonitorActive) stopDeauthMonitor();
  if (handshakeCaptureActive) stopHandshakeCapture();
  if (clientSnifferActive) stopClientSniffer();
  if (beaconFloodActive) stopBeaconFlood();
  if (evilPortalActive) stopEvilPortal();
  if (wardriveActive) stopWardrive();
  if (pktmonActive) stopPacketMon();
  if (wpsScanActive) stopWpsScan();
  if (rogueWatchActive) stopRogueWatch();
  if (hiddenRevealActive) stopHiddenReveal();
  if (cameraActive) stopCameraScan();
  if (bleDetectActive) stopBleDetect();
  if (probeLureActive) stopProbeLure();
  if (securityAuditActive) stopSecurityAudit();
  if (trackerScanActive) stopTrackerScan();
  if (bleIntelActive) stopBleIntel();
  if (spectrogramActive) stopSpectrogram();
  if (harvesterActive) stopHarvester();
  if (probeIntelActive) stopProbeIntel();
  if (karmaWatchActive) stopKarmaWatch();
  if (beaconWatchActive) stopBeaconWatch();
  if (authFloodActive) stopAuthFlood();
  if (advancedWatchActive) stopAdvancedWatch();
  if (locatorActive) stopLocator();
  if (fleetHuntActive) stopFleetHunt();
  if (topologyActive) stopTopologyMap();
  if (linkWardriveActive) stopLinkWardrive();
  if (linkState == kLinkDiscovering || linkState == kLinkAwaitConfirm) {
    linkCancelPairing();
  }
}

void handleSerial() {
  if (networkToolsOpen()) {
    handleNetworkSerial();
    return;
  }
  if (!Serial.available() || scanInProgress) return;
  noteActivity();
  const char command = static_cast<char>(tolower(Serial.read()));
  if (toolBlocksSerialShortcuts()) {
    if (command == 'h') {
      stopActiveTools();
      drawHome();
    }
    return;
  }
  if (command == 'm') startDeauthMonitor();
  if (command == 'p') startClientSniffer();
  if (command == 'k') startPacketMon();
  if (command == 'g') drawGps();
  if (command == 'n') openLinkWardrive();
  if (command == 'u') {
    cycleGpsBaud();
    if (currentView == View::kGps) drawGps();
    if (currentView == View::kSettings) drawSettings();
  }
  if (command == 't') drawSettings();
  if (command == 'r') {
    gpsRawEcho = !gpsRawEcho;
    setSettingFlag(kSettingNmeaEcho, gpsRawEcho);
    saveDeviceSettings();
    Serial.printf("\n[gps] raw echo %s\n", gpsRawEcho ? "ON" : "OFF");
    if (currentView == View::kSettings) drawSettings();
  }
  if (command == 'f') fleetStartWardrive();
  if (command == 'j') fleetArm();
  if (command == 'w') startWifiScanContinuous();
  if (command == 'c') {
    if (wifiCount) {
      drawChannelMap();
    } else {
      scanWifiForChannelMap();
    }
  }
  if (command == 'b') scanBle();
  if (command == 's') drawSavedNetworks();
  if (command == 'd') {
    initializeSdCard();
    if (sdReady) {
      initializeFirmwareAudit();
      recordFirmwareAudit("system", "sd_retry", "success", "card mounted");
      lastSavedSdWriteOk = exportSavedNetworksToSd();
      if (wifiCount) lastScanSdWriteOk = exportWifiScanToSd();
      if (bleCount) lastBleScanSdWriteOk = exportBleScanToSd();
    }
    drawHome();
  }
  if (command == 'h') drawHome();
}
