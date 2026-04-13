#include <Arduino.h>

void setup() {
  Serial.begin(115200);   // Giao tiếp với máy tính
  Serial1.begin(115200, SERIAL_8N1, 16, 17);
  // Nối RX của Arduino/ESP32 với TX của LicheeRV
}

// Hàm tính NMEA Checksum
uint8_t calculateChecksum(const char* str) {
  uint8_t cs = 0;
  for (int i = 0; str[i] && str[i] != '*'; i++) {
    cs ^= str[i];
  }
  return cs;
}

void loop() {
  if (Serial1.available()) {
    String line = Serial1.readStringUntil('\n');
    line.trim();
    Serial.println(line);

    if (line.startsWith("$YOLO,")) {
      // 1. Kiểm tra Checksum (tùy chọn nhưng khuyến nghị)
      int starIdx = line.lastIndexOf('*');
      if (starIdx > 0) {
        String dataPart = line.substring(6, starIdx); // Bỏ "$YOLO," và "*XX"
        String csStr = line.substring(starIdx + 1);
        uint8_t rxCs = (uint8_t) strtol(csStr.c_str(), NULL, 16);
        uint8_t calcCs = calculateChecksum(line.c_str() + 1);
        
        if (rxCs == calcCs) {
          // 2. Parse dữ liệu hợp lệ
          char buf[256];
          dataPart.toCharArray(buf, sizeof(buf));
          
          char* ptr = strtok(buf, ",");
          unsigned long ts = atol(ptr); // Timestamp
          
          ptr = strtok(NULL, ",");
          int count = atoi(ptr); // Số object
          
          ptr = strtok(NULL, ",");
          int imgW = atoi(ptr); // Chiều rộng ảnh YOLO
          
          ptr = strtok(NULL, ",");
          int imgH = atoi(ptr); // Chiều cao ảnh YOLO
          
          Serial.printf("Time: %lu ms | Size: %dx%d | Objects: %d\n", ts, imgW, imgH, count);
          
          for (int i = 0; i < count; i++) {
            int cls = atoi(strtok(NULL, ","));
            int x1  = atoi(strtok(NULL, ","));
            int y1  = atoi(strtok(NULL, ","));
            int x2  = atoi(strtok(NULL, ","));
            int y2  = atoi(strtok(NULL, ","));
            int score = atoi(strtok(NULL, ","));
            
            Serial.printf("  [%d] Class %d: Box(%d,%d->%d,%d) %d%%\n", 
                          i, cls, x1, y1, x2, y2, score);
          }
        }
      }
    }
  }
}



