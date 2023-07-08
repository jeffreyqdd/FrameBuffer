# Buffer

Written for Linux. This library is a frame buffer that relies on a shared memory
system. Each buffer can have multiple readers but only one writer.

I wrote this framework because I needed a way for multiple OpenCV processes / machine 
learning models to run concurrently on a single source. This framework has been isolated
from the original project.

## Installation
Requires a C compiler, python, and ninja.
1. Run `pip3 install -r requirements.txt`
2. Run `./configure.py`
3. Run `ninja`

Build configurations can be changed in the `configure.py` located in the project root.

## Running examples
`PYTHONPATH` should be set to `PROJECT_ROOT/lib/`. `LD_LIBRARY_PATH` should be 
updated to `PROJECT_ROOT/lib/binaries/`. 

You may run `source setpath.sh` to set both of those environment variables automatically.