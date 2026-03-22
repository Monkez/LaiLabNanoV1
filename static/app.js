/* ============================================================
   LaiLab Nano V1 — App Page Logic
   WebSocket-powered real-time pipeline management
   Real Docker integration
   ============================================================ */

document.addEventListener('DOMContentLoaded', () => {
    // ---- Theme ----
    const savedTheme = localStorage.getItem('lailab_theme') || 'dark';
    document.documentElement.setAttribute('data-theme', savedTheme);

    const themeToggle = document.getElementById('appThemeToggle');
    if (themeToggle) {
        themeToggle.addEventListener('click', () => {
            const current = document.documentElement.getAttribute('data-theme');
            const next = current === 'dark' ? 'light' : 'dark';
            document.documentElement.setAttribute('data-theme', next);
            localStorage.setItem('lailab_theme', next);
        });
    }

    // ---- Sidebar Toggle (Mobile) ----
    const sidebarToggle = document.getElementById('sidebarToggle');
    const sidebar = document.getElementById('sidebar');
    if (sidebarToggle && sidebar) {
        sidebarToggle.addEventListener('click', () => {
            sidebar.classList.toggle('open');
        });
    }

    // ---- WebSocket Connection ----
    let socket = null;
    const connectionStatus = document.getElementById('connectionStatus');

    function connectWebSocket() {
        socket = io(window.location.origin, {
            transports: ['websocket', 'polling']
        });

        socket.on('connect', () => {
            updateConnectionStatus(true);
            console.log('[WS] Connected');
        });

        socket.on('disconnect', () => {
            updateConnectionStatus(false);
            console.log('[WS] Disconnected');
        });

        socket.on('connected', (data) => {
            console.log('[WS] Server acknowledged:', data);
        });

        socket.on('job_update', (data) => {
            console.log('[WS] Job update:', data);
            currentJob = data;
            renderPipeline(data);
        });

        socket.on('job_log', (data) => {
            appendLog(data);
        });
    }

    function updateConnectionStatus(online) {
        if (!connectionStatus) return;
        const dot = connectionStatus.querySelector('.status-dot');
        const text = connectionStatus.querySelector('.status-text');
        if (online) {
            dot.className = 'status-dot online';
            text.textContent = 'Connected';
        } else {
            dot.className = 'status-dot offline';
            text.textContent = 'Disconnected';
        }
    }

    // ---- Docker Status Check ----
    const dockerStatusEl = document.getElementById('dockerStatus');

    async function checkDockerStatus() {
        try {
            const res = await fetch('/api/docker/status');
            const data = await res.json();
            if (dockerStatusEl) {
                if (data.docker_available) {
                    const runningContainers = data.containers.filter(c => c.running);
                    dockerStatusEl.innerHTML = `
                        <span class="status-dot online"></span>
                        <span class="status-text">Docker: ${data.containers.length} containers (${runningContainers.length} running)</span>
                    `;
                    dockerStatusEl.className = 'connection-status docker-ok';

                    // Populate docker container select if exists
                    const containerSelect = document.getElementById('dockerContainer');
                    if (containerSelect && containerSelect.tagName === 'SELECT') {
                        const currentVal = containerSelect.value;
                        containerSelect.innerHTML = '';

                        if (data.containers.length === 0) {
                            // No containers — use default container name
                            const fallback = document.createElement('option');
                            fallback.value = 'TPU-LAILAB-NANO-CONTAINER';
                            fallback.textContent = 'TPU-LAILAB-NANO (auto)';
                            fallback.selected = true;
                            containerSelect.appendChild(fallback);
                        } else {
                            data.containers.forEach(c => {
                                const opt = document.createElement('option');
                                opt.value = c.name;
                                opt.textContent = `${c.running ? '🟢' : '🔴'} ${c.name}`;
                                containerSelect.appendChild(opt);
                            });
                        }

                        // Restore previous selection if still valid
                        if (currentVal) {
                            const exists = [...containerSelect.options].some(o => o.value === currentVal);
                            if (exists) containerSelect.value = currentVal;
                        }
                    }
                } else {
                    dockerStatusEl.innerHTML = `
                        <span class="status-dot offline"></span>
                        <span class="status-text">Docker: Not available</span>
                    `;
                    dockerStatusEl.className = 'connection-status docker-error';
                }
            }
            return data;
        } catch (e) {
            console.error('Docker status check failed:', e);
            if (dockerStatusEl) {
                dockerStatusEl.innerHTML = `
                    <span class="status-dot offline"></span>
                    <span class="status-text">Docker: Error</span>
                `;
            }
            return null;
        }
    }

    // ---- Presets ----
    const presetSelect = document.getElementById('presetSelect');

    async function loadPresets() {
        try {
            const res = await fetch('/api/config/presets');
            const presets = await res.json();
            presets.forEach(p => {
                const opt = document.createElement('option');
                opt.value = p.id;
                opt.textContent = p.name;
                opt.dataset.config = JSON.stringify(p.config);
                const lang = localStorage.getItem('lailab_lang') || 'en';
                opt.title = lang === 'vi' ? p.description_vi : p.description_en;
                presetSelect.appendChild(opt);
            });
        } catch (e) {
            console.error('Failed to load presets:', e);
        }
    }

    if (presetSelect) {
        presetSelect.addEventListener('change', () => {
            const selected = presetSelect.selectedOptions[0];
            if (selected && selected.dataset.config) {
                const config = JSON.parse(selected.dataset.config);
                document.getElementById('modelName').value = config.model_name || '';
                document.getElementById('inputWidth').value = config.input_width || 640;
                document.getElementById('inputHeight').value = config.input_height || 640;
                document.getElementById('calibrationCount').value = config.calibration_count || 100;
                document.getElementById('quantize').value = config.quantize || 'int8';
                document.getElementById('processor').value = config.processor || 'cv181x';
                document.getElementById('tolerance').value = config.tolerance || '0.85,0.45';
            }
        });
    }


    // ---- File Upload (real upload to server) ----
    const fileDrop = document.getElementById('fileDrop');
    const fileInput = document.getElementById('fileInput');
    const fileInfo = document.getElementById('fileInfo');
    const fileName = document.getElementById('fileName');
    const fileRemove = document.getElementById('fileRemove');
    let uploadedFileName = null;  // server-side filename after upload

    if (fileDrop && fileInput) {
        fileDrop.addEventListener('click', () => fileInput.click());

        fileDrop.addEventListener('dragover', (e) => {
            e.preventDefault();
            fileDrop.classList.add('drag-over');
        });

        fileDrop.addEventListener('dragleave', () => {
            fileDrop.classList.remove('drag-over');
        });

        fileDrop.addEventListener('drop', (e) => {
            e.preventDefault();
            fileDrop.classList.remove('drag-over');
            const file = e.dataTransfer.files[0];
            if (file && file.name.endsWith('.pt')) {
                uploadFileToServer(file);
            }
        });

        fileInput.addEventListener('change', () => {
            if (fileInput.files[0]) {
                uploadFileToServer(fileInput.files[0]);
            }
        });

        if (fileRemove) {
            fileRemove.addEventListener('click', () => {
                uploadedFileName = null;
                fileInfo.classList.add('hidden');
                fileDrop.classList.remove('hidden');
                fileInput.value = '';
            });
        }
    }

    async function uploadFileToServer(file) {
        const lang = localStorage.getItem('lailab_lang') || 'en';
        fileName.textContent = lang === 'vi' ? `⏳ Đang tải lên ${file.name}...` : `⏳ Uploading ${file.name}...`;
        fileInfo.classList.remove('hidden');

        const formData = new FormData();
        formData.append('file', file);

        try {
            const res = await fetch('/api/upload', {
                method: 'POST',
                body: formData
            });
            const data = await res.json();

            if (res.ok) {
                uploadedFileName = data.filename;
                const sizeMB = (data.size / 1024 / 1024).toFixed(2);
                fileName.textContent = `📄 ${data.filename} (${sizeMB} MB) ✓`;
            } else {
                fileName.textContent = `❌ ${data.error || 'Upload failed'}`;
                uploadedFileName = null;
            }
        } catch (err) {
            fileName.textContent = `❌ Upload error: ${err.message}`;
            uploadedFileName = null;
        }
    }

    // ---- Advanced Settings Toggle ----
    const advancedToggle = document.getElementById('advancedToggle');
    const advancedSettings = document.getElementById('advancedSettings');

    if (advancedToggle && advancedSettings) {
        advancedToggle.addEventListener('click', () => {
            advancedToggle.classList.toggle('open');
            advancedSettings.classList.toggle('hidden');
        });
    }

    // ---- Form Submit ----
    const jobForm = document.getElementById('jobForm');
    const startBtn = document.getElementById('startBtn');
    let currentJob = null;

    if (jobForm) {
        jobForm.addEventListener('submit', async (e) => {
            e.preventDefault();

            // Get docker container value (could be select or input)
            const dockerEl = document.getElementById('dockerContainer');
            const dockerContainer = dockerEl ? dockerEl.value.trim() : 'TPU-LAILAB-NANO-CONTAINER';

            // Validate docker container is selected
            if (!dockerContainer) {
                const lang = localStorage.getItem('lailab_lang') || 'en';
                const msg = lang === 'vi'
                    ? '⚠ Vui lòng chọn Docker container trong Advanced Settings trước khi bắt đầu.'
                    : '⚠ Please select a Docker container in Advanced Settings before starting.';
                appendLog({ time: Date.now() / 1000, message: msg, level: 'error' });
                return;
            }

            const config = {
                model_name: document.getElementById('modelName').value,
                input_width: parseInt(document.getElementById('inputWidth').value),
                input_height: parseInt(document.getElementById('inputHeight').value),
                docker_container: dockerContainer,
                calibration_count: parseInt(document.getElementById('calibrationCount').value),
                quantize: document.getElementById('quantize').value,
                processor: document.getElementById('processor').value,
                tolerance: document.getElementById('tolerance').value,
                model_file: uploadedFileName || '',
            };

            startBtn.disabled = true;
            clearLogs();

            // Hide download banner from previous job
            const banner = document.getElementById('downloadBanner');
            if (banner) banner.classList.add('hidden');

            try {
                const res = await fetch('/api/jobs', {
                    method: 'POST',
                    headers: { 'Content-Type': 'application/json' },
                    body: JSON.stringify(config)
                });

                const job = await res.json();
                currentJob = job;
                renderPipeline(job);
                document.getElementById('pipelineActions').style.display = 'flex';
            } catch (err) {
                console.error('Failed to create job:', err);
                startBtn.disabled = false;
                appendLog({
                    time: Date.now() / 1000,
                    message: `❌ Failed to start job: ${err.message}`,
                    level: 'error'
                });
            }
        });
    }

    // ---- Cancel Button ----
    const cancelBtn = document.getElementById('cancelBtn');
    if (cancelBtn) {
        cancelBtn.addEventListener('click', async () => {
            if (currentJob && currentJob.job_id) {
                try {
                    await fetch(`/api/jobs/${currentJob.job_id}/cancel`, { method: 'POST' });
                } catch (err) {
                    console.error('Cancel failed:', err);
                }
            }
        });
    }

    // ---- Render Pipeline ----
    function renderPipeline(job) {
        const container = document.getElementById('pipelineSteps');
        const empty = document.getElementById('pipelineEmpty');
        if (!container) return;

        // Hide empty placeholder
        if (empty) empty.style.display = 'none';

        // Check for existing rendered pipeline or create new
        let wrapper = container.querySelector('.pipeline-wrapper');
        if (!wrapper) {
            wrapper = document.createElement('div');
            wrapper.className = 'pipeline-wrapper';
            container.appendChild(wrapper);
        }

        // Overall status
        const completedSteps = job.steps.filter(s => s.status === 'completed').length;
        const skippedSteps = job.steps.filter(s => s.status === 'skipped').length;
        const progress = Math.round(((completedSteps + skippedSteps) / job.total_steps) * 100);

        const statusLabel = getStatusLabel(job.status);
        const statusIcon = getStatusIcon(job.status);

        const lang = localStorage.getItem('lailab_lang') || 'en';

        // Download button HTML + show top banner
        let downloadHtml = '';
        if (job.status === 'completed' && job.output_model) {
            const rawFileName = job.output_model.replace(/\\/g, '/').split('/').pop() || 'model.cvimodel';
            const dlUrl = `/api/models/${encodeURIComponent(rawFileName)}/download`;
            // Strip job_id prefix for display
            const parts = rawFileName.split('_');
            const displayName = parts.length > 1 ? parts.slice(1).join('_') : rawFileName;
            const dlLabel = lang === 'vi' ? '📥 Tải xuống' : '📥 Download';
            downloadHtml = `
                <a href="${dlUrl}" class="btn-download" download="${displayName}">
                    ${dlLabel}
                </a>
            `;

            // Show prominent download banner at top
            const banner = document.getElementById('downloadBanner');
            const bannerBtn = document.getElementById('downloadBannerBtn');
            const bannerName = document.getElementById('downloadModelName');
            if (banner && bannerBtn) {
                bannerName.textContent = displayName;
                bannerBtn.href = dlUrl;
                bannerBtn.setAttribute('download', displayName);
                bannerBtn.onclick = null;
                banner.classList.remove('hidden');
            }

            // Also refresh deploy models list
            if (typeof window.refreshDeployModels === 'function') window.refreshDeployModels();
        }

        wrapper.innerHTML = `
            ${job.status !== 'pending' ? `
            <div class="pipeline-status ${job.status}">
                ${statusIcon}
                <span>${statusLabel}</span>
                ${downloadHtml}
            </div>
            ` : ''}

            <div class="pipeline-progress">
                <div class="progress-header">
                    <span class="progress-label">${lang === 'vi' ? 'Tiến trình' : 'Progress'}</span>
                    <span class="progress-value">${progress}%</span>
                </div>
                <div class="progress-bar">
                    <div class="progress-fill" style="width: ${progress}%"></div>
                </div>
            </div>

            <div class="pipeline-step-list">
                ${job.steps.map((step, i) => `
                    <div class="step-item ${step.status}">
                        <div class="step-indicator">
                            ${step.status === 'completed' ? '✓' :
                              step.status === 'failed' ? '✕' :
                              step.status === 'running' ? '▶' :
                              step.status === 'skipped' ? '—' :
                              (i + 1)}
                        </div>
                        <div class="step-info">
                            <div class="step-name">${lang === 'vi' ? step.label_vi : step.label_en}</div>
                            ${step.log ? `<div class="step-detail">${escapeHtml(step.log)}</div>` : ''}
                        </div>
                        <span class="step-status-badge">${getStepStatusLabel(step.status, lang)}</span>
                    </div>
                `).join('')}
            </div>

            ${job.status === 'completed' && job.output_model ? `
            <div class="download-section">
                <a href="/api/jobs/${job.job_id}/download" class="btn-download-lg" download>
                    <svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M21 15v4a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-4"/><polyline points="7 10 12 15 17 10"/><line x1="12" y1="15" x2="12" y2="3"/></svg>
                    ${lang === 'vi' ? 'Tải xuống Model (.cvimodel)' : 'Download Model (.cvimodel)'}
                </a>
            </div>
            ` : ''}
        `;

        // Re-enable button when job is done
        if (job.status === 'completed' || job.status === 'failed') {
            startBtn.disabled = false;
        }
    }

    function getStatusLabel(status) {
        const lang = localStorage.getItem('lailab_lang') || 'en';
        const labels = {
            en: { pending: 'Pending', running: 'Running Pipeline...', completed: 'Pipeline Completed!', failed: 'Pipeline Failed' },
            vi: { pending: 'Đang chờ', running: 'Đang chạy Pipeline...', completed: 'Pipeline hoàn thành!', failed: 'Pipeline thất bại' }
        };
        return labels[lang][status] || status;
    }

    function getStatusIcon(status) {
        switch (status) {
            case 'running':
                return '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><polyline points="16 3 21 3 21 8"/><line x1="4" y1="20" x2="21" y2="3"/></svg>';
            case 'completed':
                return '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M22 11.08V12a10 10 0 1 1-5.93-9.14"/><polyline points="22 4 12 14.01 9 11.01"/></svg>';
            case 'failed':
                return '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><circle cx="12" cy="12" r="10"/><line x1="15" y1="9" x2="9" y2="15"/><line x1="9" y1="9" x2="15" y2="15"/></svg>';
            default:
                return '';
        }
    }

    function getStepStatusLabel(status, lang) {
        const labels = {
            en: { pending: 'Pending', running: 'Running', completed: 'Done', failed: 'Failed', skipped: 'Skipped' },
            vi: { pending: 'Chờ', running: 'Đang chạy', completed: 'Xong', failed: 'Lỗi', skipped: 'Bỏ qua' }
        };
        return labels[lang][status] || status;
    }

    // ---- Log Console ----
    const logConsole = document.getElementById('logConsole');
    const clearLogsBtn = document.getElementById('clearLogs');

    function appendLog(entry) {
        if (!logConsole) return;

        // Remove empty placeholder
        const emptyEl = logConsole.querySelector('.log-empty');
        if (emptyEl) emptyEl.remove();

        const div = document.createElement('div');
        div.className = `log-entry ${entry.level || 'info'}`;

        const time = new Date(entry.time * 1000);
        const timeStr = time.toLocaleTimeString('en-US', { hour12: false, hour: '2-digit', minute: '2-digit', second: '2-digit' });

        div.innerHTML = `
            <span class="log-time">${timeStr}</span>
            <span class="log-message">${escapeHtml(entry.message)}</span>
        `;

        logConsole.appendChild(div);
        logConsole.scrollTop = logConsole.scrollHeight;
    }

    function clearLogs() {
        if (!logConsole) return;
        const lang = localStorage.getItem('lailab_lang') || 'en';
        const emptyMsg = lang === 'vi' ? 'Đang chờ pipeline khởi chạy...' : 'Waiting for pipeline to start...';
        logConsole.innerHTML = `<div class="log-empty">${emptyMsg}</div>`;
    }

    if (clearLogsBtn) {
        clearLogsBtn.addEventListener('click', clearLogs);
    }

    function escapeHtml(text) {
        const div = document.createElement('div');
        div.textContent = text;
        return div.innerHTML;
    }

    // ---- Page Navigation ----
    const pageTitles = {
        'dashboard': { en: 'Dashboard', vi: 'Bảng điều khiển' },
        'model-prep': { en: 'Model Preparation', vi: 'Chuẩn bị Model' },
        'deploy': { en: 'Deployment', vi: 'Triển khai' },
        'inference': { en: 'Test Inference', vi: 'Kiểm thử Inference' },
    };
    const pageSubtitles = {
        'dashboard': { en: 'Overview of your workspace', vi: 'Tổng quan không gian làm việc' },
        'model-prep': { en: 'Convert YOLO models for LicheeRV Nano deployment', vi: 'Chuyển đổi model YOLO cho LicheeRV Nano' },
        'deploy': { en: 'Deploy models to LicheeRV Nano device', vi: 'Triển khai model lên thiết bị LicheeRV Nano' },
        'inference': { en: 'Run inference tests on deployed models', vi: 'Chạy kiểm thử inference trên model đã triển khai' },
    };

    function switchPage(pageId) {
        // Hide all pages, show target
        document.querySelectorAll('.page-content').forEach(p => p.classList.remove('active'));
        const target = document.getElementById('page-' + pageId);
        if (target) target.classList.add('active');

        // Update sidebar active state
        document.querySelectorAll('.sidebar-link').forEach(l => l.classList.remove('active'));
        const activeLink = document.querySelector(`.sidebar-link[data-page="${pageId}"]`);
        if (activeLink) activeLink.classList.add('active');

        // Update topbar title
        const lang = localStorage.getItem('lailab_lang') || 'en';
        const topTitle = document.querySelector('.topbar-title h1');
        const topSub = document.querySelector('.topbar-title p');
        if (topTitle && pageTitles[pageId]) topTitle.textContent = pageTitles[pageId][lang] || pageTitles[pageId].en;
        if (topSub && pageSubtitles[pageId]) topSub.textContent = pageSubtitles[pageId][lang] || pageSubtitles[pageId].en;

        // Close sidebar on mobile
        if (sidebar) sidebar.classList.remove('open');
    }

    document.querySelectorAll('.sidebar-link').forEach(link => {
        link.addEventListener('click', (e) => {
            e.preventDefault();
            const pageId = link.dataset.page;
            switchPage(pageId);
        });
    });

    // ---- Deployment Page Logic ----
    const deployModelAuto = document.getElementById('deployModelAuto');
    const deployUploadGroup = document.getElementById('deployUploadGroup');
    const deployFileDrop = document.getElementById('deployFileDrop');
    const deployFileInput = document.getElementById('deployFileInput');
    const deployFileInfo = document.getElementById('deployFileInfo');
    const deployFileName = document.getElementById('deployFileName');
    const deployFileRemove = document.getElementById('deployFileRemove');
    const btnDeploy = document.getElementById('btnDeploy');
    const btnPingDevice = document.getElementById('btnPingDevice');

    let currentDeployModel = null; // { filename, display_name, size }

    // Deploy file upload
    if (deployFileDrop && deployFileInput) {
        deployFileDrop.addEventListener('click', () => deployFileInput.click());
        deployFileDrop.addEventListener('dragover', (e) => { e.preventDefault(); deployFileDrop.classList.add('dragover'); });
        deployFileDrop.addEventListener('dragleave', () => deployFileDrop.classList.remove('dragover'));
        deployFileDrop.addEventListener('drop', (e) => {
            e.preventDefault();
            deployFileDrop.classList.remove('dragover');
            if (e.dataTransfer.files.length) handleDeployFile(e.dataTransfer.files[0]);
        });
        deployFileInput.addEventListener('change', () => {
            if (deployFileInput.files.length) handleDeployFile(deployFileInput.files[0]);
        });
    }

    let deployUploadedFile = null;
    function handleDeployFile(file) {
        if (!file.name.endsWith('.cvimodel')) {
            alert('Only .cvimodel files are allowed.');
            return;
        }
        deployUploadedFile = file;
        if (deployFileName) deployFileName.textContent = `${file.name} (${(file.size / 1024 / 1024).toFixed(1)} MB)`;
        if (deployFileInfo) deployFileInfo.classList.remove('hidden');
        if (deployFileDrop) deployFileDrop.classList.add('hidden');
        updateDeployButton();
    }

    if (deployFileRemove) {
        deployFileRemove.addEventListener('click', () => {
            deployUploadedFile = null;
            if (deployFileInput) deployFileInput.value = '';
            if (deployFileInfo) deployFileInfo.classList.add('hidden');
            if (deployFileDrop) deployFileDrop.classList.remove('hidden');
            updateDeployButton();
        });
    }

    // Auto-detect model from outputs folder
    function refreshDeployModels() {
        fetch('/api/models')
            .then(r => r.json())
            .then(data => {
                const models = data.models || [];
                if (models.length > 0) {
                    // Show auto-detected model
                    currentDeployModel = models[0]; // newest
                    const nameEl = document.getElementById('deployModelName');
                    const sizeEl = document.getElementById('deployModelSize');
                    if (nameEl) nameEl.textContent = currentDeployModel.display_name;
                    if (sizeEl) sizeEl.textContent = `${(currentDeployModel.size / (1024 * 1024)).toFixed(1)} MB`;
                    if (deployModelAuto) deployModelAuto.classList.remove('hidden');
                    if (deployUploadGroup) deployUploadGroup.classList.add('hidden');
                } else {
                    // No model — show upload area
                    currentDeployModel = null;
                    if (deployModelAuto) deployModelAuto.classList.add('hidden');
                    if (deployUploadGroup) deployUploadGroup.classList.remove('hidden');
                }
                updateDeployButton();
            })
            .catch(() => {});
    }
    // Make it accessible from renderPipeline
    window.refreshDeployModels = refreshDeployModels;

    function updateDeployButton() {
        if (!btnDeploy) return;
        const hasModel = !!currentDeployModel || !!deployUploadedFile;
        btnDeploy.disabled = !hasModel;
    }

    // Ping button (frontend only — backend to be added later)
    if (btnPingDevice) {
        btnPingDevice.addEventListener('click', () => {
            const ip = document.getElementById('deployDeviceIp')?.value;
            if (!ip) return;
            btnPingDevice.className = 'btn-ping';
            btnPingDevice.querySelector('span').textContent = 'Pinging...';
            // TODO: call backend /api/device/ping endpoint
            setTimeout(() => {
                btnPingDevice.querySelector('span').textContent = 'Ping';
                // Placeholder — will be connected to backend
            }, 1500);
        });
    }

    // Deploy button (frontend only — backend to be added later)
    if (btnDeploy) {
        btnDeploy.addEventListener('click', () => {
            const lang = localStorage.getItem('lailab_lang') || 'en';
            alert(lang === 'vi' ? 'Backend triển khai sẽ được thêm sau.' : 'Deployment backend will be added later.');
        });
    }

    // Clear deploy logs
    const clearDeployLogs = document.getElementById('clearDeployLogs');
    if (clearDeployLogs) {
        clearDeployLogs.addEventListener('click', () => {
            const console = document.getElementById('deployLogConsole');
            if (console) {
                const lang = localStorage.getItem('lailab_lang') || 'en';
                console.innerHTML = `<div class="log-empty">${lang === 'vi' ? 'Đang chờ triển khai...' : 'Waiting for deployment...'}</div>`;
            }
        });
    }

    // ---- Initialize ----
    connectWebSocket();
    loadPresets();
    checkDockerStatus();
    refreshDeployModels();

    // Refresh Docker status every 30 seconds
    setInterval(checkDockerStatus, 30000);
});
