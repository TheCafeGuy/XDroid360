# Turnip patches

`tools/turnip/build.sh` builds Mesa's Turnip (`third_party/mesa`, the commit of the phone's driver) plus
these patches into an adrenotools zip. Mesa's MIT license applies (`third_party/mesa/docs/license.rst`);
backports keep their upstream author and hash.

- 0001: Mesa c88b6eb (Connor Abbott, MR 45129, Cc mesa-stable). The bindless descriptor invalidate writes
  SP_CS/GFX_BINDLESS_INVALIDATE; SP_UPDATE_CNTL alone left old descriptors in the cache.
- 0002: XDroid360. The 0001 invalidate once at the start of every command buffer: xenia resets descriptor
  pools after a submission retires and reuses set addresses with new contents, and never takes the
  descriptor buffer flush path that 0001 fixes. TU_DEBUG=oldbindlessinv turns both off (the stock
  sequence on every chip), for A/B on one build.
- 0003: ARMSX2 267e559 (Brian Degenhardt, bmdhacks/armsx2-turnip, MIT), ported by hand. A higher priority
  context (SurfaceFlinger's composition, notifications) preempting a GMEM pass leaves RB_CLEAR_TARGET,
  RB_BUFFER_CNTL and RB_CCU_CACHE_CNTL as that context set them: earlier bins come out black or stale.
  Each write of the three now keeps a copy in memory and a switch-back amble writes them back.
  TU_DEBUG=nopreemptrestore turns it off (read at device creation). One copy per device is enough for
  xenia: Turnip has no transfer-only queue family, so xenia submits on one queue.

TU_DEBUG flags are read when the app creates its Vulkan device: after a change, restart the app (gpubench's
tu_debug= key does, each run starts the app).
