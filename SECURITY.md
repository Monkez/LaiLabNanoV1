# Security configuration

LaiLab Nano controls Docker, serial devices, SSH, file uploads, and board reboot.
It is designed to run on the local development machine by default.

## Safe default

`start.bat` starts the application on `127.0.0.1:5000`, with Flask debug mode
disabled. Do not expose port 5000 through a router, reverse proxy, or public
tunnel without the controls below.

## Network-exposed installation

Set a long random API token before starting the application. On Windows PowerShell:

```powershell
$env:LAI_LAB_HOST = '0.0.0.0'
$env:LAI_LAB_API_TOKEN = '<long-random-secret>'
.\start.bat
```

In the browser that opens the UI, set the same token once:

```js
localStorage.setItem('lailab_api_token', '<long-random-secret>')
location.reload()
```

The token is sent as `X-Api-Token` for HTTP requests and during the Socket.IO
handshake. Use HTTPS and a reverse proxy when accessing the app outside a trusted
LAN; the token alone does not encrypt traffic.

## SSH host verification

The app rejects unknown SSH host keys. Add the board key before using the SSH
terminal or deployment workflow:

```powershell
ssh-keyscan -H 192.168.100.2 >> $HOME\.ssh\known_hosts
```

Set `LAI_LAB_SSH_KNOWN_HOSTS` if the known-hosts file is elsewhere. Verify the
fingerprint from a trusted board console before accepting a new key.

## Upload limit

The default maximum model upload is 1024 MB. Change it only when needed:

```powershell
$env:LAI_LAB_MAX_UPLOAD_MB = '2048'
```

Uploaded `.pt` files are not executed by the web server, but model conversion
uses third-party ML tooling in Docker. Treat models from untrusted sources as
untrusted input and use an isolated Docker host for high-risk workloads.
