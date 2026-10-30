from pathlib import Path
import subprocess, sys, urllib.request, time

here=Path(__file__).resolve().parent
mock=subprocess.Popen([sys.executable,str(here/'ai_function_mock.py')],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
try:
    for _ in range(50):
        if mock.poll() is not None: raise RuntimeError('Mock fixture failed to start; port 8000 must be free')
        try:
            urllib.request.urlopen('http://127.0.0.1:8000/stats',timeout=1).read(); break
        except OSError: time.sleep(.1)
    else: raise RuntimeError('Mock fixture did not become ready')
    result=subprocess.run([sys.executable,str(here/'ai_function_regression.py')]+sys.argv[1:],text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
    if result.returncode:
        sys.stderr.write(result.stdout)
        raise SystemExit(result.returncode)
finally:
    mock.terminate();mock.wait()
