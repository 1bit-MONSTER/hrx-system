# Compiled GPU fixture boundary

This package reserves the shared compiled-program boundary for PM4, AQL and
whole-workflow recipes. It currently contains no program bytes or runtime
loader. The existing command foundation does not claim dispatch coverage.

One small arithmetic program should first prove the public ownership chain:
host initialization, executable/descriptor and kernarg publication, scratch
where required, a real dispatch, exact changing output and final resource
release. Its result oracle is independent of the emitted machine code.

A fixture consists of source, exact target/compiler/link flags, reproducible
generation, compiled image identity, entry/descriptor offsets, resource and
kernarg metadata, and the memory/relocation requirements of that image. Target
variants are separate artifacts. Compiler metadata stays paired with the entry
code, including kernarg-preload and private-segment ABI decisions.

CTS consumers depend on those controlled fixtures, not on an installed shader
compiler or a production ELF-loading subsystem in libamdf. A loader needed for
a fixture handles that fixture's actual representation; the first vertical
slice determines the smallest shared boundary before more kernels accumulate.
The public API continues to accept caller-owned code and resource addresses.
