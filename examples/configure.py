#!/usr/bin/env python3
import sys
from ninja_syntax import Writer

outfile = sys.argv[1]
builddir = f"{outfile.replace('build.ninja', 'binaries')}"


ninja = Writer(output=open(outfile, 'w'))
ninja.variable('builddir', builddir)

ninja.comment('currently no C/Cpp examples to be built')
