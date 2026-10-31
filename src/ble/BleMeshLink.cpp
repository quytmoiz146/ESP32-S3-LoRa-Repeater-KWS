#include "BleMeshLink.h"

#if defined(CONFIG_BLE_ENABLED) || defined(ROLE_NODE)

#include <NimBLEDevice.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

namespace {
constexpr char SERVICE_UUID[] = "6E400001-B5A3-F393-E0A9-E50E24DCCA9E";
constexpr char RX_UUID[]      = "6E400002-B5A3-F393-E0A9-E50E24DCCA9E";
constexpr char TX_UUID[]      = "6E400003-B5A3-F393-E0A9-E50E24DCCA9E";

struct CommandLine {
  char value[128];
};

QueueHandle_t commandQueue = nullptr;
NimBLECharacteristic* txCharacteristic = nullptr;
volatile bool clientConnected = false;

class ServerCallbacks final : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer*) override { 
    clientConnected = true; 
    Serial.println("[BLE] Client phone connected");
  }
  void onDisconnect(NimBLEServer*) override {
    clientConnected = false;
    Serial.println("[BLE] Client disconnected — restarting advertising");
    NimBLEDevice::startAdvertising();
  }
};

class RxCallbacks final : public NimBLECharacteristicCallbacks {
  char pending[sizeof(CommandLine::value)] = {};
  size_t used = 0;
  bool overflow = false;

  void onWrite(NimBLECharacteristic* characteristic) override {
    const std::string value = characteristic->getValue();
    for (char ch : value) {
      if (ch == '\r') continue;
      if (ch == '\n') {
        if (!overflow && used > 0) {
          pending[used] = '\0';
          bleMeshEnqueueCommand(pending);
        }
        used = 0;
        overflow = false;
      } else if (!overflow) {
        if (used < sizeof(pending) - 1) pending[used++] = ch;
        else overflow = true;
      }
    }
  }
};
}  // namespace

bool bleMeshInit(const char* deviceName) {
  commandQueue = xQueueCreate(8, sizeof(CommandLine));
  if (!commandQueue) return false;

  NimBLEDevice::init(deviceName);
  NimBLEDevice::setPower(ESP_PWR_LVL_P9);
  NimBLEServer* server = NimBLEDevice::createServer();
  server->setCallbacks(new ServerCallbacks());
  NimBLEService* service = server->createService(SERVICE_UUID);
  NimBLECharacteristic* rx = service->createCharacteristic(
      RX_UUID, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
  rx->setCallbacks(new RxCallbacks());
  txCharacteristic = service->createCharacteristic(TX_UUID, NIMBLE_PROPERTY::NOTIFY);
  service->start();

  NimBLEAdvertising* advertising = NimBLEDevice::getAdvertising();
  advertising->addServiceUUID(SERVICE_UUID);
  advertising->setScanResponse(true);
  advertising->start();
  Serial.printf("[BLE] Nordic UART service started as \"%s\"\n", deviceName);
  return true;
}

bool bleMeshConnected() { return clientConnected; }

bool bleMeshEnqueueCommand(const char* line) {
  if (!commandQueue || !line) return false;
  const size_t length = strlen(line);
  if (length == 0 || length >= sizeof(CommandLine::value)) return false;
  CommandLine command{};
  memcpy(command.value, line, length);
  return xQueueSend(commandQueue, &command, 0) == pdTRUE;
}

bool bleMeshTakeCommand(char* out, size_t capacity) {
  if (!commandQueue || !out || capacity < sizeof(CommandLine::value)) return false;
  CommandLine command{};
  if (xQueueReceive(commandQueue, &command, 0) != pdTRUE) return false;
  memcpy(out, command.value, sizeof(command.value));
  return true;
}

void bleMeshNotifyLine(const String& line) {
  if (!clientConnected || !txCharacteristic) return;
  constexpr size_t CHUNK = 20;
  for (size_t pos = 0; pos < line.length(); pos += CHUNK) {
    String part = line.substring(pos, pos + CHUNK);
    txCharacteristic->setValue(reinterpret_cast<const uint8_t*>(part.c_str()), part.length());
    txCharacteristic->notify();
  }
}

#else

// Stubs khi BLE không được bật (tiết kiệm bộ nhớ cho trạm lặp thuần LoRa)
bool bleMeshInit(const char*) { return false; }
bool bleMeshConnected() { return false; }
bool bleMeshEnqueueCommand(const char*) { return false; }
bool bleMeshTakeCommand(char*, size_t) { return false; }
void bleMeshNotifyLine(const String&) {}

#endif // CONFIG_BLE_ENABLED || ROLE_NODE

