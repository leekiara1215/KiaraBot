"""Exercise the actual process; no database, service, or fixed port required."""
import selectors
import signal
import subprocess
import sys

binary = sys.argv[1]
for arguments, expected in [(["--check"], 0), (["--help"], 0), (["--unknown"], 2),
                            (["--check", "extra"], 2)]:
    result = subprocess.run([binary, *arguments], capture_output=True, timeout=5)
    if result.returncode != expected:
        raise RuntimeError((arguments, result.returncode, result.stderr))

for stop_signal in (signal.SIGTERM, signal.SIGINT):
    with subprocess.Popen([binary], stdout=subprocess.PIPE, stderr=subprocess.PIPE) as process:
        try:
            with selectors.DefaultSelector() as selector:
                selector.register(process.stdout, selectors.EVENT_READ)
                if not selector.select(timeout=5):
                    raise RuntimeError("Process did not initialize")
                started = process.stdout.readline()
            if b"rpc_ready=false" not in started:
                raise RuntimeError(started)
            try:
                process.wait(timeout=0.1)
            except subprocess.TimeoutExpired:
                pass
            else:
                raise RuntimeError("Process exited before a stop request")
            process.send_signal(stop_signal)
            output, errors = process.communicate(timeout=5)
            if process.returncode != 0 or b"process_stop" not in output or errors:
                raise RuntimeError((process.returncode, output, errors))
        finally:
            if process.poll() is None:
                process.kill()
                process.wait(timeout=5)
print("Process lifecycle, SIGINT/SIGTERM, self-check and argument rejection passed")
