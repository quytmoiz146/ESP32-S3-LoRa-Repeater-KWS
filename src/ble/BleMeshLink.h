#ifndef BLE_MESH_LINK_H
#define BLE_MESH_LINK_H

#include <Arduino.h>

// Nordic UART Service (NUS) dùng cho Mobile App kết nối với SOS Mesh
bool bleMeshInit(const char* deviceName = "SOS-Node");
bool bleMeshConnected();
bool bleMeshEnqueueCommand(const char* line);
bool bleMeshTakeCommand(char* out, size_t capacity);
void bleMeshNotifyLine(const String& line);

#endif // BLE_MESH_LINK_H

