# Hướng dẫn Deploy YOLO Streamer lên LicheeRV Nano

Tài liệu này hướng dẫn cách copy file thực thi (binary), cấp quyền chạy và thiết lập để ứng dụng tự động chạy ngầm mỗi khi khởi động board (Auto-start on boot) với các tham số đi kèm.

## 1. Copy file sang LicheeRV Nano

Sau khi build thành công chương trình trên máy tính (host), bạn cần chuyển file thực thi `Yolo_CSIStream` và file mô hình `.cvimodel` vào thư mục `/root` của board.

Sử dụng lệnh `scp` (thay IP `192.168.100.2` hoặc `192.168.42.1` bằng IP thực tế của mạng RNDIS/Ethernet bạn đang dùng):

```bash
# Copy file thực thi
scp build/Yolo_CSIStream root@192.168.100.2:/root/

# Copy file model YOLO
scp yolov8n_coco_640.cvimodel root@192.168.100.2:/root/
```

## 2. Cấp quyền thực thi và kiểm tra

Truy cập vào board thông qua SSH hoặc qua console (Serial/RNDIS):
```bash
ssh root@192.168.100.2
```

Đảm bảo file `Yolo_CSIStream` có quyền thực thi trên board:
```bash
chmod +x /root/Yolo_CSIStream
```

*(Tùy chọn)* Chạy thử trực tiếp để kiểm tra lỗi trước khi cấu hình tự động chạy:
```bash
cd /root/
./Yolo_CSIStream yolov8n_coco_640.cvimodel --cam 640x480 --uart /dev/ttyS0 --baud 115200
```
*Nhấn `Ctrl+C` để dừng chương trình.*

## 3. Cấu hình tự khởi động (Auto-run) với init script

Hệ điều hành hiện tại trên LicheeRV Nano thông thường sử dụng Busybox/SysVinit thay vì systemd. Chúng ta sẽ tạo một service script trong `/etc/init.d/` và đặt tên là `S99yolocam` (với "S" biểu thị script tự chạy lúc start up, "99" là thứ tự ưu tiên khởi động sau các dịch vụ khác như mạng, module camera).

Sử dụng trình soạn thảo `vi` trên LicheeRV Nano:
```bash
vi /etc/init.d/S99yolocam
```
*(Nhấn phím `i` để vào chế độ insert, dán thư mục lệnh bên dưới, sau đó nhấn phím `Esc` rồi gõ `:wq` để lưu và thoát)*

**Nội dung của file `/etc/init.d/S99yolocam`**:
```sh
#!/bin/sh
#
# Start YOLO Camera Streamer at boot
#

# Bạn thiết lập tham số mong muốn ở mảng ARGS dưới đây
APP_BIN="/root/Yolo_CSIStream"
MODEL="/root/yolov8n_coco_640.cvimodel"
ARGS="--cam 640x480 --uart /dev/ttyS0 --baud 115200"
LOG_FILE="/root/yolo.log"

case "$1" in
  start)
    printf "Starting Yolo_CSIStream: "
    if [ -f "$APP_BIN" ] && [ -f "$MODEL" ]; then
        cd /root/
        # Chạy app dưới background bằng cú pháp "&", đẩy log ra file
        $APP_BIN $MODEL $ARGS > $LOG_FILE 2>&1 &
        echo "OK"
    else
        echo "FAIL (Binary or Model not found)"
    fi
    ;;
  stop)
    printf "Stopping Yolo_CSIStream: "
    killall Yolo_CSIStream 2>/dev/null
    echo "OK"
    ;;
  restart|reload)
    "$0" stop
    sleep 1
    "$0" start
    ;;
  *)
    echo "Usage: $0 {start|stop|restart}"
    exit 1
esac

exit 0
```

## 4. Cấp quyền cho Script và kiểm tra hoạt động

Phân quyền Execute để hệ thống có thể thực thi file dịch vụ trên:
```bash
chmod +x /etc/init.d/S99yolocam
```

Bây giờ bạn có thể thử điều khiển ứng dụng nhanh bằng service vừa tạo:
```bash
# Bật ứng dụng chạy nền:
/etc/init.d/S99yolocam start

# Kiểm tra xem app có sống không và không bị thoát:
ps | grep Yolo_CSIStream

# Đọc log hệ thống tại thời điểm đó:
cat /root/yolo.log

# Tắt ứng dụng
/etc/init.d/S99yolocam stop
```

**Hoàn tất!** Hãy khởi động lại (`reboot`) board LicheeRV Nano, ứng dụng hệ thống cùng camera AI của bạn sẽ hoàn toàn chạy ẩn dưới máy một cách ổn định.
