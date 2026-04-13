import processing.serial.*;

Serial myPort;
String val;
int imgW = 640;
int imgH = 640;

// Store objects for drawing
class YoloObject {
  int cls;
  float x1, y1, x2, y2;
  int score;
  
  YoloObject(int c, float _x1, float _y1, float _x2, float _y2, int s) {
    cls = c; x1=_x1; y1=_y1; x2=_x2; y2=_y2; score=s;
  }
}
ArrayList<YoloObject> objects = new ArrayList<YoloObject>();

// COCO Class Colors (subset)
color[] classColors = {
  color(255, 0, 0),    // 0: person
  color(0, 255, 0),    // 1: bicycle
  color(0, 0, 255),    // 2: car
  color(255, 255, 0),  // 3: motorcycle
  color(255, 0, 255),  // 4: airplane
  color(0, 255, 255),  // 5: bus
  color(255, 128, 0)   // etc...
};

void setup() {
  size(800, 800);  // Window size
  
  // *** BE SURE TO CHANGE THIS TO YOUR ESP32 COM PORT ***
  // Example: "COM3" on Windows, "/dev/ttyUSB0" on Linux/Mac
  String portName = "COM4"; 
  
  try {
    myPort = new Serial(this, portName, 115200);
    myPort.bufferUntil('\n'); // Trigger serialEvent on newline
    println("Connected to " + portName);
  } catch (Exception e) {
    println("Error opening port " + portName + ". Check your connections/port name!");
    println("Available ports:");
    printArray(Serial.list());
  }
  
  textSize(16);
  textAlign(LEFT, TOP);
}

void draw() {
  background(40); // Dark grey background
  
  // Draw camera frame boundary
  float scaleFactor = min((float)width/imgW, (float)height/imgH);
  float drawW = imgW * scaleFactor;
  float drawH = imgH * scaleFactor;
  float offsetX = (width - drawW) / 2;
  float offsetY = (height - drawH) / 2;
  
  // Draw the "Screen"
  fill(20);
  stroke(100);
  rect(offsetX, offsetY, drawW, drawH);
  
  // Draw Objects
  for (YoloObject obj : objects) {
    // Map YOLO coordinates to Processing window coordinates
    float px1 = offsetX + (obj.x1 / imgW) * drawW;
    float py1 = offsetY + (obj.y1 / imgH) * drawH;
    float px2 = offsetX + (obj.x2 / imgW) * drawW;
    float py2 = offsetY + (obj.y2 / imgH) * drawH;
    float boxW = px2 - px1;
    float boxH = py2 - py1;
    
    // Pick color
    color c = classColors[obj.cls % classColors.length];
    
    // Draw Box
    noFill();
    stroke(c);
    strokeWeight(3);
    rect(px1, py1, boxW, boxH);
    
    // Draw Label Background
    fill(c);
    noStroke();
    rect(px1, py1 - 22, max(120, boxW), 22);
    
    // Draw Text
    fill(255);
    text("Class " + obj.cls + " (" + obj.score + "%)", px1 + 4, py1 - 20);
  }
  
  // Draw Info Overlay
  fill(255);
  text("Objects: " + objects.size(), 10, 10);
  text("Resolution: " + imgW + "x" + imgH, 10, 30);
}

// Automatically called when UART data arrives ending with \n
void serialEvent(Serial myPort) {
  val = myPort.readStringUntil('\n');
  if (val != null) {
    val = trim(val);
    // Parse $YOLO format
    if (val.startsWith("$YOLO,")) {
      int starIdx = val.indexOf('*');
      if (starIdx > 0) {
        String dataPart = val.substring(6, starIdx);
        String[] parts = split(dataPart, ',');
        
        if (parts.length >= 4) {
          // parts[0] = timestamp
          int count = int(parts[1]);
          imgW = int(parts[2]);
          imgH = int(parts[3]);
          
          // Temporary list to avoid flickering
          ArrayList<YoloObject> newObjs = new ArrayList<YoloObject>();
          
          int idx = 4;
          for (int i = 0; i < count; i++) {
            if (idx + 5 < parts.length) {
              int cls = int(parts[idx++]);
              float x1 = float(parts[idx++]);
              float y1 = float(parts[idx++]);
              float x2 = float(parts[idx++]);
              float y2 = float(parts[idx++]);
              int score = int(parts[idx++]);
              newObjs.add(new YoloObject(cls, x1, y1, x2, y2, score));
            }
          }
          
          // Safely update main list
          objects = newObjs;
        }
      }
    }
  }
}
