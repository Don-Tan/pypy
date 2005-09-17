# benchmarks on a windows machine.
# to be executed in the goal folder,
# where a couple of .exe files is expected.

current_result = """
executable           abs.richards   abs.pystone   rel.richards   rel.pystone
pypy-c-17439           35131 ms         664.546      42.1           61.3
pypy-c-17512           45805 ms         682.501      54.9           59.7
pypy-c-17516           37934 ms         727.160      45.5           56.0
pypy-c-17545-intern    34299 ms         795.603      41.1           51.2
pypy-c-17572           36121 ms         785.060      43.3           51.9
pypy-c-17600           26348 ms         932.547      31.6           43.7
pypy-c-17623-32_4      24825 ms        1010.910      29.8           40.3
pypy-c-17634           20069 ms        1018.520      24.1           40.0
python 2.3.3             834 ms       40719.600       1.0            1.0
"""

import os, sys

PYSTONE_CMD = 'from test import pystone;pystone.main(%s)'
PYSTONE_PATTERN = 'This machine benchmarks at'
RICHARDS_CMD = 'from richards import *;Richards.iterations=%d;main()'
RICHARDS_PATTERN = 'Average time for iterations:'

def get_result(txt, pattern):
    for line in txt.split('\n'):
        if line.startswith(pattern):
            break
    else:
        raise ValueError, 'this is no valid output'
    return float(line.split()[len(pattern.split())])

def run_cmd(cmd):
    print "running", cmd
    pipe = os.popen(cmd + ' 2>&1')
    result = pipe.read()
    print "done"
    return result

def run_pystone(executable='python', n=0):
    argstr = PYSTONE_CMD % (str(n) and n or '')
    txt = run_cmd('%s -c "%s"' % (executable, argstr))
    res = get_result(txt, PYSTONE_PATTERN)
    print res
    return res

def run_richards(executable='python', n=10):
    argstr = RICHARDS_CMD % n
    txt = run_cmd('%s -c "%s"' % (executable, argstr))
    res = get_result(txt, RICHARDS_PATTERN)
    print res
    return res

def get_executables():
    exes = [name for name in os.listdir('.') if name.endswith('.exe')]
    exes.sort()
    return exes

LAYOUT = '''
executable           abs.richards   abs.pystone   rel.richards   rel.pystone
pypy-c-17439           40929 ms         637.274      47.8           56.6
pypy-c-17512           46105 ms         658.1        53.9           54.8
pypy-current           33937 ms         698.415      39.6           51.7
python 2.3.3             856 ms       36081.6         1.0            1.0
'''

HEADLINE = '''\
executable           abs.richards   abs.pystone   rel.richards   rel.pystone'''
FMT = '''\
%-20s   '''          +  '%5d ms       %9.3f     ' + '%5.1f          %5.1f'

def main():
    print 'getting the richards reference'
    ref_rich = run_richards()
    print 'getting the pystone reference'
    ref_stone = run_pystone()
    res = []
    for exe in get_executables():
        exename = os.path.splitext(exe)[0]
        res.append( (exename, run_richards(exe, 1), run_pystone(exe, 2000)) )
    res.append( ('python %s' % sys.version.split()[0], ref_rich, ref_stone) )
    print HEADLINE
    for exe, rich, stone in res:
        print FMT % (exe, rich, stone, rich / ref_rich, ref_stone / stone)

if __name__ == '__main__':
    main()
