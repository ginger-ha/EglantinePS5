#!/usr/bin/env python3
# 干什么: 从 PS5 获取最新的 eden_log.txt 并打印最后 N 行 (优先走 FTP)
# 怎么跑: python3 tools/fetch_log.py [N_LINES]
# 需要什么: PS5 FTP (端口 2121) 或 Web File Manager (端口 8888) 运行中

import sys
import os
import io
import socket
import ftplib
import urllib.request
import urllib.parse
import json

IP = os.environ.get("PS5_IP", "192.168.88.33")
FTP_PORT = int(os.environ.get("PS5_FTP_PORT", "2121"))
HTTP_PORT = int(os.environ.get("PS5_PORT", "8888"))
LOG_PATH = "/data/eglantineps5/user/log/eden_log.txt"

def is_port_open(ip, port):
    s = socket.socket()
    s.settimeout(1.0)
    try:
        s.connect((ip, port))
        s.close()
        return True
    except:
        return False

def fetch_ftp(lines_to_show):
    ftp = ftplib.FTP()
    ftp.connect(IP, FTP_PORT, timeout=10)
    ftp.login()
    buf = io.BytesIO()
    ftp.retrbinary(f"RETR {LOG_PATH}", buf.write)
    ftp.quit()
    content = buf.getvalue().decode("utf-8", errors="ignore")
    lines = content.splitlines()
    print(f"--- PS5 eden_log.txt via FTP (Total {len(lines)} lines, showing last {lines_to_show}) ---")
    for line in lines[-lines_to_show:]:
        print(line)

def fetch_http(lines_to_show):
    prep_data = urllib.parse.urlencode({"paths": LOG_PATH}).encode("utf-8")
    req = urllib.request.Request(f"http://{IP}:{HTTP_PORT}/api/download/prepare", data=prep_data)
    with urllib.request.urlopen(req, timeout=10) as resp:
        data = json.loads(resp.read().decode("utf-8"))
        task_id = data["task_id"]

    dl_req = urllib.request.Request(f"http://{IP}:{HTTP_PORT}/api/download?id={task_id}")
    with urllib.request.urlopen(dl_req, timeout=30) as resp:
        content = resp.read().decode("utf-8", errors="ignore")
        lines = content.splitlines()
        print(f"--- PS5 eden_log.txt via HTTP (Total {len(lines)} lines, showing last {lines_to_show}) ---")
        for line in lines[-lines_to_show:]:
            print(line)

def main():
    lines_to_show = int(sys.argv[1]) if len(sys.argv) > 1 else 50
    if is_port_open(IP, FTP_PORT):
        fetch_ftp(lines_to_show)
    elif is_port_open(IP, HTTP_PORT):
        fetch_http(lines_to_show)
    else:
        print(f"Neither FTP ({FTP_PORT}) nor HTTP ({HTTP_PORT}) reachable.")

if __name__ == "__main__":
    main()
