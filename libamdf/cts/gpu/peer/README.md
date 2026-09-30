# Physical peer-GPU qualification boundary

This package is reserved for tests whose producer and consumer are different
physical GPUs. Two queues or two device handles for the same endpoint do not
satisfy that condition. There are no native cases in this package yet.

The first case requires two passively selected endpoints, explicit access
attachments for shared backing, a directional pair query, a source-supported
ordering operand, real device-produced changing data and final-use cleanup on
both devices. It records topology and mutually atomic reach independently of
queue operation support. Unsupported attachment or pair-query admission blocks
the cell before commands are submitted.

Its runner must provision and exclusively reserve both endpoints. The ordinary
single-GPU requirement cannot certify their presence or peer topology. A
separate multi-device run requirement and resource reservation belong with the
first executable witness, after the actual caller and available runner shape
are established. The current BUILD exports only this boundary description.
