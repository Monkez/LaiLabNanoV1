import io
import json
import unittest
from pathlib import Path
from unittest.mock import MagicMock, patch

import app


class SecurityBoundaryTests(unittest.TestCase):
    def setUp(self):
        self.client = app.app.test_client()

    def test_rejects_shell_characters_in_conversion_config(self):
        response = self.client.post('/api/jobs', json={'model_name': 'yolo;id'})
        self.assertEqual(response.status_code, 400)

    def test_rejects_deployment_path_traversal(self):
        response = self.client.post('/api/deploy', json={'model_filename': '../../app.py'})
        self.assertEqual(response.status_code, 400)

    def test_rejects_model_download_path_traversal(self):
        response = self.client.get('/api/models/../../app.py/download')
        self.assertEqual(response.status_code, 404)

    def test_upload_limit_returns_json_error(self):
        old_limit = app.app.config['MAX_CONTENT_LENGTH']
        app.app.config['MAX_CONTENT_LENGTH'] = 8
        try:
            response = self.client.post(
                '/api/upload',
                data={'file': (io.BytesIO(b'0123456789'), 'model.pt')},
                content_type='multipart/form-data',
            )
        finally:
            app.app.config['MAX_CONTENT_LENGTH'] = old_limit
        self.assertEqual(response.status_code, 413)
        self.assertIn('error', response.get_json())

    def test_api_token_protects_api_routes(self):
        old_token = app.API_TOKEN
        app.API_TOKEN = 'test-token'
        try:
            self.assertEqual(self.client.get('/api/jobs').status_code, 401)
            response = self.client.get('/api/jobs', headers={'X-Api-Token': 'test-token'})
            self.assertEqual(response.status_code, 200)
        finally:
            app.API_TOKEN = old_token

    def test_conversion_job_with_path_is_json_serializable(self):
        job = app.ConversionJob('test-job', {})
        job.output_model = Path('outputs') / 'test-job_yolo11n_320.cvimodel'
        payload = job.to_dict()
        self.assertEqual(payload['output_model'], 'test-job_yolo11n_320.cvimodel')
        json.dumps(payload)

    def test_manual_trigger_sends_validated_udp_datagram(self):
        trigger_socket = MagicMock()
        trigger_socket.__enter__.return_value = trigger_socket
        trigger_socket.sendto.return_value = 8
        with patch('app.socket.socket', return_value=trigger_socket):
            response = self.client.post('/api/inference/trigger', json={
                'ip': '192.168.100.2',
                'port': 8082,
            })
        self.assertEqual(response.status_code, 200)
        trigger_socket.sendto.assert_called_once_with(b'TRIGGER\n', ('192.168.100.2', 8082))

    def test_manual_trigger_rejects_invalid_target(self):
        response = self.client.post('/api/inference/trigger', json={
            'ip': 'not-an-ip',
            'port': 80,
        })
        self.assertEqual(response.status_code, 400)


if __name__ == '__main__':
    unittest.main()
