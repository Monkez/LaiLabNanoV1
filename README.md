<p align="center">
  <img src="https://img.shields.io/badge/Python-3.8+-3776ab?style=for-the-badge&logo=python&logoColor=white" alt="Python"/>
  <img src="https://img.shields.io/badge/Flask-3.0-000000?style=for-the-badge&logo=flask&logoColor=white" alt="Flask"/>
  <img src="https://img.shields.io/badge/Docker-Required-2496ed?style=for-the-badge&logo=docker&logoColor=white" alt="Docker"/>
  <img src="https://img.shields.io/badge/License-MIT-green?style=for-the-badge" alt="License"/>
</p>

# 🚀 LaiLab Nano V1

**AI Edge Inference Model Preparation & Deployment Platform**

> Chuyển đổi và triển khai mô hình YOLO lên LicheeRV Nano với pipeline tự động hoàn toàn — không cần viết code.

---

## ✨ Tổng quan

**LaiLab Nano V1** là một nền tảng web giúp tự động hóa toàn bộ quy trình chuyển đổi mô hình YOLO (`.pt`) sang định dạng `.cvimodel` tối ưu cho chip **CV181x (LicheeRV Nano)**. Hệ thống tích hợp Docker + TPU-MLIR, cung cấp giao diện trực quan với theo dõi tiến trình real-time qua WebSocket.

### Tính năng chính

| Tính năng | Mô tả |
|-----------|-------|
| 🔄 **Automated Pipeline** | Tự động hóa 6 bước: Setup → ONNX Export → MLIR Transform → Calibration → Quantize → Deploy |
| 🐳 **Docker Integration** | Tự động phát hiện và quản lý Docker container chạy TPU-MLIR |
| 📡 **Real-time Monitoring** | WebSocket live streaming log và tiến trình từng bước |
| 🌐 **Bilingual UI** | Hỗ trợ song ngữ Tiếng Việt / English |
| 🎨 **Modern UI** | Landing page + App dashboard với dark/light theme |
| 📦 **Preset Models** | Hỗ trợ sẵn YOLOv8n, YOLO11n (640×640, 320×320) |
| 📤 **Custom Upload** | Upload mô hình `.pt` tùy chỉnh |
| 📲 **Auto Transfer** | Tự động SCP model sang thiết bị qua SSH |

---

## 📂 Cấu trúc dự án

```
LicheeRVNano/
├── app.py                  # Flask backend server (API + WebSocket)
├── requirements.txt        # Python dependencies
├── convert_command.txt     # Tham khảo lệnh chuyển đổi thủ công
├── static/
│   ├── index.html          # Landing page
│   ├── landing.css         # Landing page styles
│   ├── landing.js          # Landing page scripts
│   ├── app.html            # App dashboard (Model Preparation)
│   ├── app.css             # App dashboard styles
│   ├── app.js              # App dashboard logic + WebSocket client
│   └── i18n.js             # Internationalization (EN/VI)
├── uploads/                # Uploaded .pt model files
├── outputs/                # Generated .cvimodel files
└── workspaces/             # Job workspace data
```

---

## ⚙️ Yêu cầu hệ thống

| Thành phần | Yêu cầu |
|------------|----------|
| **Python** | 3.8+ |
| **Docker** | Docker Desktop đang chạy |
| **Docker Container** | Container với TPU-MLIR (Sophgo), ví dụ: `MyName` |
| **Hệ điều hành** | Windows / Linux / macOS |
| **RAM** | ≥ 8GB (khuyến nghị 16GB cho calibration) |

---

## 🚀 Cài đặt & Khởi chạy

### Cài đặt môi trường phát triển đầy đủ trên Windows

Yêu cầu cài sẵn:

- Python 3.10 trở lên.
- Docker Desktop đang chạy với WSL 2 backend.
- Git.

Từ thư mục gốc của dự án, chạy:

```bat
setup.bat
```

Script này sẽ tự động:

1. Tạo hoặc sửa lại Python virtual environment `.venv`.
2. Cài toàn bộ thư viện backend từ `requirements.txt`.
3. Build Docker image chứa RISC-V cross-compiler và CVITEK TDL SDK.
4. Tải OpenCV Mobile dành cho LicheeRV Nano.
5. Kiểm tra các dependency cần cho web và inference.

Sau khi setup:

```bat
start.bat
```

Để build lại chương trình chạy trên board:

```bat
build_inference.bat
```

Binary được tạo tại:

```text
develop\Projects\OTGCamera\build\Yolo_CSIStream
develop\Projects\OTGCamera\build\reset_btn
```

Frontend dùng HTML/CSS/JavaScript thuần trong `static/`, vì vậy không cần Node.js hoặc npm.

### 1. Clone dự án

```bash
git clone https://github.com/ret7020/LicheeRVNano.git
cd LicheeRVNano
```

### 2. Tạo virtual environment

```bash
python -m venv .venv

# Windows
.venv\Scripts\activate

# Linux/macOS
source .venv/bin/activate
```

### 3. Cài đặt dependencies

```bash
pip install -r requirements.txt
```

### 4. Khởi chạy server

```bash
python app.py
```

Server sẽ chạy tại: **http://localhost:5000**

---

## 🐳 Thiết lập Docker (TPU-MLIR)

Trước khi chạy pipeline chuyển đổi, bạn cần có Docker container với TPU-MLIR:

```bash
# Pull Docker image (Sophgo TPU-MLIR)
docker pull sophgo/tpuc_dev:latest

# Tạo container
docker run -td --name MyName sophgo/tpuc_dev:latest

# Hoặc start container đã có
docker start MyName
```

> **Lưu ý:** Hệ thống sẽ tự động clone và build TPU-MLIR (v1.7) trong container nếu chưa có, nhưng quá trình này có thể mất ~30 phút.

---

## 🔧 Pipeline chuyển đổi

Pipeline tự động thực hiện 6 bước:

```
┌─────────────┐    ┌──────────────┐    ┌────────────────┐
│  1. Setup   │───▶│ 2. Export    │───▶│ 3. Transform   │
│  Docker &   │    │  .pt → ONNX  │    │  ONNX → MLIR   │
│  TPU-MLIR   │    │              │    │                │
└─────────────┘    └──────────────┘    └────────────────┘
                                              │
┌─────────────┐    ┌──────────────┐    ┌──────▼─────────┐
│  6. Transfer│◀───│ 5. Deploy    │◀───│ 4. Calibration │
│  SCP to     │    │  Quantize →  │    │  COCO2017      │
│  Device     │    │  .cvimodel   │    │  Dataset       │
└─────────────┘    └──────────────┘    └────────────────┘
```

### Chi tiết các bước

1. **Environment Setup** — Kiểm tra Docker, start container, cài đặt TPU-MLIR
2. **Export to ONNX** — Chuyển đổi `.pt` sang `.onnx` bằng `export_to_onnx.py`
3. **Model Transform** — `model_transform` tạo file `.mlir` từ ONNX
4. **Calibration** — `run_calibration` với COCO2017 dataset (mặc định 100 ảnh)
5. **Model Deploy** — `model_deploy` quantize INT8/BF16 → `.cvimodel`
6. **Transfer** — SCP truyền model sang thiết bị LicheeRV Nano

---

## 🌐 API Endpoints

| Method | Endpoint | Mô tả |
|--------|----------|-------|
| `GET` | `/` | Landing page |
| `GET` | `/app` | App dashboard |
| `GET` | `/api/health` | Health check |
| `GET` | `/api/docker/status` | Trạng thái Docker & danh sách container |
| `POST` | `/api/upload` | Upload file `.pt` |
| `GET` | `/api/jobs` | Danh sách jobs |
| `POST` | `/api/jobs` | Tạo conversion job mới |
| `GET` | `/api/jobs/<id>` | Chi tiết job |
| `POST` | `/api/jobs/<id>/cancel` | Hủy job |
| `GET` | `/api/jobs/<id>/download` | Download `.cvimodel` |
| `GET` | `/api/config/presets` | Danh sách preset models |

### WebSocket Events

| Event | Hướng | Mô tả |
|-------|-------|-------|
| `connect` | Client → Server | Kết nối WebSocket |
| `connected` | Server → Client | Xác nhận kết nối |
| `job_update` | Server → Client | Cập nhật trạng thái job |
| `job_log` | Server → Client | Log message real-time |

---

## 📋 Cấu hình Preset Models

| Preset | Input Size | Quantize | Processor |
|--------|-----------|----------|-----------|
| YOLOv8 Nano | 640×640 | INT8 | CV181x |
| YOLOv8 Nano | 320×320 | INT8 | CV181x |
| YOLO11 Nano | 640×640 | INT8 | CV181x |
| YOLO11 Nano | 320×320 | INT8 | CV181x |

---

## 🖥️ Giao diện

### Landing Page (`/`)
- Hero section với animation
- Features, Specifications, Workflow, Compatibility sections
- Dark/Light theme toggle
- Đa ngôn ngữ EN/VI

### App Dashboard (`/app`)
- **Sidebar**: Dashboard, Model Preparation, Deployment, Configuration, Monitoring
- **Model Preparation**: Form cấu hình + Pipeline tracker real-time
- **Docker Status**: Hiển thị trạng thái Docker trên top bar
- **Container Selector**: Tự động detect Docker containers đang chạy
- **Live Logs**: Console hiển thị log pipeline real-time

---

## 🛠️ Công nghệ sử dụng

| Layer | Công nghệ |
|-------|-----------|
| **Backend** | Python, Flask, Flask-SocketIO, Flask-CORS |
| **Frontend** | HTML5, CSS3 (Vanilla), JavaScript (ES6+) |
| **Real-time** | WebSocket (Socket.IO) |
| **Model Tools** | Sophgo TPU-MLIR, ONNX |
| **Container** | Docker |
| **Fonts** | Inter, JetBrains Mono (Google Fonts) |

---

## 📝 Lệnh chuyển đổi thủ công (tham khảo)

Nếu muốn chạy thủ công trong Docker:

```bash
docker start MyName
docker exec -it MyName /bin/bash

# Trong container
cd /workspace/tpu-mlir
source ./envsetup.sh

mkdir model_yolov8n && cd model_yolov8n
cp -rf ${REGRESSION_PATH}/dataset/COCO2017 .
cp -rf ${REGRESSION_PATH}/image .
mkdir Workspace && cd Workspace

# Download model & export
curl -L -O https://github.com/ultralytics/assets/releases/download/v0.0.0/yolov8n.pt
python3 export_to_onnx.py yolov8n.pt 640 640

# Transform → MLIR
model_transform \
  --model_name yolov8n \
  --model_def yolov8n.onnx \
  --input_shapes "[[1,3,640,640]]" \
  --mean "0.0,0.0,0.0" \
  --scale "0.0039216,0.0039216,0.0039216" \
  --keep_aspect_ratio \
  --pixel_format rgb \
  --test_input ../image/dog.jpg \
  --test_result yolov8n_top_outputs.npz \
  --mlir yolov8n.mlir

# Calibration
run_calibration yolov8n.mlir \
  --dataset ../COCO2017 \
  --input_num 100 \
  -o yolov8n_calib_table

# Deploy → .cvimodel
model_deploy \
  --mlir yolov8n.mlir \
  --quant_input --quant_output \
  --quantize int8 \
  --calibration_table yolov8n_calib_table \
  --processor cv181x \
  --test_input ../image/dog.jpg \
  --test_reference yolov8n_top_outputs.npz \
  --tolerance 0.85,0.45 \
  --model yolov8n_640.cvimodel

# Transfer to device
scp yolov8n_640.cvimodel root@192.168.100.2:/root/
```

---

## 📄 License

MIT License — Xem file [LICENSE](LICENSE) để biết thêm chi tiết.

---

<p align="center">
  <b>LaiLab Nano V1</b> — AI Edge Inference Platform for LicheeRV Nano<br/>
  Made with ❤️ by <a href="https://github.com/ret7020">ret7020</a>
</p>
