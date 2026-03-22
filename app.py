"""
LaiLab Nano V1 - Backend Server
AI Edge Inference Model Preparation & Deployment Platform

Real Docker integration for YOLO model conversion pipeline.
"""

import os
import json
import subprocess
import threading
import time
import uuid
import shlex
import signal
from pathlib import Path
from flask import Flask, render_template, request, jsonify, send_from_directory, send_file, Response
from flask_cors import CORS
from flask_socketio import SocketIO, emit
from werkzeug.utils import secure_filename

app = Flask(__name__, static_folder='static', template_folder='templates')
CORS(app)
socketio = SocketIO(app, cors_allowed_origins="*", async_mode='threading')

# ============================================================
# Configuration
# ============================================================
UPLOAD_FOLDER = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'uploads')
OUTPUT_FOLDER = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'outputs')
os.makedirs(UPLOAD_FOLDER, exist_ok=True)
os.makedirs(OUTPUT_FOLDER, exist_ok=True)

ALLOWED_EXTENSIONS = {'pt'}

# Docker configuration
DOCKER_IMAGE = 'sophgo/tpuc_dev:latest'
DOCKER_DEFAULT_CONTAINER = 'TPU-LAILAB-NANO-CONTAINER'
DOCKER_TPU_MLIR_PATH = '/workspace/tpu-mlir'
DOCKER_WORK_BASE = '/workspace/tpu-mlir'

# Track running jobs
jobs = {}
# Track running subprocesses for cancellation
running_processes = {}


def allowed_file(filename):
    return '.' in filename and filename.rsplit('.', 1)[1].lower() in ALLOWED_EXTENSIONS


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
            if job and job.status == 'failed':
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


def docker_pull_image(image, job=None):
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

        for line in iter(process.stdout.readline, ''):
            line = line.rstrip('\n\r')
            if line and job:
                emit_log(job, line, 'info')

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
        return {
            'job_id': self.job_id,
            'config': self.config,
            'status': self.status,
            'current_step': self.current_step,
            'total_steps': self.total_steps,
            'steps': self.steps,
            'created_at': self.created_at,
            'logs': self.logs[-200:],
            'output_model': self.output_model,
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

        # Clean up old models from outputs folder
        if os.path.exists(OUTPUT_FOLDER):
            for old_file in os.listdir(OUTPUT_FOLDER):
                if old_file.endswith('.cvimodel'):
                    old_path = os.path.join(OUTPUT_FOLDER, old_file)
                    try:
                        os.remove(old_path)
                        emit_log(job, f'Removed old model: {old_file}')
                    except Exception as e:
                        emit_log(job, f'Warning: could not remove {old_file}: {e}', 'warning')

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

        # Check if tpu-mlir exists and is built
        emit_log(job, 'Checking tpu-mlir installation...')
        success, output = docker_exec(
            docker_container,
            f'test -f {DOCKER_TPU_MLIR_PATH}/envsetup.sh && echo "EXISTS" || echo "NOT_FOUND"'
        )
        tpu_mlir_exists = 'EXISTS' in output

        if not tpu_mlir_exists:
            emit_log(job, 'tpu-mlir not found. Cloning and building (this may take a while)...', 'warning')
            success, _ = docker_exec(
                docker_container,
                'git clone -b v1.7 --depth 1 https://github.com/sophgo/tpu-mlir.git /workspace/tpu-mlir',
                cwd='/workspace',
                job=job,
                timeout=600
            )
            if not success:
                step_fail(job, 0, 'Failed to clone tpu-mlir repository.')
                return

            emit_log(job, 'Building tpu-mlir...')
            success, _ = docker_exec(
                docker_container,
                'source ./envsetup.sh && ./build.sh',
                cwd=DOCKER_TPU_MLIR_PATH,
                job=job,
                timeout=1800  # 30 min timeout for build
            )
            if not success:
                step_fail(job, 0, 'Failed to build tpu-mlir.')
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
            host_pt_path = os.path.join(UPLOAD_FOLDER, model_file)
            if os.path.exists(host_pt_path):
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

        # Download export_to_onnx.py
        emit_log(job, 'Downloading export_to_onnx.py...')
        success, _ = docker_exec(
            docker_container,
            'curl -L -O https://raw.githubusercontent.com/ret7020/LicheeRVNano/refs/heads/master/Projects/Yolov8/export_to_onnx.py',
            cwd=workspace_dir,
            job=job,
            timeout=120
        )
        if not success:
            step_fail(job, 1, 'Failed to download export_to_onnx.py.')
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
        step_start(job, 3, f'[Step 4/5] Running calibration ({calibration_count} images)...')

        calib_cmd = (
            f'{tpu_env_cmd} && '
            f'{cmd_run_calibration} '
            f'{model_name}.mlir '
            f'--dataset ../COCO2017 '
            f'--input_num {calibration_count} '
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

        # Verify calibration table
        success, check = docker_exec(
            docker_container,
            f'test -f {workspace_dir}/{model_name}_calib_table && echo "OK" || echo "FAIL"'
        )
        if 'FAIL' in check:
            step_fail(job, 3, f'{model_name}_calib_table was not created.')
            return

        step_done(job, 3, f'Calibration complete ({calibration_count} images)')

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
        host_output_path = os.path.join(OUTPUT_FOLDER, f'{job.job_id}_{output_model_name}')
        emit_log(job, f'Copying {output_model_name} to host...')
        if docker_copy_from(docker_container, f'{workspace_dir}/{output_model_name}', host_output_path):
            job.output_model = host_output_path
            emit_log(job, f'Output saved: {host_output_path}', 'success')
        else:
            emit_log(job, 'Warning: Could not copy model to host. Model is still in container.', 'warning')

        step_done(job, 4, f'Generated {output_model_name} ({quantize.upper()}, {processor})')

        # Set output_model to the filename for frontend download
        if not job.output_model:
            job.output_model = output_model_name

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
        filename = secure_filename(file.filename)
        filepath = os.path.join(UPLOAD_FOLDER, filename)
        file.save(filepath)
        return jsonify({
            'filename': filename,
            'size': os.path.getsize(filepath),
            'path': filepath
        })
    else:
        return jsonify({'error': 'Only .pt files are allowed'}), 400


# ---- Jobs ----
@app.route('/api/jobs', methods=['GET'])
def list_jobs():
    return jsonify([j.to_dict() for j in jobs.values()])


@app.route('/api/jobs', methods=['POST'])
def create_job():
    config = request.json
    job_id = str(uuid.uuid4())[:8]
    job = ConversionJob(job_id, config)
    jobs[job_id] = job

    # Start pipeline in background thread
    thread = threading.Thread(target=run_conversion_pipeline, args=(job,), daemon=True)
    thread.start()

    return jsonify(job.to_dict()), 201


@app.route('/api/jobs/<job_id>', methods=['GET'])
def get_job(job_id):
    job = jobs.get(job_id)
    if not job:
        return jsonify({'error': 'Job not found'}), 404
    return jsonify(job.to_dict())


@app.route('/api/jobs/<job_id>/cancel', methods=['POST'])
def cancel_job(job_id):
    job = jobs.get(job_id)
    if not job:
        return jsonify({'error': 'Job not found'}), 404

    job.status = 'failed'
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
    if not job.output_model or not os.path.exists(job.output_model):
        return jsonify({'error': 'Output model not available'}), 404

    # Strip job_id prefix from filename for cleaner download name
    raw_name = os.path.basename(job.output_model)
    if raw_name.startswith(job_id + '_'):
        clean_name = raw_name[len(job_id) + 1:]
    else:
        clean_name = raw_name

    with open(job.output_model, 'rb') as f:
        data = f.read()
    response = Response(data, mimetype='application/octet-stream')
    response.headers['Content-Disposition'] = f'attachment; filename="{clean_name}"'
    response.headers['Content-Length'] = len(data)
    response.headers['Cache-Control'] = 'no-cache, no-store, must-revalidate'
    return response


# ---- Direct file download (works after server restart) ----
@app.route('/api/models/<filename>/download', methods=['GET'])
def download_model_file(filename):
    """Download a .cvimodel file directly from the outputs folder."""
    # Security: prevent path traversal
    safe_name = os.path.basename(filename)
    fpath = os.path.join(OUTPUT_FOLDER, safe_name)
    if not os.path.exists(fpath):
        return jsonify({'error': 'File not found'}), 404

    # Strip job_id prefix for clean download name
    parts = safe_name.split('_', 1)
    clean_name = parts[1] if len(parts) > 1 else safe_name

    with open(fpath, 'rb') as f:
        data = f.read()
    response = Response(data, mimetype='application/octet-stream')
    response.headers['Content-Disposition'] = f'attachment; filename="{clean_name}"'
    response.headers['Content-Length'] = len(data)
    response.headers['Cache-Control'] = 'no-cache, no-store, must-revalidate'
    return response


# ---- Available Models ----
@app.route('/api/models', methods=['GET'])
def list_models():
    """List available .cvimodel files from outputs folder."""
    models = []
    if os.path.exists(OUTPUT_FOLDER):
        for f in os.listdir(OUTPUT_FOLDER):
            if f.endswith('.cvimodel'):
                fpath = os.path.join(OUTPUT_FOLDER, f)
                # Strip job_id prefix for display name
                parts = f.split('_', 1)
                display_name = parts[1] if len(parts) > 1 else f
                models.append({
                    'filename': f,
                    'display_name': display_name,
                    'path': fpath,
                    'size': os.path.getsize(fpath),
                    'modified': os.path.getmtime(fpath),
                })
    # Sort by modification time, newest first
    models.sort(key=lambda x: x['modified'], reverse=True)
    return jsonify({'models': models})


# ---- Presets ----
@app.route('/api/config/presets', methods=['GET'])
def get_presets():
    presets = [
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
            'id': 'yolo11n_320',
            'name': 'YOLO11 Nano (320x320)',
            'description_en': 'YOLO11n 320x320 - Optimized for edge',
            'description_vi': 'YOLO11n 320x320 - Tối ưu cho thiết bị biên',
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
    ]
    return jsonify(presets)


# ============================================================
# WebSocket events
# ============================================================
@socketio.on('connect')
def handle_connect():
    print('Client connected')
    emit('connected', {'status': 'ok'})


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
    socketio.run(app, host='0.0.0.0', port=5000, debug=True)
