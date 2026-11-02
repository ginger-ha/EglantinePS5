#!/usr/bin/env python3
# 干什么: 将编译出的 eboot.bin 推送更新到 PS5 (优先走 FTP，无 FTP 时走 Web File Manager)
# 怎么跑: python3 tools/deploy_ps5.py [PS5_IP]
# 需要什么: PS5 开机且 FTP (端口 2121) 或 Web File Manager (端口 8888) 运行中

import os
import sys
import time
import socket
import ftplib
import json
import urllib.request
import urllib.parse

DEFAULT_IP = os.environ.get("PS5_IP", "192.168.88.33")
FTP_PORT = int(os.environ.get("PS5_FTP_PORT", "2121"))
HTTP_PORT = int(os.environ.get("PS5_PORT", "8888"))
LOCAL_FILE = "build/release/PPSA98515/eboot.bin"
TARGET_DIRS = ["/mnt/usb1/PPSA98515", "/mnt/usb1/homebrew/PPSA98515"]

def is_port_open(ip, port):
    s = socket.socket()
    s.settimeout(1.0)
    try:
        s.connect((ip, port))
        s.close()
        return True
    except:
        return False

def deploy_via_ftp(ip, port, local_file, remote_dirs):
    filename = os.path.basename(local_file)
    size = os.path.getsize(local_file)
    print(f"[*] Connecting to FTP at {ip}:{port}...")
    ftp = ftplib.FTP()
    ftp.connect(ip, port, timeout=15)
    ftp.login()
    for d in remote_dirs:
        print(f"[*] [FTP] Uploading {filename} ({size} bytes) -> {d}/{filename} ...")
        ftp.cwd(d)
        with open(local_file, "rb") as f:
            t0 = time.time()
            try:
                ftp.delete(f"{filename}.old")
            except:
                pass
            try:
                ftp.rename(filename, f"{filename}.old")
            except:
                pass
            ftp.storbinary(f"STOR {filename}", f, blocksize=256*1024)
            dur = time.time() - t0
        rate_mb = (size / 1024 / 1024) / max(dur, 0.001)
        print(f"[+] [FTP] Deployed to {d}/{filename} in {dur:.2f}s ({rate_mb:.2f} MB/s)")
    ftp.quit()

def deploy_via_http(ip, port, local_file, remote_dirs):
    filename = os.path.basename(local_file)
    size = os.path.getsize(local_file)
    with open(local_file, "rb") as f:
        file_bytes = f.read()

    for remote_dir in remote_dirs:
        print(f"[*] [HTTP] Uploading {filename} ({size} bytes) -> {remote_dir}/{filename} ...")
        prep_data = urllib.parse.urlencode({
            "path": remote_dir,
            "src": filename,
            "total": str(size),
            "count": "1",
            "rels": filename,
            "sizes": str(size),
            "overwrite": "1"
        }).encode("utf-8")

        req = urllib.request.Request(f"http://{ip}:{port}/api/upload/prepare", data=prep_data)
        with urllib.request.urlopen(req, timeout=10) as resp:
            res = json.loads(resp.read().decode("utf-8"))
            task_id = res["task_id"]

        upload_req = urllib.request.Request(
            f"http://{ip}:{port}/api/upload-file",
            data=file_bytes,
            headers={
                "Content-Type": "application/octet-stream",
                "X-WFM-Task-ID": str(task_id),
                "X-WFM-Path": urllib.parse.quote(remote_dir),
                "X-WFM-Rel": urllib.parse.quote(filename),
                "X-WFM-Size": str(size),
                "X-WFM-Overwrite": "1"
            }
        )
        with urllib.request.urlopen(upload_req, timeout=60) as resp:
            if resp.status != 200:
                raise RuntimeError(f"Upload failed with HTTP {resp.status}")

        finish_req = urllib.request.Request(f"http://{ip}:{port}/api/upload/finish?task_id={task_id}", method="POST")
        with urllib.request.urlopen(finish_req, timeout=10) as resp:
            if resp.status != 200:
                raise RuntimeError(f"Finish failed with HTTP {resp.status}")

        print(f"[+] [HTTP] Deployed to {remote_dir}/{filename}")

def main():
    ip = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_IP
    if not os.path.exists(LOCAL_FILE):
        print(f"[-] Local file not found: {LOCAL_FILE}")
        sys.exit(1)

    if is_port_open(ip, FTP_PORT):
        deploy_via_ftp(ip, FTP_PORT, LOCAL_FILE, TARGET_DIRS)
    elif is_port_open(ip, HTTP_PORT):
        deploy_via_http(ip, HTTP_PORT, LOCAL_FILE, TARGET_DIRS)
    else:
        print(f"[-] Neither FTP ({FTP_PORT}) nor Web File Manager ({HTTP_PORT}) is reachable on {ip}")
        sys.exit(1)

if __name__ == "__main__":
    main()
