import re
with open('deploy/Yolo_CSIStream', 'rb') as f:
    data = f.read()
    data = data.replace(b'\x00', b' ')
    s = data.decode('ascii', 'ignore')
matches = re.findall(r'[ -~]{40,}', s)
for m in matches:
    if 'Usage' in m or '--' in m:
        print(m.strip())
