import unittest

import app


class DeployModeValidationTests(unittest.TestCase):
    def setUp(self):
        self.model_name = 'test_runtime_modes.cvimodel'
        self.model_path = app.OUTPUT_FOLDER / self.model_name
        self.model_path.write_bytes(b'test')

    def tearDown(self):
        self.model_path.unlink(missing_ok=True)

    def base_request(self):
        return {'model_filename': self.model_name, 'ip': '192.168.100.2'}

    def test_accepts_all_runtime_configuration(self):
        request = self.base_request()
        request.update({
            'inferenceMode': 'all',
            'streamOutput': False,
            'triggerSource': 'gpio',
            'triggerGpio': 502,
            'triggerEdge': 'both',
            'outputTransport': 'both',
            'metadataPort': 9081,
        })
        config = app.validate_deploy_request(request)
        self.assertEqual(config['inferenceMode'], 'all')
        self.assertFalse(config['streamOutput'])
        self.assertEqual(config['triggerSource'], 'gpio')
        self.assertEqual(config['outputTransport'], 'both')

    def test_defaults_ssh_password_to_root(self):
        config = app.validate_deploy_request(self.base_request())
        self.assertEqual(config['password'], 'root')

    def test_rejects_unknown_runtime_mode(self):
        request = self.base_request()
        request['inferenceMode'] = 'burst'
        with self.assertRaisesRegex(ValueError, 'inferenceMode'):
            app.validate_deploy_request(request)

    def test_rejects_trigger_without_yolo(self):
        request = self.base_request()
        request.update({'inferenceMode': 'trigger', 'noYolo': True})
        with self.assertRaisesRegex(ValueError, 'require YOLO'):
            app.validate_deploy_request(request)

    def test_rejects_uart_trigger_without_uart_device(self):
        request = self.base_request()
        request.update({
            'inferenceMode': 'trigger',
            'triggerSource': 'uart',
            'outputTransport': 'none',
            'uartDev': '',
        })
        with self.assertRaisesRegex(ValueError, 'UART device'):
            app.validate_deploy_request(request)


if __name__ == '__main__':
    unittest.main()
