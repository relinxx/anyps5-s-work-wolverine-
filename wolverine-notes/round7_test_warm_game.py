import runpy
import sys
from run_wolverine_standalone import OPTIONS, LAUNCHER, preflight

preflight()
sys.argv = [str(LAUNCHER), *OPTIONS, '--trace-pipeline', '--disk-invocations', '--capture-physical-reserve-mib', '512']
runpy.run_path(str(LAUNCHER), run_name='__main__')
