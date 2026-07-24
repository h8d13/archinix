#!/usr/bin/env python3
# Ad-hoc command runner over the SERIAL=on console: boot a uefi-vm.sh
# mode, run each argv command at the autologin shell, print its output
# with terminal escapes stripped, poweroff. The debug-anything loop for
# scripted/headless use; for a live session just run SERIAL=on
# arch/uefi-vm.sh <mode> in a terminal.
# VM_CMD env swaps the launcher for a raw qemu command line (serial on
# stdio) when no uefi-vm.sh mode fits: direct -kernel boots, scratch
# OVMF images (update-test). <mode> is then ignored, "custom" by
# convention.
# usage: serial-sh.py <mode> <cmd>...
import os, pty, re, select, subprocess, sys, time

REPO = os.path.dirname(os.path.dirname(
	os.path.dirname(os.path.abspath(__file__))))
ESCAPES = re.compile(
	rb"\x1b\].*?(?:\x07|\x1b\\)"	# OSC (incl. systemd 3008 marks)
	rb"|\x1b\[[0-9;?]*[A-Za-z]"	# CSI
	rb"|\r")

# long commands (nixgen-setup: pacman deps + store copy + grub) need
# more than the interactive default
CMD_TIMEOUT = int(os.environ.get("SERIAL_CMD_TIMEOUT", "60"))
mode, cmds = sys.argv[1], sys.argv[2:]
master, slave = pty.openpty()
env = dict(os.environ, SERIAL="on")
# exec in the sh -c wrapper so p.kill() reaches qemu itself
launch = ["sh", "-c", "exec " + os.environ["VM_CMD"]] \
	if os.environ.get("VM_CMD") else ["arch/uefi-vm.sh", mode]
p = subprocess.Popen(launch, cwd=REPO,
	stdin=slave, stdout=slave, stderr=slave, env=env, close_fds=True)
os.close(slave)
buf = b""

def wait_for(pat, timeout):
	global buf
	deadline = time.time() + timeout
	while time.time() < deadline:
		if p.poll() is not None:
			return False
		r, _, _ = select.select([master], [], [], 1)
		if master in r:
			try:
				d = os.read(master, 65536)
			except OSError:
				return False
			buf += d
			if pat in buf:
				return True
	return False

# a failure dies with the console tail in hand, not just the verdict
def fail(why):
	print(f"FAIL: {why}")
	tail = ESCAPES.sub(b"", buf).decode(errors="replace")
	print(tail[-4000:])
	p.kill()
	sys.exit(1)

if not wait_for(b"NIXARCH BOOT OK", 240):
	fail("no boot marker")
wait_for(b"]# ", 10)		# first prompt after the banner
for cmd in cmds:
	buf = b""
	os.write(master, cmd.encode() + b"\r")
	# "]# " = end of "[root@nixarch ~]# "; bare "# " false-matches
	# things like getfacl's "# file:" header lines
	if not wait_for(b"]# ", CMD_TIMEOUT):
		fail(f"no prompt after: {cmd}")
	out = ESCAPES.sub(b"", buf).decode(errors="replace")
	# drop the echoed command line and the trailing prompt
	lines = out.splitlines()
	body = [l for l in lines[1:] if not l.endswith("]# ")]
	print(f"$ {cmd}")
	print("\n".join(body).rstrip())
	print()
os.write(master, b"poweroff\r")
p.wait(timeout=120)
print(f"qemu exited rc={p.returncode}")
