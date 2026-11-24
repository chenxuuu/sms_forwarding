#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
把 lpac 的 stdio APDU 后端转发到短信转发器的 /esim/apdu 接口。

固件只做 APDU 透传（AT+CCHO / AT+CGLA / AT+CCHC），SGP.22 的下载流程
（连 SM-DP+、校验证书、解密安装包）交给 lpac 在电脑上跑。

用法：
    # 先看卡信息（不写卡）
    python tools/esim_bridge.py --host 192.168.1.9 -- lpac chip info

    # 列出配置
    python tools/esim_bridge.py --host 192.168.1.9 -- lpac profile list

    # 下载新配置（二维码解析出来的激活码）
    python tools/esim_bridge.py --host 192.168.1.9 -- \
        lpac profile download -a "LPA:1$rsp.truphone.com$QR-G-5C-1LS-1W1Z9P7"

依赖：Python 3.8+（只用标准库）、lpac（https://github.com/estkme-group/lpac/releases）。
lpac 以独立进程运行，本脚本与固件都不包含 lpac 的代码。
"""

import argparse
import base64
import json
import os
import shutil
import subprocess
import sys
import urllib.error
import urllib.parse
import urllib.request


def log(msg):
    sys.stderr.write(msg + "\n")
    sys.stderr.flush()


class Device(object):
    def __init__(self, host, port, user, password, timeout):
        self.base = "http://%s:%d/esim/apdu" % (host, port)
        self.auth = base64.b64encode(("%s:%s" % (user, password)).encode("utf-8")).decode("ascii")
        self.timeout = timeout
        self.count = 0

    def call(self, func, param=""):
        body = urllib.parse.urlencode({"func": func, "param": param}).encode("utf-8")
        req = urllib.request.Request(self.base, data=body, method="POST")
        req.add_header("Authorization", "Basic " + self.auth)
        req.add_header("Content-Type", "application/x-www-form-urlencoded")
        with urllib.request.urlopen(req, timeout=self.timeout) as resp:
            return json.loads(resp.read().decode("utf-8"))


def main():
    ap = argparse.ArgumentParser(description="把 lpac 的 APDU 转发到短信转发器")
    ap.add_argument("--host", required=True, help="设备 IP，例如 192.168.1.9")
    ap.add_argument("--port", type=int, default=80)
    ap.add_argument("--user", default="admin")
    ap.add_argument("--password", default="admin123")
    ap.add_argument("--timeout", type=int, default=60, help="单条 APDU 的 HTTP 超时（秒）")
    ap.add_argument("--lpac", default=None, help="lpac 可执行文件路径（默认从 PATH 找）")
    ap.add_argument("cmd", nargs=argparse.REMAINDER, help="-- 之后的 lpac 命令")
    args = ap.parse_args()

    cmd = args.cmd
    if cmd and cmd[0] == "--":
        cmd = cmd[1:]
    if not cmd:
        ap.error("缺少 lpac 命令，例如：-- lpac chip info")

    if args.lpac:
        cmd[0] = args.lpac
    elif not shutil.which(cmd[0]):
        log("找不到 %s，请先安装 lpac 或用 --lpac 指定路径" % cmd[0])
        return 2

    dev = Device(args.host, args.port, args.user, args.password, args.timeout)

    env = dict(os.environ)
    env["LPAC_APDU"] = "stdio"  # 固定用 stdio 后端，由本脚本转成 HTTP

    log("→ 设备 %s" % args.host)
    log("→ 命令 %s" % " ".join(cmd))

    try:
        # 必须用二进制管道：Windows 的文本模式会把 "\n" 改写成 "\r\n"，
        # lpac 的 JSON 解析器不接受行尾的 \r，收到第一行就退出（再写会报 EINVAL）。
        proc = subprocess.Popen(
            cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=None,
            env=env, bufsize=0,
        )
    except OSError as exc:
        log("启动 lpac 失败：%s" % exc)
        return 2

    try:
        while True:
            raw = proc.stdout.readline()
            if not raw:
                break
            line = raw.decode("utf-8", "replace").strip()
            if not line:
                continue
            try:
                msg = json.loads(line)
            except ValueError:
                print(line)
                continue

            if msg.get("type") != "apdu":
                # lpac 的正常输出（结果 JSON、进度）原样透传
                print(line)
                sys.stdout.flush()
                continue

            payload = msg.get("payload") or {}
            func = payload.get("func") or ""
            param = payload.get("param") or ""
            reply = {"type": "apdu", "payload": {"ecode": -1}}
            try:
                dev.count += 1
                if dev.count % 25 == 0:
                    log("...已转发 %d 条 APDU" % dev.count)
                res = dev.call(func, param)
                if res.get("success"):
                    reply["payload"]["ecode"] = int(res.get("ecode", 0))
                    if res.get("data"):
                        reply["payload"]["data"] = res["data"]
                else:
                    log("设备返回错误（%s）：%s" % (func, res.get("message") or "未知"))
            except (urllib.error.URLError, OSError, ValueError) as exc:
                log("请求设备失败（%s）：%s" % (func, exc))

            proc.stdin.write((json.dumps(reply) + "\n").encode("utf-8"))
            proc.stdin.flush()
    except KeyboardInterrupt:
        log("已中断")
        proc.kill()
        return 130
    finally:
        try:
            proc.stdin.close()
        except Exception:
            pass

    rc = proc.wait()
    log("← lpac 退出，共转发 %d 条 APDU，返回码 %d" % (dev.count, rc))
    return rc


if __name__ == "__main__":
    sys.exit(main())
