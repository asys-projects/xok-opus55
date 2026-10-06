#!/usr/bin/env python3
"""
Xok/ExOS system tests: boot the system in QEMU in several machine
configurations and check the kernel, the library OS, the drivers, the
file system (including persistence and crash recovery) and networking.

usage: run-tests.py BUILDDIR [--quick]
"""
import os, shutil, subprocess, sys, tempfile, time, socket

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from xoktest import Xok, PROMPT

B = os.path.abspath(sys.argv[1])
QUICK = "--quick" in sys.argv
TMP = tempfile.mkdtemp(prefix="xoktests-")
results = []


def report(name, ok, detail=""):
    results.append((name, ok))
    print(("PASS " if ok else "FAIL ") + name + (("  -- " + detail) if (detail and not ok) else ""))
    sys.stdout.flush()


def make_disk(path, size=64, extra=()):
    progs = open(os.path.join(B, "bin/programs.lst")).read().split()
    src = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
    files = ["/etc/rc=%s/rootfs/etc/rc" % src, "/etc/motd=%s/rootfs/etc/motd" % src]
    for f in os.listdir(os.path.join(src, "rootfs/www")):
        files.append("/www/%s=%s/rootfs/www/%s" % (f, src, f))
    files += list(extra)
    args = [os.path.join(B, "tools/mkxnfs"), "-o", path, "-s", str(size), "-D", "/www"]
    for f in files:
        args += ["-f", f]
    args += ["-d", os.path.join(B, "bin")] + progs
    subprocess.run(args, check=True, stdout=subprocess.DEVNULL)


def fsck(path):
    r = subprocess.run([os.path.join(B, "tools/mkxnfs"), "-c", "-o", path],
                       capture_output=True, text=True)
    return r.returncode == 0, (r.stdout + r.stderr).strip()


def boot(disks, net="e1000", machine=None, smp=2, fwd=None, mem=256):
    d = []
    for i, (img, iface) in enumerate(disks):
        if iface == "ahci":
            d += ["-drive", "file=%s,format=raw,if=none,id=d%d" % (img, i),
                  "-device", "ide-hd,drive=d%d,bus=ide.%d" % (i, i)]
        else:
            d += ["-drive", "file=%s,format=raw,if=%s,index=%d" % (img, iface, i)]
    n = None
    if net:
        user = "user,id=n0"
        if fwd:
            user += ",hostfwd=" + fwd
        n = ["-netdev", user, "-device", net + ",netdev=n0"]
    x = Xok(B, disk=d, net=n, machine=machine, smp=smp, mem=mem,
            log=os.path.join(TMP, "console.log"))
    x.read_until(PROMPT, 120)
    return x


def shutdown(x):
    try:
        x.send("poweroff\r")
    except Exception:
        pass
    x.close(30)


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


def run_cmd(x, c, want=None, timeout=120, name=None):
    try:
        out = x.cmd(c, timeout)
    except Exception as e:
        report(name or c, False, str(e)[:300])
        return None
    if want is not None:
        ok = want in out
        report(name or c, ok, "output: " + out[-400:])
    return out


def test_main():
    """IDE + e1000, 2 CPUs: library OS, file system, network."""
    img = os.path.join(TMP, "main.img")
    big = os.path.join(TMP, "big.bin")
    with open(big, "wb") as f:
        f.write(os.urandom(300000))
    make_disk(img, extra=["/www/big.bin=" + big])
    port = free_port()
    x = boot([(img, "ide")], fwd="tcp::%d-:80,hostfwd=udp::%d-:7" % (port, port))
    run_cmd(x, "sleep 1; ifconfig", "inet 10.0.2.15", name="netd: DHCP configuration")
    out = run_cmd(x, "xoktests", timeout=600)
    if out is not None:
        for line in out.splitlines():
            if line.startswith("PASS ") or line.startswith("FAIL "):
                report("xoktests: " + line[5:], line.startswith("PASS"))
        report("xoktests: completed", "failed" in out and " 0 failed" in out)
    run_cmd(x, "echo one two three | wc -w", "3", name="shell: pipeline")
    run_cmd(x, "seq 1 50 | grep 7 | sort -r | head -2 | tail -1", "47",
            name="shell: four-stage pipeline")
    run_cmd(x, "sh -c 'exit 3'; echo status=$?", "status=3", name="shell: exit status")
    run_cmd(x, "for_test=ok; echo $for_test", "ok", name="shell: variables")
    run_cmd(x, "echo hello > /tmp/r; echo world >> /tmp/r; cat /tmp/r | wc -l",
            "2", name="shell: redirections")
    run_cmd(x, "ls /bin | wc -l", None)
    run_cmd(x, "ps", "init", name="ps: process table from u-areas")
    run_cmd(x, "ping 10.0.2.2 2", "2 received", name="net: ping gateway (ICMP)")
    run_cmd(x, "fetch -q http://localhost/ | grep -c 'Hello from Xok'", "1",
            name="net: local HTTP over loopback TCP")
    run_cmd(x, "fetch -q -o /tmp/b http://localhost/big.bin && cmp /tmp/b /www/big.bin && echo same",
            "same", name="net: 300KB HTTP transfer, written to C-FFS")
    # From the host, through QEMU's NAT.
    try:
        r = subprocess.run(["curl", "-s", "-m", "60", "http://127.0.0.1:%d/" % port],
                           capture_output=True, text=True, timeout=90)
        report("net: host fetches / from ExOS httpd", "Hello from Xok/ExOS" in r.stdout)
        r = subprocess.run("curl -s -m 60 http://127.0.0.1:%d/big.bin | md5sum" % port,
                           shell=True, capture_output=True, text=True, timeout=90)
        exp = subprocess.run(["md5sum", big], capture_output=True, text=True).stdout.split()[0]
        report("net: host downloads 300KB from ExOS httpd", r.stdout.split()[0] == exp)
        r = subprocess.run(["curl", "-s", "-m", "60", "http://127.0.0.1:%d/status" % port],
                           capture_output=True, text=True, timeout=90)
        report("net: /status page", "Quantum vectors" in r.stdout)
        ok = 0
        for i in range(20):
            r = subprocess.run(["curl", "-s", "-m", "30", "-o", "/dev/null", "-w", "%{http_code}",
                                "http://127.0.0.1:%d/index.html" % port],
                               capture_output=True, text=True, timeout=60)
            ok += r.stdout == "200"
        report("net: 20 sequential HTTP requests", ok == 20, "%d/20" % ok)
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.settimeout(10)
        s.sendto(b"xok-udp-echo", ("127.0.0.1", port))
        report("net: UDP echo", s.recvfrom(100)[0] == b"xok-udp-echo")
    except Exception as e:
        report("net: host tests", False, str(e))
    # Persistence across a clean shutdown.
    run_cmd(x, "mkdir /home/persist; echo durable > /home/persist/file; sync", None)
    shutdown(x)
    ok, msg = fsck(img)
    report("fs: image consistent after shutdown (host UDF check)", ok, msg)
    x = boot([(img, "ide")], net=None, smp=1)
    run_cmd(x, "cat /home/persist/file", "durable", name="fs: data persists across reboot")
    run_cmd(x, "cmp /tmp/b /www/big.bin && echo same",
            "same", name="fs: large file persists across reboot")
    shutdown(x)


def test_crash():
    """Writes, then the machine is killed without syncing."""
    img = os.path.join(TMP, "crash.img")
    make_disk(img)
    x = boot([(img, "ide")], net=None)
    run_cmd(x, "mkdir /home/c; seq 1 3000 > /home/c/numbers; cp /bin/sh /home/c/sh.copy", None)
    x.p.kill()
    x.p.wait()
    ok, msg = fsck(img)
    report("crash: no reachable block is free (ordering rules)", ok, msg)
    x = boot([(img, "ide")], net=None)
    out = run_cmd(x, "ls /home; echo listed", "listed", name="crash: file system usable after crash")
    run_cmd(x, "echo after > /tmp/after; cat /tmp/after; sync", "after",
            name="crash: free map rebuilt, allocation works")
    shutdown(x)
    ok, msg = fsck(img)
    report("crash: consistent after recovery", ok, msg)


def test_config(name, disk_if, net, machine=None, smp=2):
    img = os.path.join(TMP, name + ".img")
    make_disk(img)
    try:
        x = boot([(img, disk_if)], net=net, machine=machine, smp=smp)
    except Exception as e:
        report(name + ": boot", False, str(e)[:300])
        return
    report(name + ": boot to shell", True)
    run_cmd(x, "sysinfo", "disk0", name=name + ": disk driver")
    if net:
        run_cmd(x, "sleep 1; ifconfig", "link up", name=name + ": NIC")
        run_cmd(x, "ping 10.0.2.2 2", "received", name=name + ": ping")
        run_cmd(x, "fetch -q http://localhost/ | grep -c 'Hello from Xok'", "1", name=name + ": TCP")
    run_cmd(x, "cp /bin/sh /tmp/x && cmp /bin/sh /tmp/x && echo copied", "copied",
            name=name + ": file copy")
    run_cmd(x, "sync", None)
    shutdown(x)
    ok, msg = fsck(img)
    report(name + ": fsck", ok, msg)


def test_newfs():
    img = os.path.join(TMP, "n0.img")
    d2 = os.path.join(TMP, "n1.img")
    make_disk(img)
    with open(d2, "wb") as f:
        f.truncate(16 * 1024 * 1024)
    x = boot([(img, "ide"), (d2, "ide")], net=None)
    run_cmd(x, "newfs 1", "C-FFS created", name="newfs: XN format, libFS installs types and root")
    run_cmd(x, "mkdir /mnt; mount 1 /mnt; echo on-disk-1 > /mnt/f; cat /mnt/f; sync",
            "on-disk-1", name="mount: second file system")
    shutdown(x)
    ok, msg = fsck(d2)
    report("newfs: file system built in ExOS verifies on the host", ok, msg)


test_main()
test_crash()
test_newfs()
if not QUICK:
    test_config("ahci-e1000e", "ahci", "e1000e", machine="q35")
    test_config("virtio", "virtio", "virtio-net-pci", smp=4)
    test_config("rtl8139-up", "ide", "rtl8139", smp=1)
    test_config("ne2k", "ide", "ne2k_pci")

npass = sum(1 for _, ok in results if ok)
nfail = len(results) - npass
print("\n%d passed, %d failed" % (npass, nfail))
if nfail:
    print("console log of the last run: " + os.path.join(TMP, "console.log"))
else:
    shutil.rmtree(TMP, ignore_errors=True)
sys.exit(1 if nfail else 0)
