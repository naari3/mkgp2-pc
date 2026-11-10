# sdk/

SDK entry points the game calls that Aurora does not provide and that have no
real PC implementation yet.

- One source file per SDK library, named after it: `src/ai.c`, `src/ax.c`,
  `src/exi.c`, ... Declarations come from the SDK headers the game already
  uses; this directory adds no headers for SDK functions.
- Every stub body starts with `MKGP2PC_STUB();` (`include/mkgp2pc/stub.h`).
  The first call of each stub is logged, so a run log lists the SDK calls that
  still need real code.
- A stub that grows a real implementation moves out: device behaviour to
  `port/platform/<family>/`, anything Aurora should own upstream to Aurora.
- Do not stub what Aurora implements (GX, VI, PAD, SI, DVD, CARD, MTX, most of
  OS, THP). If an Aurora function is missing or wrong, fix it in Aurora.
