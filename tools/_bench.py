import sys, time, os
sys.path.insert(0, "tools")
from qemu_session import Machine
from x86_image import build_image

fs = sys.argv[1] if len(sys.argv) > 1 else "ext2"
mib = int(sys.argv[2]) if len(sys.argv) > 2 else 64
image = f"/tmp/opencode/bench-p7-{fs}.img"
if os.path.exists(image):
    os.unlink(image)
rc = build_image(image, "build-x86", "build-host/tools/microos_mkfs", 5120, fs=fs, data_mb=0,
                 extra=["--program", f"/bin/mkbig={os.path.abspath('build-x86/mkbig.mox')}"])
print("build_image", rc)
with Machine("build-x86", image) as m:
    m.open(timeout=180)
    for cmd in [f"exec /bin/mkbig write /big.bin {mib}", f"exec /bin/mkbig verify /big.bin {mib}"]:
        t = time.monotonic()
        ans = m.run(cmd, timeout=1800, hang=1800)
        line = [l for l in ans.splitlines() if "mkbig:" in l]
        print("%.1fs" % (time.monotonic()-t), " | ".join(line))
    txt = m.transcript.decode("utf-8","replace")
    print("\n".join([l for l in txt.splitlines() if "atastat" in l][-1:]))
