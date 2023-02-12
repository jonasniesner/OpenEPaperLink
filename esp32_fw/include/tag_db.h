#include <Arduino.h>
#include <ArduinoJson.h>

#include <vector>

#pragma pack(push, 1)
#pragma once

#define SOLUM_154_033 0
#define SOLUM_29_033 1
#define SOLUM_42_033 2

#define WAKEUP_REASON_TIMED 0
#define WAKEUP_REASON_BOOTUP 1
#define WAKEUP_REASON_GPIO 2
#define WAKEUP_REASON_NFC 3

class tagRecord {
   public:
    uint16_t nextCheckinpending;
    tagRecord() : mac{0}, alias(""), lastseen(0), nextupdate(0), contentMode(0), pending(false), md5{0}, md5pending{0}, CheckinInMinPending(0), expectedNextCheckin(0), modeConfigJson(""), LQI(0), RSSI(0), temperature(0), batteryMv(0), hwType(0), wakeupReason(0), capabilities(0) {}

    uint8_t mac[6];
    String alias;
    uint32_t lastseen;
    uint32_t nextupdate;
    uint8_t contentMode;
    bool pending;
    uint8_t md5[16];
    uint8_t md5pending[16];
    uint16_t CheckinInMinPending;
    uint32_t expectedNextCheckin;
    String modeConfigJson;
    uint8_t LQI;
    int8_t RSSI;
    int8_t temperature;
    uint16_t batteryMv;
    uint8_t hwType;
    uint8_t wakeupReason;
    uint8_t capabilities; 
    static tagRecord* findByMAC(uint8_t mac[6]);
};

extern std::vector<tagRecord*> tagDB;
String tagDBtoJson(uint8_t mac[6] = nullptr, uint8_t startPos = 0);
void fillNode(JsonObject &tag, tagRecord* &taginfo);
void saveDB(String filename);
void loadDB(String filename);

#pragma pack(pop)