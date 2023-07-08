#!/usr/bin/env python3
import os
import sys
from ninja_syntax import Writer

IS_DEBUG = True
BUILD_EXAMPLES = True

# process is_debug flags
cflags = ['-Wall', '-Werror']
cppflags = []

if IS_DEBUG:
    cflags += ['-g']
    cppflags += ['-g']
else:
    cflags += ['-O3']
    cppflags += ['-O3']

# process build_examples
dirs = ['lib']
if BUILD_EXAMPLES:
    dirs += ['examples']


ninja = Writer(output=open('build.ninja', 'w'))

ninja.variable('cflags', ' '.join(cflags))
ninja.variable('cppflags', ' '.join(cflags))
ninja.newline()
ninja.variable('cc', 'gcc')
ninja.variable('cxx', 'g++')
ninja.newline()

ninja.rule('cc',
           command='$cc $cflags -c -fPIC $in -o $out',
           description='cc $out',
           )

ninja.rule('cc-shared',
           command='$cc $cflags -shared $in -o $out',
           description='cc $out',
           )

ninja.rule('cxx',
           command='$cxx $cppflags -c $in -o $out',
           description='cxx $out',
           )
ninja.rule('configure',
           command='./$in $out',
           description='configure $out',
           generator=True
           )
ninja.newline()

ninja.build('build.ninja', 'configure', 'configure.py',
            implicit=[f'{d}/build.ninja' for d in dirs])

for d in dirs:
    subconfiguration_file = f'{d}/configure.py'
    subninja_out = f'{d}/build.ninja'

    ninja.build(subninja_out, 'configure', subconfiguration_file)
    if (os.system(f'./{subconfiguration_file} {subninja_out}') != 0):
        print(f'{subconfiguration_file} did not terminate successfully')
        sys.exit(1)

    ninja.subninja(subninja_out)
