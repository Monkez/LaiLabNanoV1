"""
LaiLab Nano V1 - Backend Server
AI Edge Inference Model Preparation & Deployment Platform

Real Docker integration for YOLO model conversion pipeline.
"""

import os
import json
import subprocess
import socket
import threading
import time
import uuid
import shlex
import re
import ipaddress
import secrets
import io
import serial
import serial.tools.list_ports
import signal
from pathlib import Path
from flask import Flask, render_template, request, jsonify, send_from_directory, send_file, Response
from flask_socketio import SocketIO, emit
from werkzeug.exceptions import RequestEntityTooLarge
from werkzeug.utils import secure_filename

BASE_DIR = Path(__file__).resolve().parent
app = Flask(__name__, static_folder='static', template_folder='templates')
# This application controls Docker, serial ports and SSH. Keep browser access same-origin.
socketio = SocketIO(app, cors_allowed_origins=None, async_mode='threading')

# ============================================================
# Configuration
# ============================================================
UPLOAD_FOLDER = BASE_DIR / 'uploads'
OUTPUT_FOLDER = BASE_DIR / 'outputs'
os.makedirs(UPLOAD_FOLDER, exist_ok=True)
os.makedirs(OUTPUT_FOLDER, exist_ok=True)

ALLOWED_EXTENSIONS = {'pt'}
ALLOWED_MODEL_EXTENSIONS = {'cvimodel'}
MAX_UPLOAD_BYTES = int(os.getenv('LAI_LAB_MAX_UPLOAD_MB', '1024')) * 1024 * 1024
app.config['MAX_CONTENT_LENGTH'] = MAX_UPLOAD_BYTES
API_TOKEN = os.getenv('LAI_LAB_API_TOKEN', '')
SSH_KNOWN_HOSTS = os.getenv('LAI_LAB_SSH_KNOWN_HOSTS', str(Path.home() / '.ssh' / 'known_hosts'))

# Docker configuration
DOCKER_IMAGE = 'sophgo/tpuc_dev:latest'
DOCKER_DEFAULT_CONTAINER = 'TPU-LAILAB-NANO-CONTAINER'
DOCKER_TPU_MLIR_PATH = '/workspace/tpu-mlir'
DOCKER_WORK_BASE = '/workspace/tpu-mlir'
TPU_MLIR_VERSION = 'v1.7'
# A clean TPU-MLIR build can exceed 30 minutes on Docker Desktop, especially
# when CPU/RAM assigned to the VM is limited. Allow deployments to tune this.
TPU_MLIR_BUILD_TIMEOUT = max(1800, int(os.getenv('LAI_LAB_TPU_MLIR_BUILD_TIMEOUT', '7200')))
TPU_MLIR_BUILD_MARKER = f'{DOCKER_TPU_MLIR_PATH}/.lailab-build-complete-{TPU_MLIR_VERSION}'
# TPU-MLIR v1.7's native activation collector can segfault with large YOLO11
# calibration sets. Fifty images was verified with the 320x320 YOLO11 graph.
TPU_MLIR_SAFE_CALIBRATION_IMAGES = 50

# Track running jobs
jobs = {}
jobs_lock = threading.RLock()
# Track Docker setup requests
docker_setup_tasks = {}
# Track running subprocesses for cancellation
running_processes = {}
device_lock = threading.RLock()
deployment_lock = threading.Lock()


def api_error(message, status=400):
    return jsonify({'error': message}), status


def request_data():
    data = request.get_json(silent=True)
    if not isinstance(data, dict):
        raise ValueError('A JSON object is required.')
    return data


def safe_filename(name, extensions):
    if not isinstance(name, str):
        return None
    name = secure_filename(name)
    if not name or '.' not in name:
        return None
    extension = name.rsplit('.', 1)[1].lower()
    return name if extension in extensions else None


def safe_path(folder, filename, extensions):
    name = safe_filename(filename, extensions)
    if not name:
        return None
    candidate = (Path(folder) / name).resolve()
    try:
        candidate.relative_to(Path(folder).resolve())
    except ValueError:
        return None
    return candidate


def valid_ipv4(value):
    try:
        return str(ipaddress.IPv4Address(str(value).strip()))
    except (ipaddress.AddressValueError, ValueError):
        return None


def bounded_int(value, name, minimum, maximum):
    try:
        parsed = int(value)
    except (TypeError, ValueError):
        raise ValueError(f'{name} must be an integer.')
    if not minimum <= parsed <= maximum:
        raise ValueError(f'{name} must be between {minimum} and {maximum}.')
    return parsed


def bounded_float(value, name, minimum, maximum):
    try:
        parsed = float(value)
    except (TypeError, ValueError):
        raise ValueError(f'{name} must be a number.')
    if not minimum <= parsed <= maximum:
        raise ValueError(f'{name} must be between {minimum} and {maximum}.')
    return parsed


def build_ssh_client():
    import paramiko
    client = paramiko.SSHClient()
    if os.path.exists(SSH_KNOWN_HOSTS):
        client.load_host_keys(SSH_KNOWN_HOSTS)
    client.load_system_host_keys()
    client.set_missing_host_key_policy(paramiko.RejectPolicy())
    return client


@app.before_request
def protect_api():
    """Optional bearer token for deployments that deliberately expose the UI."""
    if not API_TOKEN or not request.path.startswith('/api/'):
        return None
    provided = request.headers.get('X-Api-Token', '')
    if not secrets.compare_digest(provided, API_TOKEN):
        return api_error('Unauthorized', 401)
    return None


@app.errorhandler(RequestEntityTooLarge)
def upload_too_large(_error):
    return api_error(f'Upload exceeds the {MAX_UPLOAD_BYTES // (1024 * 1024)} MB limit.', 413)


def allowed_file(filename):
    return safe_filename(filename, ALLOWED_EXTENSIONS) is not None


# ============================================================
# Docker Utilities
# ============================================================
def docker_exec(container, command, cwd=None, job=None, timeout=None):
    """
    Execute a command inside a Docker container and stream output in real-time.
    Returns (success: bool, output: str).
    """
    # Build the full command to run inside docker
    if cwd:
        full_cmd = f'cd {cwd} && {command}'
    else:
        full_cmd = command

    docker_cmd = [
        'docker', 'exec', container,
        '/bin/bash', '-c', full_cmd
    ]

    if job:
        emit_log(job, f'$ {command}', 'info')

    try:
        process = subprocess.Popen(
            docker_cmd,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            bufsize=1,
            encoding='utf-8',
            errors='replace'
        )

        # Track process for cancellation
        if job:
            running_processes[job.job_id] = process

        output_lines = []
        start_time = time.time()

        for line in iter(process.stdout.readline, ''):
            line = line.rstrip('\n\r')
            if line:
                output_lines.append(line)
                if job:
                    emit_log(job, line, 'info')

            # Check timeout
            if timeout and (time.time() - start_time) > timeout:
                process.kill()
                if job:
                    emit_log(job, f'⚠ Command timed out after {timeout}s', 'warning')
                return False, '\n'.join(output_lines)

            # Check if job was cancelled
            if job and job.status in {'failed', 'cancelled'}:
                process.kill()
                return False, '\n'.join(output_lines)

        process.stdout.close()
        return_code = process.wait()

        # Cleanup process tracking
        if job and job.job_id in running_processes:
            del running_processes[job.job_id]

        if return_code != 0:
            if job:
                emit_log(job, f'Command exited with code {return_code}', 'error')
            return False, '\n'.join(output_lines)

        return True, '\n'.join(output_lines)

    except FileNotFoundError:
        msg = 'Docker not found. Make sure Docker is installed and running.'
        if job:
            emit_log(job, msg, 'error')
        return False, msg
    except Exception as e:
        msg = f'Docker exec error: {str(e)}'
        if job:
            emit_log(job, msg, 'error')
        return False, msg


def docker_is_running(container):
    """Check if a Docker container is running."""
    try:
        result = subprocess.run(
            ['docker', 'inspect', '-f', '{{.State.Running}}', container],
            capture_output=True, text=True, timeout=10
        )
        return result.stdout.strip() == 'true'
    except Exception:
        return False


def docker_start(container):
    """Start a Docker container."""
    try:
        result = subprocess.run(
            ['docker', 'start', container],
            capture_output=True, text=True, timeout=30
        )
        return result.returncode == 0
    except Exception:
        return False


def docker_container_exists(container):
    """Check if a Docker container exists (running or stopped)."""
    try:
        result = subprocess.run(
            ['docker', 'inspect', container],
            capture_output=True, text=True, timeout=10
        )
        return result.returncode == 0
    except Exception:
        return False


def docker_image_exists(image):
    """Check if a Docker image exists locally."""
    try:
        result = subprocess.run(
            ['docker', 'image', 'inspect', image],
            capture_output=True, text=True, timeout=10
        )
        return result.returncode == 0
    except Exception:
        return False


def docker_pull_image(image, job=None, progress_callback=None):
    """Pull a Docker image from registry."""
    try:
        if job:
            emit_log(job, f'Pulling Docker image: {image} (this may take a while)...', 'warning')

        process = subprocess.Popen(
            ['docker', 'pull', image],
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            bufsize=1,
            encoding='utf-8',
            errors='replace'
        )

        line_count = 0
        for line in iter(process.stdout.readline, ''):
            line = line.rstrip('\n\r')
            if line:
                line_count += 1
                if job:
                    emit_log(job, line, 'info')
                if progress_callback:
                    progress_callback(min(80, 30 + line_count * 2), line)

        process.stdout.close()
        return_code = process.wait()
        return return_code == 0
    except Exception as e:
        if job:
            emit_log(job, f'Failed to pull image: {str(e)}', 'error')
        return False


def docker_create_container(container, image, job=None):
    """Create and start a new Docker container from an image."""
    try:
        if job:
            emit_log(job, f'Creating container "{container}" from image "{image}"...')

        result = subprocess.run(
            ['docker', 'run', '-itd', '--name', container, image],
            capture_output=True, text=True, timeout=60
        )

        if result.returncode == 0:
            if job:
                emit_log(job, f'Container "{container}" created successfully.', 'success')
            return True
        else:
            if job:
                emit_log(job, f'Failed to create container: {result.stderr}', 'error')
            return False
    except Exception as e:
        if job:
            emit_log(job, f'Error creating container: {str(e)}', 'error')
        return False


def docker_ensure_container(container, job=None):
    """
    Ensure a Docker container exists and is running.
    If container doesn't exist, auto-pull image and create it.
    Returns True if container is ready.
    """
    # Check if container already exists
    if docker_container_exists(container):
        if docker_is_running(container):
            if job:
                emit_log(job, f'Container "{container}" is already running.', 'success')
            return True
        else:
            if job:
                emit_log(job, f'Starting stopped container "{container}"...')
            if docker_start(container):
                if job:
                    emit_log(job, f'Container "{container}" started.', 'success')
                return True
            else:
                if job:
                    emit_log(job, f'Failed to start container "{container}".', 'error')
                return False

    # Container doesn't exist — need to create it
    if job:
        emit_log(job, f'Container "{container}" not found. Setting up automatically...', 'warning')

    # Check if image exists locally
    if not docker_image_exists(DOCKER_IMAGE):
        if job:
            emit_log(job, f'Docker image "{DOCKER_IMAGE}" not found locally.', 'warning')
        # Pull the image
        if not docker_pull_image(DOCKER_IMAGE, job):
            if job:
                emit_log(job, f'Failed to pull Docker image "{DOCKER_IMAGE}". Check your internet connection.', 'error')
            return False
        if job:
            emit_log(job, f'Docker image "{DOCKER_IMAGE}" pulled successfully.', 'success')
    else:
        if job:
            emit_log(job, f'Docker image "{DOCKER_IMAGE}" found locally.', 'success')

    # Create the container
    if not docker_create_container(container, DOCKER_IMAGE, job):
        return False

    return True


def docker_copy_to(container, src_path, dst_path):
    """Copy a file from host to Docker container."""
    try:
        result = subprocess.run(
            ['docker', 'cp', src_path, f'{container}:{dst_path}'],
            capture_output=True, text=True, timeout=60
        )
        return result.returncode == 0
    except Exception:
        return False


def docker_copy_from(container, src_path, dst_path):
    """Copy a file from Docker container to host."""
    try:
        result = subprocess.run(
            ['docker', 'cp', f'{container}:{src_path}', dst_path],
            capture_output=True, text=True, timeout=120
        )
        return result.returncode == 0
    except Exception:
        return False


# ============================================================
# Conversion Job
# ============================================================
class ConversionJob:
    """Represents a model conversion pipeline job."""

    def __init__(self, job_id, config):
        self.job_id = job_id
        self.config = config
        self.status = 'pending'  # pending, running, completed, failed
        self.current_step = 0
        self.total_steps = 5
        self.steps = [
            {'name': 'setup', 'label_en': 'Environment Setup', 'label_vi': 'Thiết lập môi trường', 'status': 'pending', 'log': ''},
            {'name': 'export_onnx', 'label_en': 'Export to ONNX', 'label_vi': 'Xuất sang ONNX', 'status': 'pending', 'log': ''},
            {'name': 'model_transform', 'label_en': 'Model Transform (MLIR)', 'label_vi': 'Chuyển đổi Model (MLIR)', 'status': 'pending', 'log': ''},
            {'name': 'calibration', 'label_en': 'Run Calibration', 'label_vi': 'Chạy Calibration', 'status': 'pending', 'log': ''},
            {'name': 'model_deploy', 'label_en': 'Quantization', 'label_vi': 'Lượng tử hóa', 'status': 'pending', 'log': ''},
        ]
        self.created_at = time.time()
        self.logs = []
        self.output_model = None  # path on host to output .cvimodel

    def to_dict(self):
        # Keep the host path private and JSON-safe. Internally output_model may
        # be a pathlib.Path; clients only need its filename for display.
        output_model = Path(self.output_model).name if self.output_model else None
        return {
            'job_id': self.job_id,
            'config': self.config,
            'status': self.status,
            'current_step': self.current_step,
            'total_steps': self.total_steps,
            'steps': self.steps,
            'created_at': self.created_at,
            'logs': self.logs[-200:],
            'output_model': output_model,
        }


def emit_job_update(job):
    """Send job status update via WebSocket."""
    socketio.emit('job_update', job.to_dict(), namespace='/')


def emit_log(job, message, level='info'):
    """Send log message via WebSocket."""
    entry = {'time': time.time(), 'message': message, 'level': level}
    job.logs.append(entry)
    socketio.emit('job_log', {'job_id': job.job_id, **entry}, namespace='/')


def step_start(job, step_idx, msg_en):
    """Mark a step as running."""
    job.current_step = step_idx
    job.steps[step_idx]['status'] = 'running'
    emit_job_update(job)
    emit_log(job, msg_en)


def step_done(job, step_idx, summary):
    """Mark a step as completed."""
    job.steps[step_idx]['status'] = 'completed'
    job.steps[step_idx]['log'] = summary
    emit_log(job, f'✓ {summary}', 'success')
    emit_job_update(job)


def step_fail(job, step_idx, error_msg):
    """Mark a step as failed and abort pipeline."""
    job.steps[step_idx]['status'] = 'failed'
    job.steps[step_idx]['log'] = error_msg
    job.status = 'failed'
    emit_log(job, f'❌ {error_msg}', 'error')
    emit_job_update(job)


class DockerSetupTask:
    """Small job-like object used to stream Docker setup logs to the UI."""

    def __init__(self, task_id, container):
        self.job_id = task_id
        self.container = container
        self.status = 'pending'
        self.progress = 0
        self.message = 'Pending'
        self.error = None
        self.logs = []
        self.created_at = time.time()

    def to_dict(self):
        return {
            'task_id': self.job_id,
            'status': self.status,
            'container': self.container,
            'image': DOCKER_IMAGE,
            'progress': self.progress,
            'message': self.message,
            'error': self.error,
            'created_at': self.created_at,
            'logs': self.logs[-200:],
        }


def valid_docker_container_name(name):
    """Validate a Docker container name before passing it to docker commands."""
    return bool(re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9_.-]{0,127}', name or ''))


def validate_conversion_config(data):
    if not isinstance(data, dict):
        raise ValueError('A JSON object is required.')
    model_name = str(data.get('model_name', 'yolov8n')).strip().lower()
    if not re.fullmatch(r'[a-z0-9][a-z0-9_-]{0,63}', model_name):
        raise ValueError('model_name may contain only lowercase letters, numbers, underscores, and hyphens.')
    container = str(data.get('docker_container', DOCKER_DEFAULT_CONTAINER)).strip()
    if not valid_docker_container_name(container):
        raise ValueError('Invalid Docker container name.')
    quantize = str(data.get('quantize', 'int8')).lower()
    processor = str(data.get('processor', 'cv181x')).lower()
    if quantize not in {'int8', 'bf16', 'f32'}:
        raise ValueError('Unsupported quantization mode.')
    if processor not in {'cv181x', 'cv180x', 'cv182x'}:
        raise ValueError('Unsupported processor.')
    tolerance = str(data.get('tolerance', '0.85,0.45')).strip()
    if not re.fullmatch(r'(?:0(?:\.\d+)?|1(?:\.0+)?)\s*,\s*(?:0(?:\.\d+)?|1(?:\.0+)?)', tolerance):
        raise ValueError('tolerance must contain two values between 0 and 1.')
    model_file = data.get('model_file', '')
    if model_file:
        model_file = safe_filename(model_file, ALLOWED_EXTENSIONS)
        if not model_file or not safe_path(UPLOAD_FOLDER, model_file, ALLOWED_EXTENSIONS).is_file():
            raise ValueError('Uploaded model was not found.')
    return {
        'model_name': model_name,
        'input_width': bounded_int(data.get('input_width', 640), 'input_width', 32, 2048),
        'input_height': bounded_int(data.get('input_height', 640), 'input_height', 32, 2048),
        'docker_container': container,
        'calibration_count': bounded_int(data.get('calibration_count', 100), 'calibration_count', 1, 1000),
        'quantize': quantize,
        'processor': processor,
        'tolerance': tolerance,
        'model_file': model_file,
    }


def emit_docker_setup_status(task, progress=None, message=None, log_level=None):
    """Update and broadcast Docker setup progress."""
    if progress is not None:
        task.progress = max(0, min(100, int(progress)))
    if message:
        task.message = message
        if log_level:
            emit_log(task, message, log_level)

    socketio.emit('docker_setup_status', task.to_dict(), namespace='/')


def finish_docker_setup(task, status, message, error=None):
    """Broadcast final Docker setup status."""
    task.status = status
    task.error = error
    task.message = message
    if status == 'completed':
        task.progress = 100
        emit_log(task, message, 'success')
    else:
        emit_log(task, message, 'error')

    socketio.emit('docker_setup_complete', task.to_dict(), namespace='/')


def run_docker_setup(task):
    """Pull the required Docker image if needed and ensure the selected container is running."""
    task.status = 'running'
    emit_docker_setup_status(
        task,
        5,
        f'Checking Docker before preparing "{task.container}"...',
        'info'
    )

    try:
        result = subprocess.run(
            ['docker', 'info'], capture_output=True, text=True, timeout=10
        )
        if result.returncode != 0:
            message = 'Docker is not running or cannot be reached. Please start Docker Desktop and try again.'
            finish_docker_setup(task, 'failed', message, message)
            return
    except FileNotFoundError:
        message = 'Docker is not installed or docker command is not in PATH.'
        finish_docker_setup(task, 'failed', message, message)
        return
    except Exception as e:
        message = f'Docker check failed: {str(e)}'
        finish_docker_setup(task, 'failed', message, message)
        return

    emit_docker_setup_status(task, 12, 'Docker is running.', 'success')

    if docker_container_exists(task.container):
        if docker_is_running(task.container):
            finish_docker_setup(task, 'completed', f'Docker container "{task.container}" is already running.')
            return

        emit_docker_setup_status(task, 45, f'Starting stopped container "{task.container}"...', 'info')
        if docker_start(task.container):
            finish_docker_setup(task, 'completed', f'Docker container "{task.container}" started and is ready.')
        else:
            message = f'Failed to start Docker container "{task.container}".'
            finish_docker_setup(task, 'failed', message, message)
        return

    emit_docker_setup_status(task, 20, f'Container "{task.container}" not found. Preparing a new one...', 'warning')

    if not docker_image_exists(DOCKER_IMAGE):
        emit_docker_setup_status(task, 25, f'Pulling Docker image "{DOCKER_IMAGE}". This can take a while...', 'warning')

        def on_pull_progress(progress, line):
            emit_docker_setup_status(task, progress, f'Pulling image: {line}')

        if not docker_pull_image(DOCKER_IMAGE, task, on_pull_progress):
            message = f'Failed to pull Docker image "{DOCKER_IMAGE}". Check your internet connection.'
            finish_docker_setup(task, 'failed', message, message)
            return

        emit_docker_setup_status(task, 85, f'Docker image "{DOCKER_IMAGE}" downloaded.', 'success')
    else:
        emit_docker_setup_status(task, 55, f'Docker image "{DOCKER_IMAGE}" already exists locally.', 'success')

    emit_docker_setup_status(task, 90, f'Creating container "{task.container}"...', 'info')
    if docker_create_container(task.container, DOCKER_IMAGE, task):
        finish_docker_setup(task, 'completed', f'Docker container "{task.container}" created and is ready.')
    else:
        message = f'Docker container "{task.container}" could not be created.'
        finish_docker_setup(task, 'failed', message, message)


def run_conversion_pipeline(job):
    """
    Execute the full model conversion pipeline using real Docker commands.

    Pipeline based on convert_command.txt:
      1. Setup: Start container, ensure tpu-mlir is cloned/built, setup workspace
      2. Export ONNX: Download .pt (or use uploaded), run export_to_onnx.py
      3. Model Transform: model_transform → .mlir
      4. Calibration: run_calibration → calib_table
      5. Model Deploy: model_deploy → .cvimodel
      6. Transfer: scp to device (optional)
    """
    try:
        job.status = 'running'
        emit_job_update(job)

        config = job.config
        model_name = config.get('model_name', 'yolov8n')
        input_width = int(config.get('input_width', 640))
        input_height = int(config.get('input_height', 640))
        docker_container = config.get('docker_container', DOCKER_DEFAULT_CONTAINER)
        calibration_count = int(config.get('calibration_count', 100))
        quantize = config.get('quantize', 'int8')
        processor = config.get('processor', 'cv181x')
        device_ip = config.get('device_ip', '192.168.100.2')
        tolerance = config.get('tolerance', '0.85,0.45')
        model_file = config.get('model_file', '')  # filename of uploaded .pt

        output_model_name = f'{model_name}_{input_width}.cvimodel'

        # Working directory inside Docker
        model_dir = f'{DOCKER_WORK_BASE}/model_{model_name}'
        workspace_dir = f'{model_dir}/Workspace'

        # Helper: source envsetup.sh for env vars, and full paths to tpu-mlir tools
        tpu_tools_dir = f'{DOCKER_TPU_MLIR_PATH}/install/python/tools'
        tpu_env_cmd = f'source {DOCKER_TPU_MLIR_PATH}/envsetup.sh'
        cmd_model_transform = f'python3 {tpu_tools_dir}/model_transform.py'
        cmd_run_calibration = f'python3 {tpu_tools_dir}/run_calibration.py'
        cmd_model_deploy = f'python3 {tpu_tools_dir}/model_deploy.py'

        # ================================================================
        # STEP 0: Environment Setup
        # ================================================================
        step_start(job, 0, f'[Step 1/5] Setting up Docker environment ({docker_container})...')

        # Ensure container exists and is running (auto-pull image + auto-create if needed)
        emit_log(job, f'Checking Docker container: {docker_container}...')
        if not docker_ensure_container(docker_container, job):
            step_fail(job, 0, f'Docker container "{docker_container}" could not be set up. Check Docker installation.')
            return

        # Check actual build outputs. envsetup.sh exists immediately after clone
        # and therefore must not be used as evidence of a completed build.
        emit_log(job, 'Checking tpu-mlir installation...')
        success, output = docker_exec(
            docker_container,
            (
                f'test -f {DOCKER_TPU_MLIR_PATH}/envsetup.sh '
                f'&& test -f {DOCKER_TPU_MLIR_PATH}/install/python/tools/model_transform.py '
                f'&& test -x {DOCKER_TPU_MLIR_PATH}/install/bin/tpuc-opt '
                '&& echo "BUILD_READY" || echo "BUILD_REQUIRED"'
            )
        )
        tpu_mlir_ready = 'BUILD_READY' in output

        if not tpu_mlir_ready:
            # Killing a local `docker exec` client does not necessarily stop the
            # command already running in the container. Reuse that build rather
            # than starting a second Ninja process in the same build directory.
            success, build_check = docker_exec(
                docker_container,
                "pgrep -f '[n]inja.*install|[c]make --build.*/workspace/tpu-mlir/build' "
                '>/dev/null && echo "BUILD_RUNNING" || echo "BUILD_IDLE"'
            )
            if 'BUILD_RUNNING' in build_check:
                emit_log(job, 'A tpu-mlir build is already running; waiting for it to finish...', 'warning')
                success, _ = docker_exec(
                    docker_container,
                    (
                        "while pgrep -f '[n]inja.*install|[c]make --build.*/workspace/tpu-mlir/build' "
                        '>/dev/null; do sleep 10; done; '
                        'test -f install/python/tools/model_transform.py '
                        '&& test -x install/bin/tpuc-opt '
                        f'&& touch {TPU_MLIR_BUILD_MARKER}'
                    ),
                    cwd=DOCKER_TPU_MLIR_PATH,
                    job=job,
                    timeout=TPU_MLIR_BUILD_TIMEOUT
                )
                if success:
                    tpu_mlir_ready = True
                    emit_log(job, 'Existing tpu-mlir build completed successfully.', 'success')

        if not tpu_mlir_ready:
            success, repo_check = docker_exec(
                docker_container,
                f'test -d {DOCKER_TPU_MLIR_PATH}/.git && echo "REPO_EXISTS" || echo "REPO_MISSING"'
            )
            if 'REPO_MISSING' in repo_check:
                emit_log(job, 'tpu-mlir repository not found. Cloning it now...', 'warning')
                success, _ = docker_exec(
                    docker_container,
                    f'git clone -b {TPU_MLIR_VERSION} --depth 1 https://github.com/sophgo/tpu-mlir.git {DOCKER_TPU_MLIR_PATH}',
                    cwd='/workspace',
                    job=job,
                    timeout=600
                )
                if not success:
                    step_fail(job, 0, 'Failed to clone tpu-mlir repository.')
                    return
            else:
                emit_log(job, 'Found an incomplete tpu-mlir build; resuming it.', 'warning')

            emit_log(job, f'Building tpu-mlir (timeout: {TPU_MLIR_BUILD_TIMEOUT}s)...')
            success, _ = docker_exec(
                docker_container,
                (
                    'source ./envsetup.sh && ./build.sh '
                    '&& test -f install/python/tools/model_transform.py '
                    '&& test -x install/bin/tpuc-opt '
                    f'&& touch {TPU_MLIR_BUILD_MARKER}'
                ),
                cwd=DOCKER_TPU_MLIR_PATH,
                job=job,
                timeout=TPU_MLIR_BUILD_TIMEOUT
            )
            if not success:
                step_fail(
                    job,
                    0,
                    'Failed to build tpu-mlir. The partial build was kept and will be resumed on the next run.'
                )
                return
        else:
            emit_log(job, 'tpu-mlir is installed.', 'success')

        # Create model workspace directory
        emit_log(job, f'Setting up workspace: {workspace_dir}')
        success, _ = docker_exec(
            docker_container,
            f'mkdir -p {workspace_dir}'
        )

        # Copy dataset and test images if not present
        success, check_out = docker_exec(
            docker_container,
            f'test -d {model_dir}/COCO2017 && echo "DATASET_OK" || echo "DATASET_MISSING"'
        )
        if 'DATASET_MISSING' in check_out:
            emit_log(job, 'Copying COCO2017 dataset and test images...')
            docker_exec(
                docker_container,
                (
                    f'{tpu_env_cmd} && '
                    f'cp -rf ${{REGRESSION_PATH}}/dataset/COCO2017 {model_dir}/ && '
                    f'cp -rf ${{REGRESSION_PATH}}/image {model_dir}/'
                ),
                job=job,
                timeout=300
            )

        step_done(job, 0, f'Environment ready (container: {docker_container})')

        if job.status == 'failed':
            return

        # ================================================================
        # STEP 1: Export to ONNX
        # ================================================================
        step_start(job, 1, f'[Step 2/5] Exporting {model_name} to ONNX ({input_width}x{input_height})...')

        # If user uploaded a .pt file, copy it into the container
        if model_file:
            host_pt_path = safe_path(UPLOAD_FOLDER, model_file, ALLOWED_EXTENSIONS)
            if host_pt_path and host_pt_path.is_file():
                emit_log(job, f'Copying uploaded model {model_file} to container...')
                if not docker_copy_to(docker_container, host_pt_path, f'{workspace_dir}/{model_file}'):
                    step_fail(job, 1, f'Failed to copy {model_file} to container.')
                    return
                pt_filename = model_file
            else:
                step_fail(job, 1, f'Uploaded file not found: {model_file}')
                return
        else:
            # For preset models, ultralytics will auto-download when YOLO(name) is called
            pt_filename = f'{model_name}.pt'
            emit_log(job, f'Model {pt_filename} will be auto-downloaded by ultralytics if not cached.')

        # Create robust export_to_onnx.py instead of downloading an outdated one
        emit_log(job, 'Generating robust export_to_onnx.py script...')
        export_script_code = """from ultralytics import YOLO
import sys

def patch_detect(model):
    detect_layer = model.model[-1]
    
    # Use class replacement instead of MethodType to survive deepcopy in Ultralytics Exporter
    class CustomDetect(type(detect_layer)):
        def forward(self, x):
            cv2 = getattr(self, 'cv2', None)
            cv3 = getattr(self, 'cv3', None)
            
            # Support YOLOv10 architecture
            if cv2 is None or cv3 is None:
                cv2 = getattr(self, 'one2one_cv2', None)
                cv3 = getattr(self, 'one2one_cv3', None)
                
            x_reg = [cv2[i](x[i]) for i in range(self.nl)]
            x_cls = [cv3[i](x[i]) for i in range(self.nl)]
            return x_reg + x_cls

    detect_layer.__class__ = CustomDetect

model_path = sys.argv[1]
input_size = (int(sys.argv[2]), int(sys.argv[3]))

model = YOLO(model_path)
patch_detect(model.model)
# Simplify disabled to prevent graph optimizations from messing up TPU-MLIR
model.export(format='onnx', opset=11, imgsz=input_size, simplify=False)
"""
        import base64
        script_b64 = base64.b64encode(export_script_code.encode('utf-8')).decode('utf-8')
        cmd_write = f"echo '{script_b64}' | base64 -d > {workspace_dir}/export_to_onnx.py"
        success, _ = docker_exec(docker_container, cmd_write, job=job)
        if not success:
            step_fail(job, 1, 'Failed to inject export_to_onnx.py to container.')
            return

        # Ensure real PyTorch + ultralytics are installed
        # tpu-mlir ships a dummy 'torch' package that breaks ultralytics
        emit_log(job, 'Checking PyTorch and ultralytics packages...')
        success, check = docker_exec(
            docker_container,
            'python3 -c "import torch; assert hasattr(torch, \'save\')" 2>/dev/null && echo "TORCH_OK" || echo "TORCH_BAD"'
        )
        if 'TORCH_BAD' in check:
            emit_log(job, 'Detected dummy torch package from tpu-mlir. Installing real PyTorch CPU...', 'warning')
            success, _ = docker_exec(
                docker_container,
                'pip install --force-reinstall torch torchvision --index-url https://download.pytorch.org/whl/cpu',
                job=job,
                timeout=600
            )
            if not success:
                step_fail(job, 1, 'Failed to install PyTorch.')
                return
            emit_log(job, 'PyTorch installed successfully.', 'success')

        success, check = docker_exec(
            docker_container,
            'python3 -c "import ultralytics" 2>/dev/null && echo "INSTALLED" || echo "MISSING"'
        )
        if 'MISSING' in check:
            emit_log(job, 'Installing ultralytics...', 'warning')
            success, _ = docker_exec(
                docker_container,
                'pip install ultralytics',
                job=job,
                timeout=300
            )
            if not success:
                step_fail(job, 1, 'Failed to install ultralytics package.')
                return
            emit_log(job, 'ultralytics installed successfully.', 'success')

        # Ensure numpy < 2 (tpu-mlir's onnxruntime needs numpy 1.x)
        success, check = docker_exec(
            docker_container,
            'python3 -c "import numpy; v=tuple(map(int,numpy.__version__.split(\'.\')[:2])); print(\'OK\' if v[0]<2 else \'BAD\')"'
        )
        if 'BAD' in check:
            emit_log(job, 'Downgrading numpy to 1.x and fixing tqdm (required by tpu-mlir)...', 'warning')
            success, _ = docker_exec(
                docker_container,
                'pip install "numpy<2" --force-reinstall tqdm',
                job=job,
                timeout=120
            )
            if not success:
                step_fail(job, 1, 'Failed to fix numpy/tqdm.')
                return
            emit_log(job, 'numpy & tqdm fixed successfully.', 'success')
        else:
            emit_log(job, 'PyTorch & ultralytics are ready.', 'success')

        # Run ONNX export (do NOT source envsetup.sh here — it overrides torch with tpu-mlir's custom version)
        emit_log(job, f'Running ONNX export: {pt_filename} → {model_name}.onnx')
        success, _ = docker_exec(
            docker_container,
            f'python3 export_to_onnx.py {pt_filename} {input_width} {input_height}',
            cwd=workspace_dir,
            job=job,
            timeout=600
        )
        if not success:
            step_fail(job, 1, 'ONNX export failed.')
            return

        # Verify .onnx was created
        success, check = docker_exec(
            docker_container,
            f'test -f {workspace_dir}/{model_name}.onnx && echo "OK" || echo "FAIL"'
        )
        if 'FAIL' in check:
            step_fail(job, 1, f'{model_name}.onnx was not created. Check model name.')
            return

        step_done(job, 1, f'Exported {model_name}.onnx ({input_width}x{input_height})')

        if job.status == 'failed':
            return

        # ================================================================
        # STEP 2: Model Transform (MLIR)
        # ================================================================
        step_start(job, 2, f'[Step 3/5] Transforming {model_name}.onnx → {model_name}.mlir ...')

        transform_cmd = (
            f'{tpu_env_cmd} && '
            f'{cmd_model_transform} '
            f'--model_name {model_name} '
            f'--model_def {model_name}.onnx '
            f'--input_shapes "[[1,3,{input_height},{input_width}]]" '
            f'--mean "0.0,0.0,0.0" '
            f'--scale "0.0039216,0.0039216,0.0039216" '
            f'--keep_aspect_ratio '
            f'--pixel_format rgb '
            f'--test_input ../image/dog.jpg '
            f'--test_result {model_name}_top_outputs.npz '
            f'--mlir {model_name}.mlir'
        )

        success, _ = docker_exec(
            docker_container,
            transform_cmd,
            cwd=workspace_dir,
            job=job,
            timeout=1200
        )
        if not success:
            step_fail(job, 2, 'Model transform failed.')
            return

        # Verify .mlir was created
        success, check = docker_exec(
            docker_container,
            f'test -f {workspace_dir}/{model_name}.mlir && echo "OK" || echo "FAIL"'
        )
        if 'FAIL' in check:
            step_fail(job, 2, f'{model_name}.mlir was not created.')
            return

        step_done(job, 2, f'Generated {model_name}.mlir')

        if job.status == 'failed':
            return

        # ================================================================
        # STEP 3: Calibration
        # ================================================================
        effective_calibration_count = min(calibration_count, TPU_MLIR_SAFE_CALIBRATION_IMAGES)
        step_start(job, 3, f'[Step 4/5] Running calibration ({effective_calibration_count} images)...')
        if effective_calibration_count != calibration_count:
            emit_log(
                job,
                f'Reduced calibration set from {calibration_count} to {effective_calibration_count} images '
                'to avoid a known TPU-MLIR v1.7 native collector crash.',
                'warning'
            )

        calib_cmd = (
            f'{tpu_env_cmd} && '
            f'{cmd_run_calibration} '
            f'{model_name}.mlir '
            f'--dataset ../COCO2017 '
            f'--input_num {effective_calibration_count} '
            f'--tune_num 0 '
            f'-o {model_name}_calib_table'
        )

        success, _ = docker_exec(
            docker_container,
            calib_cmd,
            cwd=workspace_dir,
            job=job,
            timeout=3600  # Calibration can take a long time
        )
        if not success:
            step_fail(job, 3, 'Calibration failed.')
            return

        success, check = docker_exec(
            docker_container,
            f'test -s {workspace_dir}/{model_name}_calib_table && echo "OK" || echo "FAIL"'
        )
        if 'FAIL' in check:
            step_fail(job, 3, 'Calibration command finished without producing a valid table.')
            return

        # Verify calibration table
        success, check = docker_exec(
            docker_container,
            f'test -f {workspace_dir}/{model_name}_calib_table && echo "OK" || echo "FAIL"'
        )
        if 'FAIL' in check:
            step_fail(job, 3, f'{model_name}_calib_table was not created.')
            return

        step_done(job, 3, f'Calibration complete ({effective_calibration_count} images)')

        if job.status == 'failed':
            return

        # ================================================================
        # STEP 4: Model Deploy (Quantization)
        # ================================================================
        step_start(job, 4, f'[Step 5/5] Quantizing model ({quantize.upper()}, {processor})...')

        deploy_cmd = (
            f'{tpu_env_cmd} && '
            f'{cmd_model_deploy} '
            f'--mlir {model_name}.mlir '
            f'--quant_input '
            f'--quant_output '
            f'--quantize {quantize} '
            f'--calibration_table {model_name}_calib_table '
            f'--processor {processor} '
            f'--test_input ../image/dog.jpg '
            f'--test_reference {model_name}_top_outputs.npz '
            f'--tolerance {tolerance} '
            f'--model {output_model_name}'
        )

        success, _ = docker_exec(
            docker_container,
            deploy_cmd,
            cwd=workspace_dir,
            job=job,
            timeout=3600
        )
        if not success:
            step_fail(job, 4, 'Model deploy failed.')
            return

        # Verify .cvimodel was created
        success, check = docker_exec(
            docker_container,
            f'test -f {workspace_dir}/{output_model_name} && echo "OK" || echo "FAIL"'
        )
        if 'FAIL' in check:
            step_fail(job, 4, f'{output_model_name} was not created.')
            return

        # Copy .cvimodel from container to host outputs folder
        host_output_path = OUTPUT_FOLDER / f'{job.job_id}_{output_model_name}'
        emit_log(job, f'Copying {output_model_name} to host...')
        if docker_copy_from(docker_container, f'{workspace_dir}/{output_model_name}', host_output_path):
            job.output_model = host_output_path
            emit_log(job, f'Output saved: {host_output_path}', 'success')
        else:
            emit_log(job, 'Warning: Could not copy model to host. Model is still in container.', 'warning')

        step_done(job, 4, f'Generated {output_model_name} ({quantize.upper()}, {processor})')

        if job.status == 'failed':
            return

        emit_job_update(job)

        # ================================================================
        # COMPLETE
        # ================================================================
        job.status = 'completed'
        emit_log(job, f'🎉 Pipeline completed! Output: {output_model_name}', 'success')
        if job.output_model:
            emit_log(job, f'📥 Download your model: /api/jobs/{job.job_id}/download', 'success')
        emit_job_update(job)

    except Exception as e:
        job.status = 'failed'
        if 0 <= job.current_step < len(job.steps):
            job.steps[job.current_step]['status'] = 'failed'
            job.steps[job.current_step]['log'] = str(e)
        emit_log(job, f'❌ Pipeline failed: {str(e)}', 'error')
        emit_job_update(job)
    finally:
        # Cleanup process tracking
        if job.job_id in running_processes:
            del running_processes[job.job_id]


# ============================================================
# Routes
# ============================================================
@app.route('/')
def index():
    return send_from_directory('static', 'index.html')


@app.route('/app')
def app_page():
    return send_from_directory('static', 'app.html')


@app.route('/api/health', methods=['GET'])
def health():
    return jsonify({'status': 'ok', 'version': '1.0.0', 'name': 'LaiLab Nano V1'})


# ---- Docker Status ----
@app.route('/api/docker/status', methods=['GET'])
def docker_status():
    """Check Docker availability and list containers."""
    try:
        # Check if Docker daemon is accessible
        result = subprocess.run(
            ['docker', 'info'], capture_output=True, text=True, timeout=10
        )
        docker_available = result.returncode == 0

        # List containers
        containers = []
        if docker_available:
            result = subprocess.run(
                ['docker', 'ps', '-a', '--format', '{{.Names}}\t{{.Status}}\t{{.Image}}'],
                capture_output=True, text=True, timeout=10
            )
            if result.returncode == 0:
                for line in result.stdout.strip().split('\n'):
                    if line.strip():
                        parts = line.split('\t')
                        if len(parts) >= 3:
                            name, status, image = parts[0], parts[1], parts[2]
                            containers.append({
                                'name': name,
                                'status': status,
                                'image': image,
                                'running': 'Up' in status
                            })

        return jsonify({
            'docker_available': docker_available,
            'containers': containers
        })
    except FileNotFoundError:
        return jsonify({
            'docker_available': False,
            'containers': [],
            'error': 'Docker is not installed'
        })
    except Exception as e:
        return jsonify({
            'docker_available': False,
            'containers': [],
            'error': str(e)
        })


@app.route('/api/docker/setup', methods=['POST'])
def docker_setup():
    """Start a background task that pulls the required Docker image and creates/starts a container."""
    data = request.get_json(silent=True) or {}
    container = (data.get('container') or DOCKER_DEFAULT_CONTAINER).strip()

    if not valid_docker_container_name(container):
        return jsonify({
            'error': 'Invalid Docker container name. Use letters, numbers, dots, underscores, or hyphens.'
        }), 400

    task_id = f'docker-setup-{str(uuid.uuid4())[:8]}'
    task = DockerSetupTask(task_id, container)
    docker_setup_tasks[task_id] = task

    thread = threading.Thread(target=run_docker_setup, args=(task,), daemon=True)
    thread.start()

    return jsonify({
        'task_id': task_id,
        'status': task.status,
        'container': container,
        'image': DOCKER_IMAGE,
        'progress': task.progress,
        'message': task.message,
    }), 202


@app.route('/api/docker/setup/<task_id>', methods=['GET'])
def docker_setup_status(task_id):
    """Return the latest state for a Docker setup task."""
    task = docker_setup_tasks.get(task_id)
    if not task:
        return jsonify({'error': 'Docker setup task not found'}), 404
    return jsonify(task.to_dict())


# ---- File Upload ----
@app.route('/api/upload', methods=['POST'])
def upload_file():
    """Upload a .pt model file."""
    if 'file' not in request.files:
        return jsonify({'error': 'No file provided'}), 400

    file = request.files['file']
    if file.filename == '':
        return jsonify({'error': 'No file selected'}), 400

    if file and allowed_file(file.filename):
        filename = safe_filename(file.filename, ALLOWED_EXTENSIONS)
        filepath = safe_path(UPLOAD_FOLDER, filename, ALLOWED_EXTENSIONS)
        if filepath.exists():
            return api_error('A file with this name already exists. Rename it before uploading.', 409)
        file.save(filepath)
        return jsonify({
            'filename': filename,
            'size': filepath.stat().st_size,
        })
    else:
        return jsonify({'error': 'Only .pt files are allowed'}), 400


# ---- Jobs ----
@app.route('/api/jobs', methods=['GET'])
def list_jobs():
    with jobs_lock:
        return jsonify([j.to_dict() for j in jobs.values()])


@app.route('/api/jobs', methods=['POST'])
def create_job():
    try:
        config = validate_conversion_config(request_data())
    except ValueError as exc:
        return api_error(str(exc))
    with jobs_lock:
        if any(job.status in {'pending', 'running'} for job in jobs.values()):
            return api_error('A conversion job is already running. Wait for it to finish or cancel it first.', 409)
        job_id = str(uuid.uuid4())[:8]
        job = ConversionJob(job_id, config)
        jobs[job_id] = job

    # Start pipeline in background thread
    thread = threading.Thread(target=run_conversion_pipeline, args=(job,), daemon=True)
    thread.start()

    return jsonify(job.to_dict()), 201


@app.route('/api/jobs/<job_id>', methods=['GET'])
def get_job(job_id):
    with jobs_lock:
        job = jobs.get(job_id)
    if not job:
        return jsonify({'error': 'Job not found'}), 404
    return jsonify(job.to_dict())


@app.route('/api/jobs/<job_id>/cancel', methods=['POST'])
def cancel_job(job_id):
    with jobs_lock:
        job = jobs.get(job_id)
    if not job:
        return jsonify({'error': 'Job not found'}), 404

    job.status = 'cancelled'
    # Kill running subprocess
    proc = running_processes.get(job_id)
    if proc:
        try:
            proc.kill()
        except Exception:
            pass
    emit_log(job, '⚠ Job cancelled by user', 'warning')
    emit_job_update(job)
    return jsonify({'status': 'cancelled'})


@app.route('/api/jobs/<job_id>/download', methods=['GET'])
def download_model(job_id):
    """Download the output .cvimodel file."""
    job = jobs.get(job_id)
    if not job:
        return jsonify({'error': 'Job not found'}), 404
    output_path = Path(job.output_model) if job.output_model else None
    if not output_path or not output_path.is_file():
        return jsonify({'error': 'Output model not available'}), 404

    # Strip job_id prefix from filename for cleaner download name
    raw_name = output_path.name
    if raw_name.startswith(job_id + '_'):
        clean_name = raw_name[len(job_id) + 1:]
    else:
        clean_name = raw_name

    return send_from_directory(OUTPUT_FOLDER, raw_name, as_attachment=True, download_name=clean_name, max_age=0)


# ---- Direct file download (works after server restart) ----
@app.route('/api/models/<filename>/download', methods=['GET'])
def download_model_file(filename):
    """Download a .cvimodel file directly from the outputs folder."""
    # Security: prevent path traversal
    safe_name = safe_filename(filename, ALLOWED_MODEL_EXTENSIONS)
    fpath = safe_path(OUTPUT_FOLDER, safe_name, ALLOWED_MODEL_EXTENSIONS) if safe_name else None
    if not fpath or not fpath.is_file():
        return jsonify({'error': 'File not found'}), 404

    # Strip job_id prefix for clean download name
    parts = safe_name.split('_', 1)
    clean_name = parts[1] if len(parts) > 1 else safe_name

    return send_from_directory(OUTPUT_FOLDER, safe_name, as_attachment=True, download_name=clean_name, max_age=0)


# ---- Available Models ----
@app.route('/api/models', methods=['GET'])
def list_models():
    """List available .cvimodel files from outputs folder."""
    models = []
    if os.path.exists(OUTPUT_FOLDER):
        for f in os.listdir(OUTPUT_FOLDER):
            if f.endswith('.cvimodel'):
                fpath = safe_path(OUTPUT_FOLDER, f, ALLOWED_MODEL_EXTENSIONS)
                if not fpath:
                    continue
                # Strip job_id prefix for display name
                parts = f.split('_', 1)
                display_name = parts[1] if len(parts) > 1 else f
                models.append({
                    'filename': f,
                    'display_name': display_name,
                    'size': fpath.stat().st_size,
                    'modified': fpath.stat().st_mtime,
                })
    # Sort by modification time, newest first
    models.sort(key=lambda x: x['modified'], reverse=True)
    return jsonify({'models': models})


# ---- Presets ----
@app.route('/api/config/presets', methods=['GET'])
def get_presets():
    presets = [
        {
            'id': 'yolo11n_320',
            'name': 'YOLO11 Nano (320x320)',
            'description_en': 'YOLO11n 320x320 - Optimized for edge (Default)',
            'description_vi': 'YOLO11n 320x320 - Tối ưu nhất cho thiết bị biên (Mặc định)',
            'config': {
                'model_name': 'yolo11n',
                'input_width': 320,
                'input_height': 320,
                'calibration_count': 100,
                'quantize': 'int8',
                'processor': 'cv181x',
                'tolerance': '0.85,0.45',
            }
        },
        {
            'id': 'yolov8n_320',
            'name': 'YOLOv8 Nano (320x320)',
            'description_en': 'YOLOv8n with 320x320 input - Fastest inference',
            'description_vi': 'YOLOv8n 320x320 - Tốc độ suy luận nhanh nhất',
            'config': {
                'model_name': 'yolov8n',
                'input_width': 320,
                'input_height': 320,
                'calibration_count': 100,
                'quantize': 'int8',
                'processor': 'cv181x',
                'tolerance': '0.85,0.45',
            }
        },
        {
            'id': 'yolo11n_640',
            'name': 'YOLO11 Nano (640x640)',
            'description_en': 'Latest YOLO11n with 640x640 input',
            'description_vi': 'YOLO11n mới nhất với đầu vào 640x640',
            'config': {
                'model_name': 'yolo11n',
                'input_width': 640,
                'input_height': 640,
                'calibration_count': 100,
                'quantize': 'int8',
                'processor': 'cv181x',
                'tolerance': '0.85,0.45',
            }
        },
        {
            'id': 'yolov8n_640',
            'name': 'YOLOv8 Nano (640x640)',
            'description_en': 'Standard YOLOv8n with 640x640 input - Best accuracy',
            'description_vi': 'YOLOv8n tiêu chuẩn 640x640 - Độ chính xác tốt nhất',
            'config': {
                'model_name': 'yolov8n',
                'input_width': 640,
                'input_height': 640,
                'calibration_count': 100,
                'quantize': 'int8',
                'processor': 'cv181x',
                'tolerance': '0.85,0.45',
            }
        },
    ]
    return jsonify(presets)


# ============================================================
# Deploy API and Inference Logic
# ============================================================
inference_thread = None
inference_running = False


def validate_deploy_request(data):
    if not isinstance(data, dict):
        raise ValueError('A JSON object is required.')
    ip = valid_ipv4(data.get('ip', '192.168.100.2'))
    if not ip:
        raise ValueError('A valid IPv4 device address is required.')
    model_filename = safe_filename(data.get('model_filename'), ALLOWED_MODEL_EXTENSIONS)
    model_path = safe_path(OUTPUT_FOLDER, model_filename, ALLOWED_MODEL_EXTENSIONS) if model_filename else None
    if not model_path or not model_path.is_file():
        raise ValueError('The selected .cvimodel file was not found.')
    uart_dev = str(data.get('uartDev', '/dev/ttyS0')).strip()
    if uart_dev and not re.fullmatch(r'/dev/ttyS[0-9]+', uart_dev):
        raise ValueError('uartDev must be a /dev/ttyS<number> device.')
    user = str(data.get('user', 'root')).strip()
    if not re.fullmatch(r'[a-z_][a-z0-9_-]{0,31}', user):
        raise ValueError('Invalid SSH username.')
    password = data.get('password', 'root')
    if not isinstance(password, str) or len(password) > 256:
        raise ValueError('Invalid SSH password.')
    inference_mode = str(data.get('inferenceMode', 'continuous')).strip().lower()
    if inference_mode not in {'continuous', 'trigger', 'all'}:
        raise ValueError('inferenceMode must be continuous, trigger, or all.')
    trigger_source = str(data.get('triggerSource', 'ethernet')).strip().lower()
    if trigger_source not in {'uart', 'ethernet', 'gpio'}:
        raise ValueError('triggerSource must be uart, ethernet, or gpio.')
    trigger_edge = str(data.get('triggerEdge', 'rising')).strip().lower()
    if trigger_edge not in {'rising', 'falling', 'both'}:
        raise ValueError('triggerEdge must be rising, falling, or both.')
    output_transport = str(data.get('outputTransport', 'ethernet')).strip().lower()
    if output_transport not in {'none', 'uart', 'ethernet', 'both'}:
        raise ValueError('outputTransport must be none, uart, ethernet, or both.')
    stream_output = data.get('streamOutput', True)
    if not isinstance(stream_output, bool):
        raise ValueError('streamOutput must be a boolean.')
    no_yolo = data.get('noYolo', False)
    if not isinstance(no_yolo, bool):
        raise ValueError('noYolo must be a boolean.')
    if no_yolo and inference_mode != 'continuous':
        raise ValueError('Trigger and all modes require YOLO inference to be enabled.')
    camera_exposure = data.get('cameraExposure', 'auto')
    if camera_exposure != 'auto':
        camera_exposure = bounded_int(camera_exposure, 'cameraExposure', 0, 100000)
    if (trigger_source == 'uart' or output_transport in {'uart', 'both'}) and not uart_dev:
        raise ValueError('A UART device is required for the selected trigger/output transport.')
    return {
        'ip': ip,
        'model_filename': model_filename,
        'model_path': model_path,
        'streamWidth': bounded_int(data.get('streamWidth', 640), 'streamWidth', 32, 1920),
        'streamHeight': bounded_int(data.get('streamHeight', 480), 'streamHeight', 32, 1080),
        'yoloW': bounded_int(data.get('yoloW', 320), 'yoloW', 32, 1280),
        'yoloH': bounded_int(data.get('yoloH', 320), 'yoloH', 32, 1280),
        'camWidth': bounded_int(data.get('camWidth', 640), 'camWidth', 32, 1920),
        'camHeight': bounded_int(data.get('camHeight', 480), 'camHeight', 32, 1080),
        'confThresh': bounded_float(data.get('confThresh', 0.5), 'confThresh', 0, 1),
        'nmsThresh': bounded_float(data.get('nmsThresh', 0.5), 'nmsThresh', 0, 1),
        'noYolo': no_yolo,
        'jpegQuality': bounded_int(data.get('jpegQuality', 70), 'jpegQuality', 1, 100),
        'cameraExposure': camera_exposure,
        'uartDev': uart_dev,
        'baudRate': bounded_int(data.get('baudRate', 115200), 'baudRate', 1200, 1000000),
        'password': password,
        'user': user,
        'inferenceMode': inference_mode,
        'streamOutput': stream_output,
        'triggerSource': trigger_source,
        'triggerPort': bounded_int(data.get('triggerPort', 8082), 'triggerPort', 1024, 65535),
        'triggerGpio': bounded_int(data.get('triggerGpio', 502), 'triggerGpio', 0, 1024),
        'triggerEdge': trigger_edge,
        'outputTransport': output_transport,
        'metadataPort': bounded_int(data.get('metadataPort', 8081), 'metadataPort', 1024, 65535),
    }

def udp_listener(ip):
    global inference_running
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    # Allows address reuse
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    try:
        sock.bind(('', 8081))
    except Exception as e:
        print("Could not bind UDP 8081:", e)
        return
    sock.settimeout(1.0)
    
    while inference_running:
        try:
            data, addr = sock.recvfrom(4096)
            if addr[0] == ip:
                parsed = json.loads(data.decode('utf-8', 'ignore'))
                socketio.emit('inference_meta', parsed, namespace='/')
        except socket.timeout:
            continue
        except Exception as e:
            pass
    sock.close()


@app.route('/api/device/ping', methods=['POST'])
def ping_device():
    """Ping a device to check if it's reachable."""
    import platform
    try:
        ip = valid_ipv4(request_data().get('ip', ''))
    except ValueError as exc:
        return api_error(str(exc))
    if not ip:
        return jsonify({'error': 'A valid IPv4 address is required.', 'reachable': False}), 400

    try:
        # Platform-specific ping command
        param = '-n' if platform.system().lower() == 'windows' else '-c'
        cmd = ['ping', param, '2', '-w', '2000', ip]

        result = subprocess.run(
            cmd,
            capture_output=True,
            text=True,
            timeout=10
        )

        reachable = result.returncode == 0

        # Parse average time from ping output
        avg_ms = '?'
        if reachable:
            # Windows: Average = 0ms
            avg_match = re.search(r'Average\s*=\s*(\d+)ms', result.stdout)
            if avg_match:
                avg_ms = avg_match.group(1)
            else:
                # Linux: rtt min/avg/max/mdev = 0.123/0.456/0.789/0.012 ms
                avg_match = re.search(r'rtt\s+.*?=\s*[\d.]+/([\d.]+)/', result.stdout)
                if avg_match:
                    avg_ms = avg_match.group(1)
                else:
                    # try time<1ms pattern (Windows)
                    if 'time<1ms' in result.stdout or 'time=0ms' in result.stdout:
                        avg_ms = '<1'

        return jsonify({
            'reachable': reachable,
            'ip': ip,
            'avg_ms': avg_ms,
            'output': result.stdout[-300:] if result.stdout else ''
        })

    except subprocess.TimeoutExpired:
        return jsonify({'reachable': False, 'ip': ip, 'avg_ms': '?', 'error': 'Ping timed out'})
    except Exception as e:
        return jsonify({'reachable': False, 'ip': ip, 'avg_ms': '?', 'error': str(e)})


@app.route('/api/deploy', methods=['POST'])
def deploy_to_device():
    try:
        req = validate_deploy_request(request_data())
    except ValueError as exc:
        return api_error(str(exc))
    ip = req['ip']
    model_filename = req['model_filename']
    model_path = req['model_path']
    streamWidth, streamHeight = req['streamWidth'], req['streamHeight']
    yoloW, yoloH = req['yoloW'], req['yoloH']
    camWidth, camHeight = req['camWidth'], req['camHeight']
    confThresh, nmsThresh = req['confThresh'], req['nmsThresh']
    noYolo, jpegQuality = req['noYolo'], req['jpegQuality']
    cameraExposure = req['cameraExposure']
    uartDev, baudRate, password, user = req['uartDev'], req['baudRate'], req['password'], req['user']
    inferenceMode, streamOutput = req['inferenceMode'], req['streamOutput']
    triggerSource, triggerPort = req['triggerSource'], req['triggerPort']
    triggerGpio, triggerEdge = req['triggerGpio'], req['triggerEdge']
    outputTransport, metadataPort = req['outputTransport'], req['metadataPort']
    if not deployment_lock.acquire(blocking=False):
        return api_error('A deployment is already in progress.', 409)

    def emit_deploy_log(message, level='info'):
        entry = {'time': time.time(), 'message': message, 'level': level}
        socketio.emit('deploy_log', entry, namespace='/')

    steps = [
        {'name': 'Upload Model', 'status': 'pending', 'detail': ''},
        {'name': 'Upload Binary', 'status': 'pending', 'detail': ''},
        {'name': 'Upload Boot Script', 'status': 'pending', 'detail': ''},
        {'name': 'Set Permissions & Autostart', 'status': 'pending', 'detail': ''},
        {'name': 'Reboot Device', 'status': 'pending', 'detail': ''},
    ]

    def emit_steps():
        socketio.emit('deploy_steps', {'steps': steps}, namespace='/')

    def set_step(idx, status, detail=''):
        steps[idx]['status'] = status
        if detail:
            steps[idx]['detail'] = detail
        if status == 'failed':
            for j in range(idx + 1, len(steps)):
                steps[j]['status'] = 'skipped'
        emit_steps()

    emit_steps()

    def deploy_worker():
        import paramiko

        emit_deploy_log(f'Starting deployment to {user}@{ip}...')

        try:
            emit_deploy_log(f'Connecting to {ip}...')
            client = build_ssh_client()
            ckw = {'hostname': ip, 'port': 22, 'username': user, 'timeout': 10}
            if password != '':
                ckw['password'] = password
            else:
                # Try empty password and fallback to keys
                ckw['password'] = ''
                ckw['allow_agent'] = True
                ckw['look_for_keys'] = True
            client.connect(**ckw)
            emit_deploy_log(f'Connected to {user}@{ip}', 'success')

            sftp = client.open_sftp()

            def ssh_exec(cmd):
                emit_deploy_log(f'$ {cmd}')
                si, so, se = client.exec_command(cmd, timeout=30)
                out = so.read().decode('utf-8', errors='replace').strip()
                err = se.read().decode('utf-8', errors='replace').strip()
                ec = so.channel.recv_exit_status()
                if out:
                    emit_deploy_log(out)
                if err:
                    emit_deploy_log(err, 'warning' if ec == 0 else 'error')
                return ec

            remote_model_name = model_filename
            parts = model_filename.split('_', 1)
            if len(parts) > 1:
                remote_model_name = parts[1]

            # Stop running app to prevent ETXTBSY upload failure
            emit_deploy_log('Stopping any running inference processes...')
            try:
                ssh_exec('/etc/init.d/S99yolocam stop')
                ssh_exec('killall -9 Yolo_CSIStream')
                # Disable the legacy service that otherwise races this runtime
                # for the single CVI VB/VPSS instance after reboot.
                ssh_exec('if [ -f /etc/init.d/S99yolo_camera ]; then mv /etc/init.d/S99yolo_camera /root/S99yolo_camera.disabled; fi')
            except Exception:
                pass

            # Step 0: Upload Model
            set_step(0, 'running')
            sz = os.path.getsize(model_path) / (1024 * 1024)
            emit_deploy_log(f'Uploading {remote_model_name} ({sz:.1f} MB)...')
            sftp.put(str(model_path), f'/root/{remote_model_name}')
            emit_deploy_log(f'Model uploaded to /root/{remote_model_name}', 'success')
            set_step(0, 'completed', f'{remote_model_name} ({sz:.1f} MB)')

            # Step 1: Upload Binary
            set_step(1, 'running')
            bin_path = BASE_DIR / 'develop' / 'Projects' / 'OTGCamera' / 'build' / 'Yolo_CSIStream'
            if not bin_path.is_file():
                bin_path = BASE_DIR / 'deploy' / 'Yolo_CSIStream'
            if os.path.exists(bin_path):
                bsz = os.path.getsize(bin_path) / (1024 * 1024)
                emit_deploy_log(f'Uploading Yolo_CSIStream ({bsz:.1f} MB)...')
                sftp.put(bin_path, '/root/Yolo_CSIStream')
                emit_deploy_log('Binary uploaded to /root/Yolo_CSIStream', 'success')
                set_step(1, 'completed', f'Yolo_CSIStream ({bsz:.1f} MB)')
            else:
                emit_deploy_log('Yolo_CSIStream not found in deploy/, skipping', 'warning')
                steps[1]['status'] = 'skipped'
                steps[1]['detail'] = 'Not found'
                emit_steps()

            # Step 2: Upload Boot Script
            set_step(2, 'running')
            emit_deploy_log('Creating boot script S99yolocam...')
            # Build ARGS dynamically
            args_list = []
            args_list.append(f"--cam {camWidth}x{camHeight}")
            args_list.append(f"--stream {streamWidth}x{streamHeight}")
            args_list.append(f"--yolo {yoloW}")
            args_list.append(f"--quality {jpegQuality}")
            args_list.append(f"--conf {confThresh}")
            args_list.append(f"--nms {nmsThresh}")
            args_list.append(f"--exposure {cameraExposure}")
            args_list.append(f"--mode {inferenceMode}")
            args_list.append(f"--output {outputTransport}")
            args_list.append(f"--metadata-port {metadataPort}")
            if not streamOutput:
                args_list.append("--no-stream")
            if inferenceMode in {'trigger', 'all'}:
                args_list.append(f"--trigger-source {triggerSource}")
                if triggerSource == 'ethernet':
                    args_list.append(f"--trigger-port {triggerPort}")
                elif triggerSource == 'gpio':
                    args_list.append(f"--trigger-gpio {triggerGpio}")
                    args_list.append(f"--trigger-edge {triggerEdge}")
            if noYolo:
                args_list.append("--no-yolo")
            if uartDev and (triggerSource == 'uart' or outputTransport in {'uart', 'both'}):
                args_list.append(f"--uart {uartDev}")
                args_list.append(f"--baud {baudRate}")
            
            args_str = " ".join(args_list)

            script = f"""#!/bin/sh
# Auto-generated by LaiLab Nano V1
APP_BIN="/root/Yolo_CSIStream"
MODEL="/root/{remote_model_name}"
ARGS="{args_str}"
LOG_FILE="/root/yolo.log"
WATCHDOG_PID="/var/run/yolocam-watchdog.pid"

# Setup library paths for CV180xB SDK & OpenCV
export LD_LIBRARY_PATH=/root/libs_patch/lib:/root/libs_patch/middleware_v2:/root/libs_patch/middleware_v2_3rd:/root/libs_patch/opencv:/root/libs_patch/tpu_sdk_libs:/root/libs_patch:$LD_LIBRARY_PATH
[ -f /root/board_setup.sh ] && source /root/board_setup.sh

wait_for_stable_video() {{
    last_nodes=""
    stable_count=0
    attempt=0
    while [ "$attempt" -lt 20 ]; do
        nodes="$(ls /dev/video* 2>/dev/null | tr '\n' ' ')"
        if [ -n "$nodes" ] && [ "$nodes" = "$last_nodes" ]; then
            stable_count=$((stable_count + 1))
        else
            stable_count=0
        fi
        [ "$stable_count" -ge 2 ] && return 0
        last_nodes="$nodes"
        attempt=$((attempt + 1))
        sleep 1
    done
    return 1
}}

case "$1" in
  start)
    if [ -f "$APP_BIN" ] && [ -f "$MODEL" ]; then
        if [ -f "$WATCHDOG_PID" ] && kill -0 "$(cat "$WATCHDOG_PID")" 2>/dev/null; then
            exit 0
        fi
        : > "$LOG_FILE"
        (
            while true; do
                if wait_for_stable_video; then
                    echo "[Watchdog] Starting camera runtime" >> "$LOG_FILE"
                    cd /root/
                    $APP_BIN $MODEL $ARGS >> "$LOG_FILE" 2>&1
                    rc=$?
                    echo "[Watchdog] Runtime stopped rc=$rc; waiting for camera" >> "$LOG_FILE"
                fi
                sleep 2
            done
        ) </dev/null >> "$LOG_FILE" 2>&1 &
        echo $! > "$WATCHDOG_PID"
    fi
    ;;
  stop)
    if [ -f "$WATCHDOG_PID" ]; then
        kill "$(cat "$WATCHDOG_PID")" 2>/dev/null
        rm -f "$WATCHDOG_PID"
    fi
    killall Yolo_CSIStream 2>/dev/null
    ;;
  restart|reload)
    $0 stop
    sleep 1
    $0 start
    ;;
  *)
    exit 1
esac
exit 0
"""
            sftp.putfo(io.BytesIO(script.encode('utf-8')), '/etc/init.d/S99yolocam')
            emit_deploy_log('Boot script uploaded to /etc/init.d/S99yolocam', 'success')
            set_step(2, 'completed', 'S99yolocam')

            sftp.close()

            # Step 3: Set permissions & autostart
            set_step(3, 'running')
            emit_deploy_log('Setting permissions and enabling autostart...')
            ssh_exec('chmod +x /root/Yolo_CSIStream /etc/init.d/S99yolocam')
            emit_deploy_log('Autostart service configured', 'success')
            set_step(3, 'completed', 'Autostart enabled')

            # Step 4: Reboot device
            set_step(4, 'running')
            emit_deploy_log('Rebooting device...')
            try:
                client.exec_command('nohup sh -c "sleep 1 && reboot" &', timeout=5)
            except Exception:
                pass
            emit_deploy_log('Reboot command sent. Device will restart shortly.', 'success')
            set_step(4, 'completed', 'Reboot initiated')

            try:
                client.close()
            except Exception:
                pass

            emit_deploy_log('Deployment completed! Device is rebooting.', 'success')
            socketio.emit('deploy_complete', {'status': 'success'}, namespace='/')

        except Exception as e:
            msg = str(e)
            emit_deploy_log(f'Deployment failed: {msg}', 'error')
            for i, s in enumerate(steps):
                if s['status'] == 'running':
                    set_step(i, 'failed', msg)
                    break
            socketio.emit('deploy_complete', {'status': 'failed', 'error': msg}, namespace='/')
        finally:
            deployment_lock.release()

    threading.Thread(target=deploy_worker, daemon=True).start()
    return jsonify({'status': 'deploying'})

# ============================================================
# Serial / UART Logic
# ============================================================
serial_port_obj = None
serial_thread = None
serial_running = False

def serial_worker():
    global serial_port_obj, serial_running
    while serial_running and serial_port_obj and serial_port_obj.is_open:
        try:
            if serial_port_obj.in_waiting > 0:
                data = serial_port_obj.read(serial_port_obj.in_waiting)
                try:
                    text_data = data.decode('utf-8')
                except UnicodeDecodeError:
                    text_data = data.decode('ascii', 'ignore')
                socketio.emit('serial_data', {'text': text_data}, namespace='/')
                time.sleep(0.01)
            else:
                time.sleep(0.05)
        except serial.SerialException as e:
            socketio.emit('serial_error', {'error': str(e)}, namespace='/')
            serial_running = False
            if serial_port_obj:
                serial_port_obj.close()
                serial_port_obj = None
            break
        except Exception:
            time.sleep(0.05)

@app.route('/api/serial/ports', methods=['GET'])
def get_serial_ports():
    ports = serial.tools.list_ports.comports()
    port_list = [{'port': p.device, 'desc': p.description} for p in ports]
    return jsonify(port_list)

@app.route('/api/serial/connect', methods=['POST'])
def connect_serial():
    global serial_port_obj, serial_running, serial_thread
    try:
        req = request_data()
        port = str(req.get('port', ''))
        baudrate = bounded_int(req.get('baudrate', 115200), 'baudrate', 1200, 1000000)
    except ValueError as exc:
        return api_error(str(exc))
    available_ports = {p.device for p in serial.tools.list_ports.comports()}
    if port not in available_ports:
        return api_error('Selected serial port is not available.')
    
    if serial_port_obj and serial_port_obj.is_open:
        serial_running = False
        serial_port_obj.close()
        
    try:
        serial_port_obj = serial.Serial(port, baudrate, timeout=1)
        serial_running = True
        serial_thread = threading.Thread(target=serial_worker, daemon=True)
        serial_thread.start()
        return jsonify({'status': 'connected'})
    except Exception as e:
        serial_port_obj = None
        serial_running = False
        return jsonify({'error': str(e)}), 400

@app.route('/api/serial/disconnect', methods=['POST'])
def disconnect_serial():
    global serial_port_obj, serial_running
    serial_running = False
    if serial_port_obj and serial_port_obj.is_open:
        serial_port_obj.close()
        serial_port_obj = None
    return jsonify({'status': 'disconnected'})

@app.route('/api/serial/write', methods=['POST'])
def write_serial():
    global serial_port_obj
    try:
        text = request_data().get('text', '')
    except ValueError as exc:
        return api_error(str(exc))
    if not isinstance(text, str) or len(text) > 4096:
        return api_error('Serial message must be text no longer than 4096 characters.')
    if serial_port_obj and serial_port_obj.is_open:
        try:
            serial_port_obj.write((text + '\r\n').encode('utf-8'))
            return jsonify({'status': 'ok'})
        except Exception as e:
            return jsonify({'error': str(e)}), 400
    return jsonify({'error': 'Not connected'}), 400

# ============================================================
# SSH Terminal Logic
# ============================================================
ssh_client = None
ssh_channel = None
ssh_running = False
ssh_read_thread = None

def ssh_reader():
    global ssh_running, ssh_channel
    while ssh_running and ssh_channel:
        try:
            if ssh_channel.recv_ready():
                data = ssh_channel.recv(4096)
                if data:
                    text = data.decode('utf-8', errors='replace')
                    socketio.emit('ssh_data', {'text': text}, namespace='/')
                else:
                    break
            elif ssh_channel.recv_stderr_ready():
                data = ssh_channel.recv_stderr(4096)
                if data:
                    text = data.decode('utf-8', errors='replace')
                    socketio.emit('ssh_data', {'text': text, 'error': True}, namespace='/')
            else:
                time.sleep(0.05)
        except Exception as e:
            socketio.emit('ssh_data', {'text': f'\r\n[Connection lost: {e}]\r\n', 'error': True}, namespace='/')
            break
    ssh_running = False

@app.route('/api/ssh/connect', methods=['POST'])
def ssh_connect():
    global ssh_client, ssh_channel, ssh_running, ssh_read_thread
    try:
        import paramiko
    except ImportError:
        return jsonify({'error': 'paramiko not installed. Run: pip install paramiko'}), 500

    try:
        req = request_data()
        host = valid_ipv4(req.get('host', '192.168.100.2'))
        user = str(req.get('user', 'root')).strip()
        password = req.get('password', 'root')
        port = bounded_int(req.get('port', 22), 'port', 1, 65535)
    except ValueError as exc:
        return api_error(str(exc))
    if not host or not re.fullmatch(r'[a-z_][a-z0-9_-]{0,31}', user):
        return api_error('A valid IPv4 host and SSH username are required.')
    if not isinstance(password, str) or len(password) > 256:
        return api_error('Invalid SSH password.')

    # Close existing session
    if ssh_client:
        try:
            ssh_running = False
            if ssh_channel:
                ssh_channel.close()
            ssh_client.close()
        except:
            pass
        ssh_client = None
        ssh_channel = None

    try:
        ssh_client = build_ssh_client()

        connect_kwargs = {'hostname': host, 'port': port, 'username': user, 'timeout': 10}
        if password != '':
            connect_kwargs['password'] = password
        else:
            connect_kwargs['password'] = ''
            connect_kwargs['allow_agent'] = True
            connect_kwargs['look_for_keys'] = True

        ssh_client.connect(**connect_kwargs)

        # Open interactive shell
        ssh_channel = ssh_client.invoke_shell(term='xterm', width=120, height=40)
        ssh_channel.settimeout(0.1)

        ssh_running = True
        ssh_read_thread = threading.Thread(target=ssh_reader, daemon=True)
        ssh_read_thread.start()

        return jsonify({'status': 'connected', 'host': host, 'user': user})
    except Exception as e:
        ssh_client = None
        ssh_channel = None
        return jsonify({'error': f'SSH connection failed. Add the device host key to {SSH_KNOWN_HOSTS} and verify credentials.'}), 400

@app.route('/api/ssh/disconnect', methods=['POST'])
def ssh_disconnect():
    global ssh_client, ssh_channel, ssh_running
    ssh_running = False
    if ssh_channel:
        try:
            ssh_channel.close()
        except:
            pass
        ssh_channel = None
    if ssh_client:
        try:
            ssh_client.close()
        except:
            pass
        ssh_client = None
    return jsonify({'status': 'disconnected'})

@app.route('/api/ssh/write', methods=['POST'])
def ssh_write():
    global ssh_channel
    try:
        text = request_data().get('text', '')
    except ValueError as exc:
        return api_error(str(exc))
    if not isinstance(text, str) or len(text) > 4096:
        return api_error('SSH command must be text no longer than 4096 characters.')
    if ssh_channel:
        try:
            ssh_channel.send(text + '\n')
            return jsonify({'status': 'ok'})
        except Exception as e:
            return jsonify({'error': str(e)}), 400
    return jsonify({'error': 'Not connected'}), 400


# ============================================================
# WebSocket events
# ============================================================
@socketio.on('connect')
def handle_connect():
    if API_TOKEN:
        provided = request.args.get('token', '')
        if not secrets.compare_digest(provided, API_TOKEN):
            return False
    print('Client connected')
    emit('connected', {'status': 'ok'})

@socketio.on('start_inference')
def handle_start_inference(data):
    global inference_running, inference_thread
    ip = valid_ipv4((data or {}).get('ip'))
    if ip and not inference_running:
        inference_running = True
        inference_thread = threading.Thread(target=udp_listener, args=(ip,), daemon=True)
        inference_thread.start()

@socketio.on('stop_inference')
def handle_stop_inference(data):
    global inference_running
    inference_running = False


@app.route('/api/inference/trigger', methods=['POST'])
def send_inference_trigger():
    """Relay a manual UI trigger to the board's Ethernet trigger input."""
    try:
        data = request_data()
        ip = valid_ipv4(data.get('ip', '192.168.100.2'))
        port = bounded_int(data.get('port', 8082), 'port', 1024, 65535)
    except ValueError as exc:
        return api_error(str(exc))
    if not ip:
        return api_error('A valid IPv4 device address is required.')

    payload = b'TRIGGER\n'
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as trigger_socket:
            sent = trigger_socket.sendto(payload, (ip, port))
    except OSError:
        return api_error('Could not send the Ethernet trigger to the device.', 502)

    return jsonify({
        'status': 'sent',
        'ip': ip,
        'port': port,
        'bytes': sent,
        'sent_at': int(time.time() * 1000),
    })


@app.route('/api/stream_proxy')
def stream_proxy():
    ip = valid_ipv4(request.args.get('ip', '192.168.100.2'))
    if not ip:
        return api_error('A valid IPv4 device address is required.')
    try:
        port = bounded_int(request.args.get('port', 8080), 'port', 1, 65535)
    except ValueError as exc:
        return api_error(str(exc))

    def generate():
        import time
        video_frame_count = 0
        video_last_fps_time = time.time()
        _reconnect_count = 0
        
        while True:
            sock = None
            try:
                sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
                sock.settimeout(1.5)
                sock.connect((ip, port))
                
                request_str = (
                    f"GET / HTTP/1.0\r\n"
                    f"Host: {ip}:{port}\r\n"
                    f"Accept: multipart/x-mixed-replace\r\n"
                    f"Connection: close\r\n\r\n"
                )
                sock.sendall(request_str.encode())
                # A healthy MJPEG source produces a frame well inside five
                # seconds. A shorter timeout prevents abandoned browser image
                # requests from accumulating upstream connections forever.
                sock.settimeout(5.0)
                
                header_buf = b''
                while b'\r\n\r\n' not in header_buf:
                    chunk = sock.recv(1024)
                    if not chunk:
                        break
                    header_buf += chunk
                    
                if b'200' not in header_buf.split(b'\r\n')[0]:
                    raise Exception("Bad HTTP response")
                
                remaining = header_buf.split(b'\r\n\r\n', 1)[1]
                buf = bytearray(remaining)
                
                while True:
                    try:
                        chunk = sock.recv(65536)
                    except socket.timeout:
                        break
                    
                    if not chunk:
                        break
                    
                    buf.extend(chunk)
                    
                    while True:
                        start = buf.find(b'\xff\xd8')
                        if start == -1:
                            if len(buf) > 2:
                                del buf[:len(buf)-2]
                            break
                        
                        end = buf.find(b'\xff\xd9', start + 2)
                        if end == -1:
                            if start > 0:
                                del buf[:start]
                            break
                            
                        jpg_bytes = bytes(buf[start:end+2])
                        del buf[:end+2]
                        _reconnect_count = 0
                        
                        video_frame_count += 1
                        now = time.time()
                        if now - video_last_fps_time >= 1.0:
                            fps = video_frame_count / (now - video_last_fps_time)
                            socketio.emit('video_meta', {'fps': round(fps, 1)}, namespace='/')
                            video_frame_count = 0
                            video_last_fps_time = now
                        
                        yield (b'--frame\r\n'
                               b'Content-Type: image/jpeg\r\n'
                               b'Content-Length: ' + str(len(jpg_bytes)).encode() + b'\r\n\r\n' +
                               jpg_bytes + b'\r\n')
                        
                    if len(buf) > 2 * 1024 * 1024:
                        buf.clear()
                        
            except GeneratorExit:
                if sock:
                    try: sock.close()
                    except: pass
                return
            except Exception as exc:
                print(f'[stream_proxy] {ip}:{port} disconnected: {exc}', flush=True)
            finally:
                if sock:
                    try: sock.close()
                    except: pass
            
            _reconnect_count += 1
            if _reconnect_count >= 5:
                print(f'[stream_proxy] giving up {ip}:{port} after 5 retries', flush=True)
                return
            time.sleep(min(_reconnect_count * 0.1, 0.5))

    return Response(generate(), mimetype='multipart/x-mixed-replace; boundary=frame')

@socketio.on('disconnect')
def handle_disconnect():
    print('Client disconnected')


# ============================================================
# Main
# ============================================================
if __name__ == '__main__':
    print('═' * 60)
    print('   LaiLab Nano V1 - AI Edge Inference Platform')
    print('   http://localhost:5000')
    print('═' * 60)
    host = os.getenv('LAI_LAB_HOST', '127.0.0.1')
    port = bounded_int(os.getenv('LAI_LAB_PORT', '5000'), 'LAI_LAB_PORT', 1, 65535)
    debug = os.getenv('LAI_LAB_DEBUG', '').lower() in {'1', 'true', 'yes'}
    socketio.run(app, host=host, port=port, debug=debug, allow_unsafe_werkzeug=debug)
