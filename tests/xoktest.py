#!/usr/bin/env python3
"""
Drive Xok/ExOS in QEMU over the serial console: boot, wait for the
shell prompt, run commands and collect their output.

usage: xoktest.py BUILDDIR [options] -- 'cmd1' 'cmd2' ...
"""
import argparse, os, select, subprocess, sys, time

PROMPT = b"# "

class Xok:
    def __init__(self, builddir, disk=None, extra=(), mem=256, smp=2,
                 machine=None, net=None, log=None):
        self.log = open(log, "wb") if log else None
        args = ["qemu-system-i386", "-m", str(mem), "-smp", str(smp),
                "-nographic", "-serial", "stdio", "-monitor", "none",
                "-no-reboot", "-kernel", os.path.join(builddir, "xok.mb")]
        if machine:
            args += ["-M", machine]
        if disk:
            args += list(disk)
        if net is not None:
            args += list(net)
        else:
            args += ["-nic", "none"]
        args += list(extra)
        self.p = subprocess.Popen(args, stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE,
                                  stderr=subprocess.STDOUT)
        self.buf = b""

    def read_until(self, token, timeout):
        end = time.time() + timeout
        while token not in self.buf:
            left = end - time.time()
            if left <= 0:
                raise TimeoutError("timeout waiting for %r; got:\n%s" %
                                   (token, self.buf[-2000:].decode(errors="replace")))
            r, _, _ = select.select([self.p.stdout], [], [], left)
            if r:
                data = os.read(self.p.stdout.fileno(), 65536)
                if not data:
                    raise EOFError("QEMU exited; output:\n" +
                                   self.buf[-3000:].decode(errors="replace"))
                if self.log:
                    self.log.write(data)
                    self.log.flush()
                self.buf += data
        i = self.buf.index(token) + len(token)
        out, self.buf = self.buf[:i], self.buf[i:]
        return out

    def send(self, s):
        for ch in s.encode():
            self.p.stdin.write(bytes([ch]))
            self.p.stdin.flush()
            time.sleep(0.002)

    def cmd(self, c, timeout=60):
        self.send(c + "\r")
        out = self.read_until(PROMPT, timeout)
        text = out.decode(errors="replace").replace("\r", "")
        lines = text.split("\n")
        # Drop the echoed command line and the next prompt.
        if lines and lines[0].strip().endswith(c.strip()[:20]) or (lines and c[:10] in lines[0]):
            lines = lines[1:]
        return "\n".join(lines[:-1])

    def close(self, timeout=20):
        try:
            self.p.stdin.close()
        except Exception:
            pass
        try:
            self.p.wait(timeout)
        except subprocess.TimeoutExpired:
            self.p.kill()
            self.p.wait()

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("builddir")
    ap.add_argument("--disk", default=None)
    ap.add_argument("--timeout", type=float, default=60)
    ap.add_argument("cmds", nargs="*")
    a = ap.parse_args()
    disk = ["-drive", "file=%s,format=raw,if=ide" %
            (a.disk or os.path.join(a.builddir, "disk.img"))]
    x = Xok(a.builddir, disk=disk)
    try:
        x.read_until(PROMPT, a.timeout)
        for c in a.cmds:
            print("$ " + c)
            print(x.cmd(c, a.timeout))
        x.send("poweroff\r")
        x.close()
    except Exception as e:
        print("ERROR:", e)
        x.p.kill()
        sys.exit(1)

if __name__ == "__main__":
    main()
