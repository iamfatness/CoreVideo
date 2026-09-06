# Requested and received video quality

CoreVideo requests 360p, 720p, or 1080p from Zoom. The output canvas and the
SDK accepting a request do not prove that Zoom delivered that size. Check
**observed dimensions** and frame freshness in diagnostics. A fresh 640×360
feed remains live even when the source asks for 1920×1080.

On macOS, all outputs for one participant share one renderer. The highest
current target request wins when subscribing or recovering, regardless of
whether a tile or fixed source was added first. Adding an HD output raises the
existing renderer in place. A refusal keeps the working renderer and feed.
Removing or rebinding an HD target removes its request from future recovery;
a warm renderer is not downgraded merely because that target left.

The diagnostic `negotiated_resolution` field means the SDK-accepted request,
not observed frame size. A failed resolution request does not overwrite the
last accepted quality. Its SDK result code remains available in diagnostics.

An active source with fresh but smaller frames can make three automatic quality
retry attempts. The first requires at least 20 seconds after subscribing;
subsequent attempts have 120-second and 240-second cooldowns. After exhaustion,
automatic quality retry stops and its countdown disappears. Manual retry remains
available. Receiving the requested dimensions or clearing the subscription
state resets this budget. This policy does not imply that retries can enable
HD unavailable from Zoom.

## Live validation still required

A controlled HD-capable sender and verified meeting/account HD configuration
have not yet been tested against this fix. Compare fixed 360p, 720p, and 1080p
requests for at least 60 seconds each, then Active Speaker and multiple tiles.
Record SDK request results and actual dimensions separately. No claim of
successful live HD delivery, upstream entitlement, or completed soak is made.
