#include "node_common.h"

void sensor_setup() {}

void sensor_update() {}

bool sensor_payload(char *buf, size_t n) {
  unsigned long uptime_s = millis() / 1000UL;
  unsigned long free_heap_kb = (unsigned long)(ESP.getFreeHeap() / 1024);
  if (wifi_up())
    snprintf(buf, n, "{\"uptime_s\":%lu,\"free_heap_kb\":%lu,\"rssi\":%d}", uptime_s, free_heap_kb, WiFi.RSSI());
  else
    snprintf(buf, n, "{\"uptime_s\":%lu,\"free_heap_kb\":%lu}", uptime_s, free_heap_kb);
  return true;
}

void setup() { node_setup(); }
void loop()  { node_loop(); }
