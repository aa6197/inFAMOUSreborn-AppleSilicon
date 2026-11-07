"""Write the launcher environment ([launch] env in installer/infamous.toml) as
KEY=VALUE lines for the iOS app bundle. On Windows and macOS the launcher sets
these itself; an iOS app has no launcher, so the shell (ios_shell.mm) reads
this file at start-up. Values already in the environment win.

usage: gen_launch_env.py <infamous.toml> <out file>
"""
import sys
import tomllib

with open(sys.argv[1], "rb") as f:
    cfg = tomllib.load(f)
env = cfg["launch"]["env"]
lines = [f"{k}={v}\n" for k, v in env.items()]
out = "".join(lines)
try:
    with open(sys.argv[2]) as f:
        if f.read() == out:
            sys.exit(0)
except FileNotFoundError:
    pass
with open(sys.argv[2], "w") as f:
    f.write(out)
