/* ============================================================
   LaiLab Nano V1 — Internationalization (i18n)
   Supports English & Vietnamese
   ============================================================ */

const translations = {
    en: {
        // Navbar
        nav_features: 'Features',
        nav_specs: 'Specifications',
        nav_workflow: 'Workflow',
        nav_compatible: 'Compatible',
        nav_launch: 'Launch App',

        // Hero
        hero_badge: '<span class="badge-dot"></span>Breakthrough in Edge AI Inference',
        hero_title_1: 'AI Inference',
        hero_title_2: 'On The Edge',
        hero_title_3: 'Redefined',
        hero_desc: 'Deploy YOLO object detection models on LaiLab Nano with lightning-fast 20 FPS real-time inference. No coding required. Compatible with any USB camera and embedded platforms.',
        hero_cta_start: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" width="20" height="20"><polygon points="5 3 19 12 5 21 5 3"/></svg>Get Started',
        hero_cta_learn: 'Learn More<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" width="18" height="18"><path d="M7 17l9.2-9.2M17 17V7.8H7.8"/></svg>',
        stat_realtime: 'Real-time Detection',
        stat_latency: 'Ultra-low Latency',
        stat_code_unit: 'lines',
        stat_code: 'Code Required',

        // Features
        features_tag: 'Features',
        features_title: 'Everything You Need for Edge AI',
        features_desc: 'From model conversion to deployment, LaiLab Nano V1 provides a complete, automated pipeline for running AI inference on edge devices.',
        feature_1_title: 'Automated Model Conversion',
        feature_1_desc: 'One-click pipeline from YOLO .pt model to optimized .cvimodel. Handles ONNX export, MLIR transformation, INT8 quantization, and calibration automatically.',
        feature_2_title: 'Zero-Code Deployment',
        feature_2_desc: 'Deploy models to LaiLab Nano without writing a single line of code. Intuitive visual interface handles everything.',
        feature_3_title: 'Lightning Performance',
        feature_3_desc: 'Achieve 20 FPS real-time object detection with ultra-low latency. Optimized INT8 quantization delivers maximum throughput.',
        feature_4_title: 'USB Camera Ready',
        feature_4_desc: 'Plug and play with any standard USB camera. Start detecting objects in seconds with zero configuration.',
        feature_5_title: 'Real-time Monitoring',
        feature_5_desc: 'Live WebSocket-powered progress tracking. Watch every step of the conversion pipeline in real time.',
        feature_6_title: 'Universal Compatibility',
        feature_6_desc: 'Seamlessly integrates with Arduino, Raspberry Pi, PC, and other embedded platforms. UART/SPI/I2C communication protocols supported for versatile deployment.',

        // Specs
        specs_tag: 'Specifications',
        specs_title: 'Powerful Specs, Tiny Footprint',
        spec_fps: 'Real-time YOLO Detection',
        spec_quant_unit: 'Quantization',
        spec_quant: 'Optimized Precision',
        spec_processor: 'Custom AI Processor',
        spec_accel_unit: 'Accelerator',
        spec_accel: 'Dedicated AI Core',
        spec_camera: 'Plug & Play Input',
        spec_power_unit: 'Power',
        spec_power: 'Ultra-low Consumption',

        // Workflow
        workflow_tag: 'Workflow',
        workflow_title: 'From Model to Edge in Minutes',
        workflow_desc: 'Our automated pipeline handles the entire conversion process. Just upload your model and configure a few parameters.',
        step_1_title: 'Upload Model',
        step_1_desc: 'Upload your YOLO .pt model or select from preset models (YOLOv8n, YOLO11n)',
        step_2_title: 'Configure Parameters',
        step_2_desc: 'Set input resolution, quantization type, calibration count, and target processor',
        step_3_title: 'Automated Conversion',
        step_3_desc: 'ONNX export → MLIR transform → INT8 calibration → CVI model deploy — all automated',
        step_4_title: 'Deploy to Device',
        step_4_desc: 'Automatically transfer the optimized model to your LaiLab Nano device via SSH. Ready to run!',

        // Compatible
        compat_tag: 'Compatibility',
        compat_title: 'Works With Your Platform',
        compat_desc: 'LaiLab Nano V1 integrates seamlessly with popular embedded platforms and development environments.',
        compat_arduino: 'UART serial communication for detection results',
        compat_rpi: 'SPI/I2C/UART interface for Pi integration',
        compat_pc: 'USB/Ethernet connection for desktop applications',
        compat_iot_title: 'IoT Devices',
        compat_iot: 'Deploy on any embedded system with standard protocols',

        // CTA
        cta_title: 'Ready to Deploy AI on the Edge?',
        cta_desc: 'Start converting and deploying your YOLO models in minutes. No coding experience required.',
        cta_button: 'Launch LaiLab Nano V1<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" width="20" height="20"><line x1="5" y1="12" x2="19" y2="12"/><polyline points="12 5 19 12 12 19"/></svg>',

        // Footer
        footer_desc: 'AI Edge Inference Platform',
        footer_product: 'Product',
        footer_resources: 'Resources',
        footer_docs: 'Documentation',

        // ======== APP PAGE ========
        app_title: 'Model Preparation',
        app_subtitle: 'Convert YOLO models for edge device deployment',
        app_nav_guide: 'User Guide',
        app_nav_model_prep: 'Model Preparation',
        app_nav_deploy: 'Deployment',
        app_nav_config: 'Configuration',
        app_nav_monitor: 'Monitoring',
        inference_serial_title: 'Serial Monitor',
        inference_title: 'Test Inference',
        inference_connect: 'Connect Data',
        inference_no_stream: 'No Active Stream',
        
        // Guide
        guide_title: 'Quick Start Guide',
        guide_step_1: '1. Model Preparation',
        guide_step_1_desc: 'Navigate to "Model Preparation" to upload a raw YOLO `.pt` model or pick a preset. Click "Start Conversion" to automate the ONNX → MLIR → CVI process. Ensure Docker is running in background.',
        guide_step_2: '2. Deployment',
        guide_step_2_desc: 'Once the model becomes a `.cvimodel`, go to "Deployment". Enter your device IP address and click "Deploy". The system will automatically configure, transfer the model, and reboot the CSI stream on the edge device.',
        guide_step_3: '3. Test Inference',
        guide_step_3_desc: 'Go to "Test Inference" to test your deployed model in real-time. Use the Video stream to verify visual detection, and open the Serial Monitor to capture bounding box metrics via UART if configured.',

        // Model Prep Form
        form_title: 'New Conversion Job',
        form_model_source: 'Model Source',
        form_settings: 'Settings',
        form_preset: 'Preset',
        form_custom: 'Upload',
        form_select_preset: 'Select a preset model...',
        form_model_name: 'Name',
        form_input_width: 'Resolution',
        form_input_height: 'Height',
        form_docker_container: 'Docker',
        form_calibration_count: 'Calibration',
        form_quantize: 'Quantize',
        form_processor: 'Processor',
        form_tolerance: 'Tolerance',
        form_device_ip: 'Device IP',
        form_auto_transfer: 'Auto Transfer to Device',
        form_start: 'Start Conversion',
        form_advanced: 'Advanced',
        form_docker_hint: 'Container will be auto-created if not found.',
        docker_setup_btn: 'Download Docker Container',
        docker_setup_hint: 'Pulls sophgo/tpuc_dev:latest and creates the selected container.',

        // Pipeline
        pipeline_title: 'Pipeline',
        pipeline_empty: 'No active jobs. Create a new job to start.',
        pipeline_log_title: 'Logs',
        pipeline_log_empty: 'Waiting for pipeline to start...',

        // Download
        download_ready: 'Ready to download',
        download_btn: 'Download .cvimodel',

        // Status
        status_pending: 'Pending',
        status_running: 'Running',
        status_completed: 'Completed',
        status_failed: 'Failed',
        status_skipped: 'Skipped',
    },

    vi: {
        // Navbar
        nav_features: 'Tính năng',
        nav_specs: 'Thông số',
        nav_workflow: 'Quy trình',
        nav_compatible: 'Tương thích',
        nav_launch: 'Khởi chạy',

        // Hero
        hero_badge: '<span class="badge-dot"></span>Đột phá trong Suy luận AI biên',
        hero_title_1: 'Suy luận AI',
        hero_title_2: 'Trên Thiết Bị Biên',
        hero_title_3: 'Được Tái Định Nghĩa',
        hero_desc: 'Triển khai mô hình phát hiện đối tượng YOLO trên LaiLab Nano với tốc độ suy luận thời gian thực 20 FPS siêu nhanh. Không cần lập trình. Tương thích với mọi camera USB và nền tảng nhúng.',
        hero_cta_start: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" width="20" height="20"><polygon points="5 3 19 12 5 21 5 3"/></svg>Bắt đầu ngay',
        hero_cta_learn: 'Tìm hiểu thêm<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" width="18" height="18"><path d="M7 17l9.2-9.2M17 17V7.8H7.8"/></svg>',
        stat_realtime: 'Phát hiện thời gian thực',
        stat_latency: 'Độ trễ cực thấp',
        stat_code_unit: 'dòng',
        stat_code: 'Code cần viết',

        // Features
        features_tag: 'Tính năng',
        features_title: 'Mọi thứ bạn cần cho AI biên',
        features_desc: 'Từ chuyển đổi mô hình đến triển khai, LaiLab Nano V1 cung cấp pipeline tự động hoàn chỉnh để chạy suy luận AI trên thiết bị biên.',
        feature_1_title: 'Chuyển đổi Model tự động',
        feature_1_desc: 'Pipeline một cú nhấp từ YOLO .pt sang .cvimodel tối ưu. Tự động xử lý xuất ONNX, chuyển đổi MLIR, lượng tử hóa INT8 và hiệu chuẩn.',
        feature_2_title: 'Triển khai không cần Code',
        feature_2_desc: 'Triển khai mô hình lên LaiLab Nano mà không cần viết một dòng code nào. Giao diện trực quan xử lý mọi thứ.',
        feature_3_title: 'Hiệu suất siêu nhanh',
        feature_3_desc: 'Đạt 20 FPS phát hiện đối tượng thời gian thực với độ trễ cực thấp. Lượng tử hóa INT8 tối ưu mang lại thông lượng tối đa.',
        feature_4_title: 'Sẵn sàng Camera USB',
        feature_4_desc: 'Cắm và chạy với bất kỳ camera USB tiêu chuẩn nào. Bắt đầu phát hiện đối tượng trong vài giây với cấu hình bằng không.',
        feature_5_title: 'Giám sát thời gian thực',
        feature_5_desc: 'Theo dõi tiến trình qua WebSocket trực tiếp. Xem từng bước của pipeline chuyển đổi trong thời gian thực.',
        feature_6_title: 'Tương thích đa nền tảng',
        feature_6_desc: 'Tích hợp liền mạch với Arduino, Raspberry Pi, PC và các nền tảng nhúng khác. Hỗ trợ giao thức UART/SPI/I2C cho triển khai đa dạng.',

        // Specs
        specs_tag: 'Thông số',
        specs_title: 'Thông số mạnh mẽ, Kích thước nhỏ gọn',
        spec_fps: 'Phát hiện YOLO thời gian thực',
        spec_quant_unit: 'Lượng tử hóa',
        spec_quant: 'Độ chính xác tối ưu',
        spec_processor: 'Bộ xử lý AI tùy chỉnh',
        spec_accel_unit: 'Bộ tăng tốc',
        spec_accel: 'Lõi AI chuyên dụng',
        spec_camera: 'Đầu vào Cắm & Chạy',
        spec_power_unit: 'Công suất',
        spec_power: 'Tiêu thụ cực thấp',

        // Workflow
        workflow_tag: 'Quy trình',
        workflow_title: 'Từ Model đến Thiết bị biên trong vài phút',
        workflow_desc: 'Pipeline tự động xử lý toàn bộ quy trình chuyển đổi. Chỉ cần tải lên model và cấu hình vài tham số.',
        step_1_title: 'Tải lên Model',
        step_1_desc: 'Tải lên model YOLO .pt hoặc chọn từ các preset có sẵn (YOLOv8n, YOLO11n)',
        step_2_title: 'Cấu hình tham số',
        step_2_desc: 'Đặt độ phân giải đầu vào, kiểu lượng tử hóa, số ảnh hiệu chuẩn và bộ xử lý đích',
        step_3_title: 'Chuyển đổi tự động',
        step_3_desc: 'Xuất ONNX → Chuyển đổi MLIR → Hiệu chuẩn INT8 → Triển khai CVI model — tất cả tự động',
        step_4_title: 'Triển khai lên thiết bị',
        step_4_desc: 'Tự động truyền model đã tối ưu đến thiết bị LaiLab Nano qua SSH. Sẵn sàng chạy!',

        // Compatible
        compat_tag: 'Tương thích',
        compat_title: 'Hoạt động với nền tảng của bạn',
        compat_desc: 'LaiLab Nano V1 tích hợp liền mạch với các nền tảng nhúng phổ biến và môi trường phát triển.',
        compat_arduino: 'Giao tiếp UART serial cho kết quả phát hiện',
        compat_rpi: 'Giao diện SPI/I2C/UART cho tích hợp Pi',
        compat_pc: 'Kết nối USB/Ethernet cho ứng dụng desktop',
        compat_iot_title: 'Thiết bị IoT',
        compat_iot: 'Triển khai trên bất kỳ hệ thống nhúng nào với giao thức tiêu chuẩn',

        // CTA
        cta_title: 'Sẵn sàng triển khai AI trên thiết bị biên?',
        cta_desc: 'Bắt đầu chuyển đổi và triển khai model YOLO trong vài phút. Không cần kinh nghiệm lập trình.',
        cta_button: 'Khởi chạy LaiLab Nano V1<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" width="20" height="20"><line x1="5" y1="12" x2="19" y2="12"/><polyline points="12 5 19 12 12 19"/></svg>',

        // Footer
        footer_desc: 'Nền tảng Suy luận AI biên',
        footer_product: 'Sản phẩm',
        footer_resources: 'Tài nguyên',
        footer_docs: 'Tài liệu',

        // ======== APP PAGE ========
        app_title: 'Chuẩn bị Model',
        app_subtitle: 'Chuyển đổi model YOLO để triển khai trên thiết bị biên',
        app_nav_guide: 'Hướng dẫn sử dụng',
        app_nav_model_prep: 'Chuẩn bị Model',
        app_nav_deploy: 'Triển khai',
        app_nav_config: 'Cấu hình',
        app_nav_monitor: 'Giám sát',
        inference_serial_title: 'Màn hình Serial',
        inference_title: 'Kiểm thử Suy luận',
        inference_connect: 'Kết nối Truyền dữ liệu',
        inference_no_stream: 'Không tìm thấy Stream',
        
        // Guide
        guide_title: 'Hướng dẫn Sử dụng Nhanh',
        guide_step_1: '1. Chuẩn bị Mô hình (Model Prep)',
        guide_step_1_desc: 'Sang mục "Chuyển đổi Model" để upload model YOLO `.pt` hoặc chọn preset. Nhấn "Start" để tự động hóa quy trình ONNX → MLIR → CVI. Đảm bảo cấu hình Docker chạy ngầm.',
        guide_step_2: '2. Triển khai (Deployment)',
        guide_step_2_desc: 'Khi model đã chuyển thành `.cvimodel`, sang mục "Triển khai". Nhập IP thiết bị và nhấn "Deploy". Hệ thống sẽ cấu hình và đổ model trực tiếp vào thiết bị.',
        guide_step_3: '3. Kiểm thử (Test Inference)',
        guide_step_3_desc: 'Mở trang "Kiểm thử Suy luận" để test luồng chạy thực tế. Dùng Video stream xem bbox trực quan, hoặc mở Serial Monitor kiểm tra tín hiệu gốc từ cổng COM.',

        // Model Prep Form
        form_title: 'Job chuyển đổi mới',
        form_model_source: 'Nguồn Model',
        form_settings: 'Cài đặt',
        form_preset: 'Preset',
        form_custom: 'Tải lên',
        form_select_preset: 'Chọn model preset...',
        form_model_name: 'Tên',
        form_input_width: 'Độ phân giải',
        form_input_height: 'Cao',
        form_docker_container: 'Docker',
        form_calibration_count: 'Calibration',
        form_quantize: 'Lượng tử',
        form_processor: 'Bộ xử lý',
        form_tolerance: 'Dung sai',
        form_device_ip: 'IP thiết bị',
        form_auto_transfer: 'Tự động truyền tới thiết bị',
        form_start: 'Bắt đầu chuyển đổi',
        form_advanced: 'Nâng cao',
        form_docker_hint: 'Container tự động tạo nếu chưa có.',
        docker_setup_btn: 'Tải Docker Container',
        docker_setup_hint: 'Tải sophgo/tpuc_dev:latest và tạo container đang chọn.',

        // Pipeline
        pipeline_title: 'Pipeline',
        pipeline_empty: 'Không có job. Tạo job mới để bắt đầu.',
        pipeline_log_title: 'Log',
        pipeline_log_empty: 'Đang chờ pipeline khởi chạy...',

        // Download
        download_ready: 'Sẵn sàng tải xuống',
        download_btn: 'Tải xuống .cvimodel',

        // Status
        status_pending: 'Đang chờ',
        status_running: 'Đang chạy',
        status_completed: 'Hoàn thành',
        status_failed: 'Thất bại',
        status_skipped: 'Bỏ qua',
    }
};

let currentLang = localStorage.getItem('lailab_lang') || 'en';

function setLanguage(lang) {
    currentLang = lang;
    localStorage.setItem('lailab_lang', lang);
    document.documentElement.lang = lang === 'vi' ? 'vi' : 'en';

    const labelEl = document.getElementById('langLabel');
    if (labelEl) labelEl.textContent = lang.toUpperCase();

    document.querySelectorAll('[data-i18n]').forEach(el => {
        const key = el.getAttribute('data-i18n');
        if (translations[lang] && translations[lang][key]) {
            el.innerHTML = translations[lang][key];
        }
    });

    // Fire custom event for app page
    window.dispatchEvent(new CustomEvent('languageChanged', { detail: { lang } }));
}

function toggleLanguage() {
    setLanguage(currentLang === 'en' ? 'vi' : 'en');
}

function t(key) {
    return (translations[currentLang] && translations[currentLang][key]) || key;
}

// Initialize
document.addEventListener('DOMContentLoaded', () => {
    const langBtn = document.getElementById('langToggle');
    if (langBtn) langBtn.addEventListener('click', toggleLanguage);
    setLanguage(currentLang);
});
