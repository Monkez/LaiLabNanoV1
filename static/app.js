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
    let activeDockerSetupTask = null;
    let dockerSetupPollTimer = null;

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

        socket.on('docker_setup_status', (data) => {
            if (activeDockerSetupTask && activeDockerSetupTask !== 'pending' && data.task_id !== activeDockerSetupTask) return;
            renderDockerSetupStatus(data);
        });

        socket.on('docker_setup_complete', async (data) => {
            if (activeDockerSetupTask && activeDockerSetupTask !== 'pending' && data.task_id !== activeDockerSetupTask) return;
            await completeDockerSetup(data);
            return;

            const lang = localStorage.getItem('lailab_lang') || 'en';
            const ok = data.status === 'completed';
            const message = ok
                ? (lang === 'vi' ? `Container ${data.container} đã sẵn sàng.` : `Container ${data.container} is ready.`)
                : (data.error || (lang === 'vi' ? `Không thể chuẩn bị ${data.container}.` : `Could not prepare ${data.container}.`));

            setDockerSetupState(ok ? 'success' : 'error', message);
            activeDockerSetupTask = null;

            const btnDockerSetup = document.getElementById('btnDockerSetup');
            if (btnDockerSetup) btnDockerSetup.disabled = false;

            await checkDockerStatus();
            const containerSelect = document.getElementById('dockerContainer');
            if (containerSelect && data.container) {
                const exists = [...containerSelect.options].some(o => o.value === data.container);
                if (exists) containerSelect.value = data.container;
            }
        });

        // Inference meta listener
        let yoloFrames = 0;
        let lastYoloMetaTime = Date.now();
        const lblYoloFps = document.getElementById('lblYoloFps');
        const lblVideoFps = document.getElementById('lblVideoFps');

        socket.on('inference_meta', (data) => {
            drawInferenceOverlay(data);
            
            yoloFrames++;
            const now = Date.now();
            if (now - lastYoloMetaTime >= 1000) {
                const fps = yoloFrames / ((now - lastYoloMetaTime) / 1000);
                if (lblYoloFps) lblYoloFps.textContent = fps.toFixed(1);
                yoloFrames = 0;
                lastYoloMetaTime = now;
            }
        });
        
        socket.on('video_meta', (data) => {
            if (lblVideoFps && data.fps !== undefined) {
                lblVideoFps.textContent = data.fps.toFixed(1);
            }
        });

        // Deploy log listener — shows in LOGS panel
        socket.on('deploy_log', (entry) => {
            const logConsole = document.getElementById('deployLogConsole');
            if (!logConsole) return;

            // Remove empty placeholder
            const emptyEl = logConsole.querySelector('.log-empty');
            if (emptyEl) emptyEl.remove();

            const div = document.createElement('div');
            div.className = `log-entry ${entry.level || 'info'}`;
            const time = new Date(entry.time * 1000);
            const timeStr = time.toLocaleTimeString('en-US', { hour12: false, hour: '2-digit', minute: '2-digit', second: '2-digit' });
            div.innerHTML = `<span class="log-time">${timeStr}</span><span class="log-message">${entry.message.replace(/</g,'&lt;').replace(/>/g,'&gt;')}</span>`;
            logConsole.appendChild(div);
            logConsole.scrollTop = logConsole.scrollHeight;
        });

        // Deploy steps listener — renders pipeline in DEPLOYMENT STATUS panel
        socket.on('deploy_steps', (data) => {
            const statusPanel = document.getElementById('deployEmpty');
            if (!statusPanel) return;

            const steps = data.steps || [];
            const icons = {
                pending: '○',
                running: '◉',
                completed: '✓',
                failed: '✗',
                skipped: '—',
            };

            let html = '<div class="deploy-step-list">';
            steps.forEach((step, i) => {
                html += `
                    <div class="deploy-step-item ${step.status}">
                        <div class="deploy-step-indicator ${step.status}">${icons[step.status] || '○'}</div>
                        <div class="deploy-step-info">
                            <div class="deploy-step-name">${step.name}</div>
                            ${step.detail ? `<div class="deploy-step-detail">${step.detail}</div>` : ''}
                        </div>
                    </div>`;
            });
            html += '</div>';
            statusPanel.innerHTML = html;
        });

        // Deploy completion listener
        socket.on('deploy_complete', (data) => {
            if (btnDeploy) btnDeploy.disabled = false;
        });

        // Serial data listeners
        socket.on('serial_data', (data) => {
            appendSerialLog(data.text);
            if (typeof processSerialVizData === 'function') {
                processSerialVizData(data.text);
            }
        });
        socket.on('serial_error', (data) => {
            appendSerialLog(`[ERROR] ${data.error}\n`, true);
            // Optionally auto-disconnect the UI
            if (serialActive) {
                disconnectSerialUI();
            }
        });
    }

    function appendSerialLog(text, isError=false) {
        const consoleEl = document.getElementById('serialConsole');
        if (!consoleEl) return;
        
        const span = document.createElement('span');
        span.textContent = text;
        if (isError) span.style.color = '#ef4444';
        
        consoleEl.appendChild(span);
        // Auto-scroll to bottom
        consoleEl.scrollTop = consoleEl.scrollHeight;
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
    const btnDockerSetup = document.getElementById('btnDockerSetup');
    const dockerSetupMeta = document.getElementById('dockerSetupMeta');
    const dockerSetupProgress = document.getElementById('dockerSetupProgress');
    const dockerSetupProgressFill = document.getElementById('dockerSetupProgressFill');
    const dockerSetupBtnLabel = btnDockerSetup ? btnDockerSetup.querySelector('span') : null;
    const dockerSetupDefaultLabel = dockerSetupBtnLabel ? dockerSetupBtnLabel.textContent : 'Download Docker Container';

    function setDockerSetupState(state, message) {
        if (!dockerSetupMeta) return;
        dockerSetupMeta.className = `docker-setup-meta ${state || ''}`.trim();
        dockerSetupMeta.textContent = message;
    }

    function setDockerSetupProgress(progress) {
        const safeProgress = Math.max(0, Math.min(100, parseInt(progress || 0, 10)));
        if (dockerSetupProgress) dockerSetupProgress.classList.remove('hidden');
        if (dockerSetupProgressFill) dockerSetupProgressFill.style.width = `${safeProgress}%`;
    }

    function renderDockerSetupStatus(data) {
        const state = data.status === 'failed' ? 'error' : data.status === 'completed' ? 'success' : 'running';
        const progress = data.progress ?? (state === 'success' ? 100 : 0);
        const message = data.error || data.message || `Preparing ${data.container || 'Docker container'}...`;
        setDockerSetupState(state, `${progress}% - ${message}`);
        setDockerSetupProgress(progress);

        if (btnDockerSetup) btnDockerSetup.disabled = state === 'running';
        if (dockerSetupBtnLabel) {
            dockerSetupBtnLabel.textContent = state === 'running'
                ? `Downloading... ${progress}%`
                : dockerSetupDefaultLabel;
        }
    }

    function stopDockerSetupPolling() {
        if (dockerSetupPollTimer) {
            clearInterval(dockerSetupPollTimer);
            dockerSetupPollTimer = null;
        }
    }

    function startDockerSetupPolling(taskId) {
        stopDockerSetupPolling();
        dockerSetupPollTimer = setInterval(async () => {
            try {
                const res = await fetch(`/api/docker/setup/${taskId}`);
                const contentType = res.headers.get('content-type') || '';
                const data = contentType.includes('application/json')
                    ? await res.json()
                    : { error: await res.text() };
                if (!res.ok) throw new Error(data.error || 'Could not read Docker setup status.');

                renderDockerSetupStatus(data);
                if (data.status === 'completed' || data.status === 'failed') {
                    await completeDockerSetup(data);
                }
            } catch (err) {
                stopDockerSetupPolling();
                activeDockerSetupTask = null;
                if (btnDockerSetup) btnDockerSetup.disabled = false;
                if (dockerSetupBtnLabel) dockerSetupBtnLabel.textContent = dockerSetupDefaultLabel;
                setDockerSetupState('error', err.message);
                appendLog({
                    time: Date.now() / 1000,
                    message: `Docker setup status error: ${err.message}`,
                    level: 'error'
                });
            }
        }, 1000);
    }

    async function completeDockerSetup(data) {
        stopDockerSetupPolling();
        renderDockerSetupStatus(data);
        activeDockerSetupTask = null;

        if (btnDockerSetup) btnDockerSetup.disabled = false;
        if (dockerSetupBtnLabel) dockerSetupBtnLabel.textContent = dockerSetupDefaultLabel;

        await checkDockerStatus();
        const containerSelect = document.getElementById('dockerContainer');
        if (containerSelect && data.container) {
            const exists = [...containerSelect.options].some(o => o.value === data.container);
            if (exists) containerSelect.value = data.container;
        }
    }

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
                        const defaultContainer = 'TPU-LAILAB-NANO-CONTAINER';
                        const hasDefault = data.containers.some(c => c.name === defaultContainer);
                        containerSelect.innerHTML = '';

                        if (data.containers.length > 0 && !hasDefault) {
                            const fallback = document.createElement('option');
                            fallback.value = defaultContainer;
                            fallback.textContent = 'TPU-LAILAB-NANO (auto)';
                            containerSelect.appendChild(fallback);
                        }

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

    if (btnDockerSetup) {
        btnDockerSetup.addEventListener('click', async () => {
            const lang = localStorage.getItem('lailab_lang') || 'en';
            const dockerEl = document.getElementById('dockerContainer');
            const container = dockerEl ? dockerEl.value.trim() : 'TPU-LAILAB-NANO-CONTAINER';

            if (!container) {
                setDockerSetupState('error', lang === 'vi'
                    ? 'Vui lòng chọn Docker container trước.'
                    : 'Please select a Docker container first.');
                return;
            }

            btnDockerSetup.disabled = true;
            activeDockerSetupTask = 'pending';
            clearLogs();
            setDockerSetupProgress(1);
            if (dockerSetupBtnLabel) dockerSetupBtnLabel.textContent = 'Starting...';
            setDockerSetupState('running', lang === 'vi'
                ? `Đang chuẩn bị ${container}...`
                : `Preparing ${container}...`);
            appendLog({
                time: Date.now() / 1000,
                message: lang === 'vi'
                    ? `Bắt đầu tải/chuẩn bị Docker container "${container}".`
                    : `Starting Docker container download/setup for "${container}".`,
                level: 'info'
            });

            try {
                const res = await fetch('/api/docker/setup', {
                    method: 'POST',
                    headers: { 'Content-Type': 'application/json' },
                    body: JSON.stringify({ container })
                });
                const data = await res.json();

                if (!res.ok) {
                    throw new Error(data.error || 'Docker setup failed');
                }

                if (activeDockerSetupTask !== null) {
                    activeDockerSetupTask = data.task_id;
                    renderDockerSetupStatus(data);
                    startDockerSetupPolling(data.task_id);
                }
            } catch (err) {
                stopDockerSetupPolling();
                activeDockerSetupTask = null;
                btnDockerSetup.disabled = false;
                if (dockerSetupBtnLabel) dockerSetupBtnLabel.textContent = dockerSetupDefaultLabel;
                setDockerSetupState('error', err.message);
                appendLog({
                    time: Date.now() / 1000,
                    message: `Docker setup failed: ${err.message}`,
                    level: 'error'
                });
            }
        });
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
        'guide': { en: 'User Guide', vi: 'Hướng dẫn sử dụng' },
        'model-prep': { en: 'Model Preparation', vi: 'Chuẩn bị Model' },
        'deploy': { en: 'Deployment', vi: 'Triển khai' },
        'inference': { en: 'Test Inference', vi: 'Kiểm thử Inference' },
    };
    const pageSubtitles = {
        'guide': { en: 'Quick Start Guide to Edge AI pipelines', vi: 'Hướng dẫn nhanh quy trình Edge AI' },
        'model-prep': { en: 'Convert YOLO models for edge device deployment', vi: 'Chuyển đổi model YOLO cho thiết bị biên' },
        'deploy': { en: 'Deploy models to LaiLab Nano device', vi: 'Triển khai model lên thiết bị LaiLab Nano' },
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

    // Ping button — calls real backend
    if (btnPingDevice) {
        btnPingDevice.addEventListener('click', async () => {
            const ip = document.getElementById('deployDeviceIp')?.value?.trim();
            if (!ip) return;

            btnPingDevice.disabled = true;
            btnPingDevice.className = 'btn-ping';
            btnPingDevice.querySelector('span').textContent = 'Pinging...';

            const deviceStatusEl = document.getElementById('deviceStatus');

            try {
                const res = await fetch('/api/device/ping', {
                    method: 'POST',
                    headers: { 'Content-Type': 'application/json' },
                    body: JSON.stringify({ ip })
                });
                const data = await res.json();

                if (data.reachable) {
                    btnPingDevice.className = 'btn-ping success';
                    btnPingDevice.querySelector('span').textContent = `${data.avg_ms}ms`;
                    if (deviceStatusEl) {
                        deviceStatusEl.innerHTML = `
                            <div class="status-indicator online">
                                <span class="status-dot"></span>
                                <span>Online — ${data.avg_ms}ms</span>
                            </div>
                        `;
                    }
                } else {
                    btnPingDevice.className = 'btn-ping fail';
                    btnPingDevice.querySelector('span').textContent = 'Offline';
                    if (deviceStatusEl) {
                        deviceStatusEl.innerHTML = `
                            <div class="status-indicator offline">
                                <span class="status-dot"></span>
                                <span>Unreachable</span>
                            </div>
                        `;
                    }
                }
            } catch (e) {
                btnPingDevice.className = 'btn-ping fail';
                btnPingDevice.querySelector('span').textContent = 'Error';
            } finally {
                btnPingDevice.disabled = false;
                // Reset button text after 3 seconds
                setTimeout(() => {
                    btnPingDevice.querySelector('span').textContent = 'Ping';
                    btnPingDevice.className = 'btn-ping';
                }, 3000);
            }
        });
    }

    // Deploy button
    if (btnDeploy) {
        btnDeploy.addEventListener('click', async () => {
            const ip = document.getElementById('deployDeviceIp')?.value.trim();
            if (!ip) return;
            
            const streamWidth = document.getElementById('streamWidth')?.value || 640;
            const streamHeight = document.getElementById('streamHeight')?.value || 480;
            const yoloW = document.getElementById('yoloInputW')?.value || 320;
            const yoloH = document.getElementById('yoloInputH')?.value || 320;
            const camWidth = document.getElementById('camWidth')?.value || 640;
            const camHeight = document.getElementById('camHeight')?.value || 480;
            const confThresh = document.getElementById('confThresh')?.value || 0.5;
            const nmsThresh = document.getElementById('nmsThresh')?.value || 0.5;
            const noYolo = document.getElementById('noYolo')?.checked || false;
            const jpegQuality = document.getElementById('jpegQuality')?.value || 70;
            const uartDev = document.getElementById('uartDev')?.value || '/dev/ttyS0';
            const baudRate = document.getElementById('baudRate')?.value || 115200;
            
            // Get password from SSH password field (shared with deploy)
            const password = document.getElementById('sshPassword')?.value || '';
            const user = document.getElementById('sshUser')?.value?.trim() || 'root';
            
            let modelFilename = currentDeployModel ? currentDeployModel.filename : null;
            if (deployUploadedFile) {
                modelFilename = deployUploadedFile.name;
            }

            if (!modelFilename) return;

            btnDeploy.disabled = true;
            const logConsole = document.getElementById('deployLogConsole');
            if (logConsole) logConsole.innerHTML = '';

            // Reset status panel — steps will be populated by deploy_steps WS event
            const statusPanel = document.getElementById('deployEmpty');
            if (statusPanel) statusPanel.innerHTML = '<span class="deploy-status-text">⏳ Starting deployment...</span>';

            try {
                const res = await fetch('/api/deploy', {
                    method: 'POST',
                    headers: { 'Content-Type': 'application/json' },
                    body: JSON.stringify({ 
                        ip, model_filename: modelFilename, streamWidth, streamHeight, yoloW, yoloH, 
                        password, user,
                        camWidth, camHeight, confThresh, nmsThresh, noYolo, jpegQuality, uartDev, baudRate
                    })
                });
                
                if (!res.ok) {
                    const data = await res.json();
                    if (statusPanel) statusPanel.innerHTML = `<span class="deploy-status-text" style="color:#ef4444">❌ ${data.error || 'Deploy failed'}</span>`;
                    btnDeploy.disabled = false;
                }
            } catch (e) {
                console.error('Deploy error', e);
                if (statusPanel) statusPanel.innerHTML = '<span class="deploy-status-text" style="color:#ef4444">❌ Network error</span>';
                btnDeploy.disabled = false;
            }
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

    // ---- Test Inference Logic ----
    const btnConnectInference = document.getElementById('btnConnectInference');
    const inferenceDeviceIp = document.getElementById('inferenceDeviceIp');
    const mjpegStream = document.getElementById('mjpegStream');
    const noStreamOverlay = document.getElementById('noStreamOverlay');
    const yoloOverlay = document.getElementById('yoloOverlay');
    let inferenceActive = false;
    let inferenceMetaTimer = null;

    if (btnConnectInference) {
        btnConnectInference.addEventListener('click', () => {
            const ip = inferenceDeviceIp.value.trim();
            if (!ip) return;
            
            if (!inferenceActive) {
                // Connect
                inferenceActive = true;
                btnConnectInference.innerHTML = `<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" width="14" height="14"><rect x="6" y="6" width="12" height="12"/></svg><span>Disconnect</span>`;
                btnConnectInference.classList.replace('btn-primary', 'btn-secondary');
                
                mjpegStream.src = `/api/stream_proxy?ip=${ip}&t=` + Date.now();
                mjpegStream.style.display = 'block';
                noStreamOverlay.style.display = 'none';
                
                // Tell backend to start listening for UDP metrics from this IP
                socket.emit('start_inference', { ip: ip });
            } else {
                // Disconnect
                inferenceActive = false;
                btnConnectInference.innerHTML = `<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" width="14" height="14"><polygon points="5 3 19 12 5 21 5 3"/></svg><span>Connect Data</span>`;
                btnConnectInference.classList.replace('btn-secondary', 'btn-primary');
                
                mjpegStream.src = '';
                mjpegStream.style.display = 'none';
                noStreamOverlay.style.display = 'flex';
                
                if (yoloOverlay) {
                    const ctx = yoloOverlay.getContext('2d');
                    ctx.clearRect(0, 0, yoloOverlay.width, yoloOverlay.height);
                }
                
                const yoloFpsEl = document.getElementById('lblYoloFps');
                const videoFpsEl = document.getElementById('lblVideoFps');
                if (yoloFpsEl) yoloFpsEl.textContent = '--';
                if (videoFpsEl) videoFpsEl.textContent = '--';
                
                socket.emit('stop_inference', {});
            }
        });
    }

    function drawInferenceOverlay(data) {
        if (!inferenceActive || !yoloOverlay) return;
        const ctx = yoloOverlay.getContext('2d');
        
        // Ensure yoloOverlay internal width/height matches its CSS rendering size
        if (yoloOverlay.width !== yoloOverlay.clientWidth || yoloOverlay.height !== yoloOverlay.clientHeight) {
            yoloOverlay.width = yoloOverlay.clientWidth;
            yoloOverlay.height = yoloOverlay.clientHeight;
        }
        
        ctx.clearRect(0, 0, yoloOverlay.width, yoloOverlay.height);
        
        if (!data || !data.objs) return;
        
        // Extract resolutions from metadata (with fallbacks)
        const yw = data.yw || 320;
        const yh = data.yh || 320;
        const sw = data.sw || 640;
        const sh = data.sh || 480;

        // 1. YOLO -> Stream scaling (VPSS ASPECT_RATIO_AUTO)
        const yoloScale = Math.min(yw / sw, yh / sh);
        const yoloNewW = sw * yoloScale;
        const yoloNewH = sh * yoloScale;
        const yoloPadX = (yw - yoloNewW) / 2;
        const yoloPadY = (yh - yoloNewH) / 2;

        // 2. Stream -> Canvas scaling (CSS object-fit: contain)
        const canvasW = yoloOverlay.width;
        const canvasH = yoloOverlay.height;
        const imgScale = Math.min(canvasW / sw, canvasH / sh);
        const imgW = sw * imgScale;
        const imgH = sh * imgScale;
        const imgPadX = (canvasW - imgW) / 2;
        const imgPadY = (canvasH - imgH) / 2;

        data.objs.forEach(obj => {
            const classId = obj.c;

            // Map from YOLO to Stream
            const streamX = (obj.x - yoloPadX) / yoloScale;
            const streamY = (obj.y - yoloPadY) / yoloScale;
            const streamW = obj.w / yoloScale;
            const streamH = obj.h / yoloScale;

            // Clamp to stream bounds (safeguard)
            const clamp = (val, min, max) => Math.max(min, Math.min(max, val));
            const x1s = clamp(streamX, 0, sw - 1);
            const y1s = clamp(streamY, 0, sh - 1);
            const x2s = clamp(streamX + streamW, 0, sw - 1);
            const y2s = clamp(streamY + streamH, 0, sh - 1);
            
            if (x2s <= x1s || y2s <= y1s) return; // invalid box

            // Map from Stream to Canvas
            const x = imgPadX + x1s * imgScale;
            const y = imgPadY + y1s * imgScale;
            const w = (x2s - x1s) * imgScale;
            const h = (y2s - y1s) * imgScale;

            const score = Math.round(obj.s * 100);

            // Draw Box
            ctx.strokeStyle = '#22c55e'; // Green box
            ctx.lineWidth = 2;
            ctx.strokeRect(x, y, w, h);

            // Draw Background for text
            ctx.fillStyle = '#22c55e';
            const labelW = w > 100 ? w : 100;
            ctx.fillRect(x, y - 24, labelW, 24);

            // Draw Text
            ctx.fillStyle = '#fff';
            ctx.font = '14px Inter, sans-serif';
            ctx.fillText(`Class ${classId} : ${score}%`, x + 4, y - 8);
        });
    }
    
    // ---- Serial Monitor Logic ----
    const serialPortSel = document.getElementById('serialPort');
    const serialBaudSel = document.getElementById('serialBaud');
    const btnRefreshPorts = document.getElementById('btnRefreshPorts');
    const btnConnectSerial = document.getElementById('btnConnectSerial');
    const serialBtnText = document.getElementById('serialBtnText');
    const serialInput = document.getElementById('serialInput');
    const btnSendSerial = document.getElementById('btnSendSerial');
    const clearSerialLogs = document.getElementById('clearSerialLogs');
    const serialConsole = document.getElementById('serialConsole');
    const serialVizCanvas = document.getElementById('serialVizCanvas');
    const serialViewMode = document.getElementById('serialViewMode');

    let serialActive = false;
    let serialVizIsActive = false;

    // Viz Data
    let serialBuffer = '';
    let serialYoloObjects = [];
    let serialYoloImgW = 640;
    let serialYoloImgH = 480;

    const classColors = [
        '#ef4444', '#22c55e', '#3b82f6', '#eab308', '#d946ef', '#06b6d4', '#f97316'
    ];

    window.processSerialVizData = function(text) {
        if (!serialVizIsActive || !serialVizCanvas) return;
        
        serialBuffer += text;
        const lines = serialBuffer.split('\n');
        serialBuffer = lines.pop(); // Keep the last incomplete line
        
        for (let line of lines) {
            line = line.trim();
            if (line.startsWith('$YOLO,')) {
                const starIdx = line.indexOf('*');
                if (starIdx > 0) {
                    const dataPart = line.substring(6, starIdx);
                    const parts = dataPart.split(',').map(Number);
                    if (parts.length >= 4) {
                        const count = parts[1];
                        serialYoloImgW = parts[2];
                        serialYoloImgH = parts[3];
                        
                        const newObjs = [];
                        let idx = 4;
                        for (let i = 0; i < count; i++) {
                            if (idx + 5 <= parts.length) {
                                newObjs.push({
                                    c: parts[idx++],
                                    x1: parts[idx++],
                                    y1: parts[idx++],
                                    x2: parts[idx++],
                                    y2: parts[idx++],
                                    s: parts[idx++]
                                });
                            }
                        }
                        serialYoloObjects = newObjs;
                    }
                }
            }
        }
    };

    function renderSerialViz() {
        if (!serialVizIsActive || !serialVizCanvas) return;
        
        const dpr = window.devicePixelRatio || 1;
        const rect = serialVizCanvas.getBoundingClientRect();
        const displayWidth = Math.floor(rect.width);
        const displayHeight = Math.floor(rect.height);
        
        if (serialVizCanvas.width !== displayWidth * dpr || serialVizCanvas.height !== displayHeight * dpr) {
            serialVizCanvas.width = displayWidth * dpr;
            serialVizCanvas.height = displayHeight * dpr;
            // Only force width/height styling if it isn't already taking full space
            // serialVizCanvas.style.width = displayWidth + 'px';
            // serialVizCanvas.style.height = displayHeight + 'px';
        }
        
        const ctx = serialVizCanvas.getContext('2d');
        ctx.resetTransform();
        ctx.scale(dpr, dpr);
        
        const W = displayWidth;
        const H = displayHeight;
        
        ctx.fillStyle = '#282828';
        ctx.fillRect(0, 0, W, H);
        
        const scaleFactor = Math.min(W / serialYoloImgW, H / serialYoloImgH);
        const drawW = serialYoloImgW * scaleFactor;
        const drawH = serialYoloImgH * scaleFactor;
        const offsetX = (W - drawW) / 2;
        const offsetY = (H - drawH) / 2;
        
        ctx.fillStyle = '#141414';
        ctx.strokeStyle = '#646464';
        ctx.lineWidth = 1;
        ctx.fillRect(offsetX, offsetY, drawW, drawH);
        ctx.strokeRect(offsetX, offsetY, drawW, drawH);
        
        serialYoloObjects.forEach(obj => {
            const px1 = offsetX + (obj.x1 / serialYoloImgW) * drawW;
            const py1 = offsetY + (obj.y1 / serialYoloImgH) * drawH;
            const px2 = offsetX + (obj.x2 / serialYoloImgW) * drawW;
            const py2 = offsetY + (obj.y2 / serialYoloImgH) * drawH;
            const boxW = px2 - px1;
            const boxH = py2 - py1;
            
            const color = classColors[obj.c % classColors.length];
            
            ctx.strokeStyle = color;
            ctx.lineWidth = 3;
            ctx.strokeRect(px1, py1, boxW, boxH);
            
            ctx.fillStyle = color;
            const labelW = Math.max(120, boxW);
            ctx.fillRect(px1, py1 - 22, labelW, 22);
            
            ctx.fillStyle = '#ffffff';
            ctx.font = '600 14px Inter, sans-serif';
            ctx.fillText(`Class ${obj.c} (${obj.s}%)`, px1 + 4, py1 - 6);
        });
        
        ctx.fillStyle = '#ffffff';
        ctx.font = '600 16px Inter, sans-serif';
        // Draw slightly inset
        ctx.fillText(`Objects: ${serialYoloObjects.length}`, 16, 28);
        ctx.fillText(`Resolution: ${serialYoloImgW} × ${serialYoloImgH}`, 16, 52);
        
        requestAnimationFrame(renderSerialViz);
    }

    if (serialViewMode) {
        serialViewMode.addEventListener('change', () => {
            if (serialViewMode.value === 'viz') {
                serialConsole.style.display = 'none';
                serialVizCanvas.style.display = 'block';
                serialVizIsActive = true;
                requestAnimationFrame(renderSerialViz);
            } else {
                serialConsole.style.display = 'block';
                serialVizCanvas.style.display = 'none';
                serialVizIsActive = false;
            }
        });
    }

    async function loadSerialPorts() {
        if (!serialPortSel) return;
        try {
            const res = await fetch('/api/serial/ports');
            const data = await res.json();
            serialPortSel.innerHTML = '';
            
            if (data.length === 0) {
                const opt = document.createElement('option');
                opt.value = "";
                opt.textContent = "No ports found";
                opt.disabled = true;
                opt.selected = true;
                serialPortSel.appendChild(opt);
                return;
            }

            data.forEach(p => {
                const opt = document.createElement('option');
                opt.value = p.port;
                opt.textContent = `${p.port} (${p.desc})`;
                serialPortSel.appendChild(opt);
            });
        } catch (e) {
            console.error("Failed to load serial ports:", e);
        }
    }

    if (btnRefreshPorts) {
        btnRefreshPorts.addEventListener('click', loadSerialPorts);
        // Load initially
        loadSerialPorts();
    }

    function disconnectSerialUI() {
        serialActive = false;
        serialBtnText.textContent = 'Open Serial Port';
        btnConnectSerial.classList.replace('btn-secondary', 'btn-primary');
        serialInput.disabled = true;
        btnSendSerial.disabled = true;
        serialPortSel.disabled = false;
        serialBaudSel.disabled = false;
        appendSerialLog('\n[DISCONNECTED]\n', true);
    }

    if (btnConnectSerial) {
        btnConnectSerial.addEventListener('click', async () => {
            if (!serialActive) {
                const port = serialPortSel.value;
                const baudrate = serialBaudSel.value;
                if (!port) return;

                btnConnectSerial.disabled = true;
                
                try {
                    const res = await fetch('/api/serial/connect', {
                        method: 'POST',
                        headers: { 'Content-Type': 'application/json' },
                        body: JSON.stringify({ port, baudrate })
                    });
                    const data = await res.json();
                    
                    if (res.ok) {
                        serialActive = true;
                        serialBtnText.textContent = 'Close Port';
                        btnConnectSerial.classList.replace('btn-primary', 'btn-secondary');
                        serialInput.disabled = false;
                        btnSendSerial.disabled = false;
                        serialPortSel.disabled = true;
                        serialBaudSel.disabled = true;
                        serialConsole.innerHTML = '';
                        appendSerialLog(`[CONNECTED] ${port} @ ${baudrate}\n`);
                    } else {
                        appendSerialLog(`[CONNECTION FAILED] ${data.error || 'Unknown error'}\n`, true);
                    }
                } catch (e) {
                    appendSerialLog(`[NETWORK ERROR] Could not connect to backend.\n`, true);
                } finally {
                    btnConnectSerial.disabled = false;
                }
            } else {
                try {
                    btnConnectSerial.disabled = true;
                    await fetch('/api/serial/disconnect', { method: 'POST' });
                } catch (e) {
                    console.error("Disconnect error:", e);
                } finally {
                    btnConnectSerial.disabled = false;
                    disconnectSerialUI();
                }
            }
        });
    }

    async function sendSerialCommand() {
        if (!serialActive || !serialInput.value.trim()) return;
        const text = serialInput.value;
        try {
            const res = await fetch('/api/serial/write', {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify({ text })
            });
            if (res.ok) {
                appendSerialLog(`> ${text}\n`);
                serialInput.value = '';
            }
        } catch (e) {
            console.error("Failed to write to serial", e);
        }
    }

    if (btnSendSerial) {
        btnSendSerial.addEventListener('click', sendSerialCommand);
    }

    if (serialInput) {
        serialInput.addEventListener('keypress', (e) => {
            if (e.key === 'Enter') {
                e.preventDefault();
                sendSerialCommand();
            }
        });
    }

    if (clearSerialLogs) {
        clearSerialLogs.addEventListener('click', () => {
            if (serialConsole) serialConsole.innerHTML = '';
        });
    }

    // ---- SSH Terminal Logic ----
    const btnSshConnect = document.getElementById('btnSshConnect');
    const sshBtnText = document.getElementById('sshBtnText');
    const sshConsole = document.getElementById('sshConsole');
    const sshInput = document.getElementById('sshInput');
    const btnSshSend = document.getElementById('btnSshSend');
    const clearSshLogs = document.getElementById('clearSshLogs');
    let sshActive = false;

    function appendSshLog(text, cssClass = '') {
        if (!sshConsole) return;
        const span = document.createElement('span');
        if (cssClass) span.className = cssClass;
        span.textContent = text;
        sshConsole.appendChild(span);
        sshConsole.scrollTop = sshConsole.scrollHeight;
    }

    if (socket) {
        socket.on('ssh_data', (data) => {
            if (!sshConsole) return;
            const span = document.createElement('span');
            if (data.error) span.className = 'ssh-error';
            // Strip common ANSI escape codes for cleaner display
            span.textContent = data.text.replace(/\x1b\[[0-9;]*[a-zA-Z]/g, '');
            sshConsole.appendChild(span);
            sshConsole.scrollTop = sshConsole.scrollHeight;
        });
    }

    if (btnSshConnect) {
        btnSshConnect.addEventListener('click', async () => {
            const host = document.getElementById('deployDeviceIp')?.value.trim();
            const user = document.getElementById('sshUser')?.value.trim() || 'root';
            const password = document.getElementById('sshPassword')?.value || '';

            if (!host) return;

            if (!sshActive) {
                // Connect
                btnSshConnect.disabled = true;
                sshConsole.innerHTML = '';
                appendSshLog(`Connecting to ${user}@${host}...\n`, 'ssh-system');

                try {
                    const res = await fetch('/api/ssh/connect', {
                        method: 'POST',
                        headers: { 'Content-Type': 'application/json' },
                        body: JSON.stringify({ host, user, password })
                    });
                    const data = await res.json();

                    if (res.ok) {
                        sshActive = true;
                        sshBtnText.textContent = 'Disconnect';
                        btnSshConnect.classList.replace('btn-primary', 'btn-secondary');
                        sshInput.disabled = false;
                        btnSshSend.disabled = false;
                        appendSshLog(`Connected to ${user}@${host}\n`, 'ssh-system');
                    } else {
                        appendSshLog(`Connection failed: ${data.error}\n`, 'ssh-error');
                    }
                } catch (e) {
                    appendSshLog(`Network error: ${e.message}\n`, 'ssh-error');
                } finally {
                    btnSshConnect.disabled = false;
                }
            } else {
                // Disconnect
                btnSshConnect.disabled = true;
                try {
                    await fetch('/api/ssh/disconnect', { method: 'POST' });
                } catch (e) {}
                sshActive = false;
                sshBtnText.textContent = 'Connect';
                btnSshConnect.classList.replace('btn-secondary', 'btn-primary');
                sshInput.disabled = true;
                btnSshSend.disabled = true;
                appendSshLog('\n[DISCONNECTED]\n', 'ssh-system');
                btnSshConnect.disabled = false;
            }
        });
    }

    async function sendSshCommand() {
        if (!sshActive || !sshInput || !sshInput.value.trim()) return;
        const text = sshInput.value;
        try {
            const res = await fetch('/api/ssh/write', {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify({ text })
            });
            if (res.ok) {
                sshInput.value = '';
            }
        } catch (e) {
            appendSshLog(`Send failed: ${e.message}\n`, 'ssh-error');
        }
    }

    if (btnSshSend) {
        btnSshSend.addEventListener('click', sendSshCommand);
    }

    if (sshInput) {
        sshInput.addEventListener('keypress', (e) => {
            if (e.key === 'Enter') {
                e.preventDefault();
                sendSshCommand();
            }
        });
    }

    if (clearSshLogs) {
        clearSshLogs.addEventListener('click', () => {
            if (sshConsole) sshConsole.innerHTML = '';
        });
    }

});
