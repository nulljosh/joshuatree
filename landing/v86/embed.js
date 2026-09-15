// Real, live, in-browser boot of this exact kernel.elf, not a recording.
// v86 (x86-to-wasm emulator, https://github.com/copy/v86) does the actual
// CPU/device emulation; the wiring below is specific to this kernel:
//
// - Keyboard capture is gated behind a real click into the screen, not on
//   by default. v86's own keyboard adapter listens on `window` globally,
//   which means it picks up ANY keydown on the page, typing in an address
//   bar, scrolling with space, anything, as if it were meant for the
//   emulator. Every real v86 embed handles this the same way: leave it off
//   until the visitor has actually clicked in.
// - The kernel itself boots straight into its GUI desktop now (kmain calls
//   gui_run() before ever reaching the text shell loop), so this file no
//   longer needs to script typing "gui" itself, that used to live here
//   before the kernel grew that behavior on its own.
(function () {
  var container = document.getElementById("v86-embed");
  if (!container) return;

  // The actual screen box, not the outer wrapper: v86's ScreenAdapter does
  // a getElementsByTagName search *inside whatever container it's given*
  // for its first div/canvas to reuse. Handing it the outer #v86-embed
  // wrapper (which also contains #v86-overlay) let it walk into the wrong
  // descendants; handing it #screen_container directly keeps that search
  // scoped to exactly our #screen_text div and #screen_canvas canvas.
  // v42: the kernel's cursor and layout live in this LOGICAL space; the
  // physical mode is 2x this. Keep in sync with gui_run's window_open_scaled.
  var LOGICAL_W = 960, LOGICAL_H = 540;
  // v52.6: real shadow cursor position, kept in sync by every real send
  // this file makes (mousemove, touchmove drags, and moveCursorTo's own
  // taps below), starting at gui_run's own literal initial (400,300).
  // See moveCursorTo's own comment for why this replaced homing-every-tap.
  var trackedKx = 400, trackedKy = 300;
  function trackCursorDelta(dx, dy) {
    trackedKx = Math.max(0, Math.min(LOGICAL_W - 1, trackedKx + dx));
    trackedKy = Math.max(0, Math.min(LOGICAL_H - 1, trackedKy + dy));
  }
  // v62: absolute pointer. The kernel now speaks the VMware absolute-mouse
  // backdoor (drivers/vmmouse.c), the same protocol v86 implements in
  // src/vmware.js (read in the vendored libv86.js, not assumed): once the
  // guest enables it, v86 fires "vmware-absolute-mouse" true on the bus,
  // and from then on a "mouse-absolute" [x, y, w, h] send puts the guest's
  // pointer at exactly x/w, y/h of the screen in one packet, no relative
  // walk, no shadow-cursor bookkeeping, no drift to insure against. The
  // shadow tracking above stays only as the fallback for a kernel that
  // never advertises it (an older kernel.elf, or the backdoor probe
  // failing), so nothing here is worse than before if that ever happens.
  var absoluteMouse = false; // set by the bus listener registered right after `new V86()` below, the emulator doesn't exist yet up here
  function sendAbsolute(kx, ky) {
    kx = Math.max(0, Math.min(LOGICAL_W - 1, Math.round(kx)));
    ky = Math.max(0, Math.min(LOGICAL_H - 1, Math.round(ky)));
    // Pixel CENTRE on the wire: v86 rounds x/w to 0..65535 and the kernel
    // floors (v * w) >> 16 back to a pixel, and only a centre survives
    // both roundings landing on exactly this pixel, never the one before.
    emulator.bus.send("mouse-absolute", [kx + 0.5, ky + 0.5, LOGICAL_W, LOGICAL_H]);
    trackedKx = kx; trackedKy = ky; // keep the fallback's shadow honest too, in case the mode ever flips back
  }
  // A client-space point (a touch or a mouse position) to LOGICAL kernel
  // pixels, measured against the canvas's own live box (see resizeCanvas
  // below for why the canvas, never the container).
  function clientToKernel(cx, cy) {
    var r = screenCanvas.getBoundingClientRect();
    return [(cx - r.left) / r.width * LOGICAL_W, (cy - r.top) / r.height * LOGICAL_H];
  }
  // The kernel's own serial log, captured from the first byte so a QA
  // script (mobiletest.mjs) can read what the guest actually reported,
  // e.g. whether the backdoor probe found v86's vmmouse and whether an
  // absolute packet really arrived, not just whether the page sent one.
  var serialLog = "";
  var screenContainer = document.getElementById("screen_container");
  var screenText = document.getElementById("screen_text");
  var screenCanvas = document.getElementById("screen_canvas");
  var overlay = document.getElementById("v86-overlay");

  var emulator = new V86({
    wasm_path: "v86/v86.wasm",
    memory_size: 32 * 1024 * 1024,
    vga_memory_size: 16 * 1024 * 1024, // v41: 1600x1200x32bpp is 7.68MB, 8 was one bad rounding away from failing
    screen_container: screenContainer,
    multiboot: { url: "v86/kernel.elf" },
    autostart: true,
  });

  // keyboard_adapter/mouse_adapter aren't attached synchronously: V86's
  // constructor kicks off the wasm load and only wires them up once the
  // CPU is actually built, so touching them right after `new V86()` throws.
  // "emulator-ready" fires once they exist.
  var adaptersReady = false;
  emulator.add_listener("emulator-ready", function () {
    adaptersReady = true;
    // Keyboard stays disabled until the visitor clicks in; v86 listens
    // globally on `window`, so this is the only thing standing between
    // "typing in the search bar" and "typing into someone else's kernel".
    emulator.keyboard_adapter.emu_enabled = false;
    emulator.mouse_adapter.emu_enabled = false;
  });
  // v62: both registered here, after the constructor, because add_listener
  // is the emulator's own bus. Registering before "emulator-ready" is fine
  // and necessary: the serial log starts at the first boot byte, and the
  // kernel enables the backdoor a few ms into boot, long before ready.
  emulator.add_listener("vmware-absolute-mouse", function (on) { absoluteMouse = !!on; });
  emulator.add_listener("serial0-output-byte", function (b) {
    if (serialLog.length < 65536) serialLog += String.fromCharCode(b);
  });

  var focused = false;
  function focusIn() {
    if (focused || !adaptersReady) return;
    focused = true;
    emulator.keyboard_adapter.emu_enabled = true;
    emulator.mouse_adapter.emu_enabled = true;
    if (overlay) overlay.classList.add("hidden");
    stopAutoplay();
  }
  container.addEventListener("mousedown", focusIn);
  container.addEventListener("touchstart", focusIn, { passive: true });
  container.addEventListener("keydown", focusIn);

  // v52: the demo now lives behind the hero text (direct request). This
  // toggle is purely visual, separate from `focused` above on purpose:
  // `focused` is one-way, once a real visitor has taken control the idle
  // tour must never restart, so it can't double as "is the pointer
  // currently inside the demo" for showing/hiding the headline, that
  // needs to flip back and forth freely as someone clicks in and out.
  var heroEl = document.querySelector(".hero");
  var heroCopyEl = document.querySelector(".hero-copy");
  if (heroEl) {
    var showDemo = function () { heroEl.classList.add("demo-focused"); };
    var showText = function () { heroEl.classList.remove("demo-focused"); };
    container.addEventListener("mousedown", showDemo);
    container.addEventListener("touchstart", showDemo, { passive: true });
    // The frosted card sits visually on top of the canvas (z-index above
    // it) but is a separate element, so a click on it never reaches
    // #v86-embed at all, confirmed live: a click square in the middle of
    // the card did nothing until this was added. Its own listener both
    // hides the card AND stops the click there, a real, deliberate two-
    // step interaction (dismiss, then click again on the now-revealed
    // demo) rather than forwarding the same click into the kernel, which
    // would fire whatever dock icon happened to be under the card.
    if (heroCopyEl) {
      heroCopyEl.addEventListener("mousedown", function (ev) { showDemo(); ev.stopPropagation(); });
      heroCopyEl.addEventListener("touchstart", function (ev) { showDemo(); ev.stopPropagation(); }, { passive: true });
    }
    document.addEventListener("mousedown", function (ev) { if (!container.contains(ev.target)) showText(); });
    document.addEventListener("touchstart", function (ev) { if (!container.contains(ev.target)) showText(); }, { passive: true });
    document.addEventListener("keydown", function (ev) { if (ev.key === "Escape") showText(); });
  }

  // A real QA hook, not debug scaffolding left behind: "apps don't open on
  // mobile" got reported and 'fixed' several times while every check was a
  // human squinting at a phone, because nothing here is observable from
  // outside this closure. mobiletest.mjs drives a real iPhone emulation
  // against the real page and needs to see whether input is actually armed
  // to tell "the tap missed" apart from "input was never enabled".
  window.__jt = {
    emu: emulator, /* mobiletest.mjs reads the kernel's own serial log through this: the guest's klog output is the only view into what the kernel actually thinks happened */
    get ready() { return adaptersReady; },
    get focused() { return focused; },
    get mouseOn() { return !!(emulator.mouse_adapter && emulator.mouse_adapter.emu_enabled); },
    get absolute() { return absoluteMouse; }, /* v62: did the kernel enable v86's vmmouse backdoor */
    get serial() { return serialLog; },
    click: function () {
      emulator.bus.send("mouse-click", [true, false, false]);
      setTimeout(function () { emulator.bus.send("mouse-click", [false, false, false]); }, 60);
    },
    move: function (dx, dy) {
      if (absoluteMouse) { sendAbsolute(trackedKx + dx, trackedKy + dy); return; }
      emulator.bus.send("mouse-delta", [dx, -dy]);
    },
    moveTo: function (kx, ky) { sendAbsolute(kx, ky); }
  };

  // Real bug, reported directly ("the cursor is really misplaced... ten
  // or twenty pixels off") and independently confirmed while testing: CSS
  // `object-fit: contain` on a canvas sized to 100%/100% of its container
  // only changes what's *visually drawn*, the canvas element's own DOM
  // box (what getBoundingClientRect reports, what mouse coordinates get
  // measured against) still spans the full, un-letterboxed container.
  // Any click or mouse move gets measured against the wrong box the
  // instant the visible image and the DOM box disagree, which is
  // whenever the viewport's aspect ratio isn't exactly 800x600, i.e.
  // almost always, and worst on a portrait phone. Setting the canvas
  // element's actual CSS size to the real dimensions it's drawn at, not
  // relying on object-fit at all, makes the DOM box and the visible
  // image the same rectangle, so coordinate math anywhere downstream
  // (this file or v86's own mouse adapter) is correct by construction.
  //
  // v52.2: cover, not contain, direct request to close the remaining
  // real letterbox once the demo became the hero's own background layer.
  // Every downstream consumer of "where did this click/touch land"
  // (touchmove, touchend, mousemove below) reads the canvas's own live
  // getBoundingClientRect(), not the container, so this stays correct by
  // the same construction as before regardless of which scale wins:
  // whatever box the canvas actually ends up, cropped or not, is still
  // the exact rectangle clicks get measured against, nothing downstream
  // needed to change. #screen_canvas gets flex-shrink:0 so flex centering
  // never quietly shrinks it back down and undoes whichever fit was
  // chosen.
  //
  // v52.9: real regression caught live and fixed same day, not a second
  // guess: unconditional cover cropped the menu bar's own clock/weather
  // clean off the right edge on a real narrow phone screenshot, a much
  // worse bug than the letterbox it replaced (illegible UI beats a black
  // margin every time). Cover and contain are fundamentally at odds on an
  // extreme aspect ratio (a 16:9 source in a much-taller-than-wide box
  // has to either crop real content or leave real dead space, no CSS
  // trick escapes that math), so this now picks per viewport instead of
  // one global rule: cover only when it would still show at least 75% of
  // the source on its cropped axis (true for a laptop/desktop window and
  // most landscape-ish phones), contain otherwise (a real tall portrait
  // phone), so a visitor never loses real UI to a crop just to avoid a
  // margin.
  var currentScale = 1;
  function resizeCanvas() {
    if (!screenCanvas) return;
    var box = screenContainer.getBoundingClientRect();
    var w = screenCanvas.width || 800, h = screenCanvas.height || 600;
    var coverScale = Math.max(box.width / w, box.height / h) || 1;
    var containScale = Math.min(box.width / w, box.height / h) || 1;
    var visibleFrac = Math.min(box.width / (w * coverScale), box.height / (h * coverScale));
    currentScale = visibleFrac >= 0.75 ? coverScale : containScale;
    screenCanvas.style.width = Math.round(w * currentScale) + "px";
    screenCanvas.style.height = Math.round(h * currentScale) + "px";
  }
  window.addEventListener("resize", resizeCanvas);
  window.addEventListener("orientationchange", resizeCanvas);
  resizeCanvas();

  // A second, real bug behind the same "cursor is misplaced" report, found
  // after the letterbox-vs-DOM-box fix above still didn't fully fix it:
  // a real browser mousemove's movementX/movementY are always reported in
  // CSS pixels of actual physical mouse travel, completely unaware of any
  // CSS sizing this page applies. v86's own mouse adapter forwards that
  // raw value straight into the kernel's relative PS/2-style cursor with
  // no scale compensation at all (confirmed by reading its source: no
  // division by any scale factor anywhere in that path), and neither does
  // v86's own `set_scale`/CSS-transform display mechanism, so switching to
  // it instead wouldn't have helped either. Displaying the kernel's real
  // 800x600 output any size other than exactly 800x600 CSS pixels means
  // real mouse travel and the kernel's own cursor travel at different
  // rates, drifting apart the more the visitor actually moves the mouse,
  // exactly the reported symptom. Fixed by intercepting real movement
  // ourselves in the capture phase (before v86's own bubble-phase listener
  // ever sees it), scaling it down to real kernel pixels, and sending the
  // corrected delta over the same bus event v86's own adapter uses,
  // `stopImmediatePropagation` so v86 never also sends the raw, wrong one.
  screenContainer.addEventListener("mousemove", function (ev) {
    if (!focused || !emulator.mouse_adapter || !emulator.mouse_adapter.emu_enabled) return;
    if (document.pointerLockElement) return; // pointer-locked play (a real click-drag drag) already reports device-independent deltas v86 handles correctly on its own
    if (absoluteMouse) {
      // v62: the kernel's pointer sits exactly under the real one, so
      // scale drift can't exist by construction; v86's own adapter would
      // send the same event measured against the CONTAINER box, which is
      // the wrong rectangle whenever cover/contain has resized the canvas.
      var kp = clientToKernel(ev.clientX, ev.clientY);
      sendAbsolute(kp[0], kp[1]);
      ev.stopImmediatePropagation();
      return;
    }
    var lscale = screenCanvas.getBoundingClientRect().width / LOGICAL_W; // CSS px per LOGICAL kernel px (v41: canvas is 1600 physical, cursor is 800 logical)
    var dx = ev.movementX / lscale, dy = ev.movementY / lscale;
    emulator.bus.send("mouse-delta", [dx, -dy]); // y inverted, matching v86's own convention exactly
    trackCursorDelta(dx, dy); // v52.6: keep the tap-to-click shadow position in sync with real mouse drags too, not just its own sends
    ev.stopImmediatePropagation();
  }, true);

  // Touch has no movementX/Y at all, v86 computes its own delta from
  // consecutive touch positions (clientX/Y), which has the exact same
  // unscaled-pixel problem as mouse movementX/Y above, worse on a phone
  // where the display scale is usually larger. Same fix, our own tracked
  // last-touch position instead of the browser-provided movement value.
  //
  // Real, reported bug this used to have: the hero is full viewport height
  // (100svh), and this used to call preventDefault() on every touchmove
  // once focused, which is the browser's own scroll gesture, trapping a
  // mobile visitor with no way to scroll past it at all once they'd
  // tapped in even once. Never calling preventDefault() here means the
  // page can still scroll normally during a touch-drag inside the kernel
  // view, at the cost of that drag not being quite as clean (the page
  // may scroll a little at the same time), a real, deliberate trade-off:
  // never trapping a visitor matters more than perfectly smooth touch
  // control, which wasn't the primary way to use this anyway.
  var lastTouchX = null, lastTouchY = null;
  screenContainer.addEventListener("touchmove", function (ev) {
    if (!focused || !emulator.mouse_adapter || !emulator.mouse_adapter.emu_enabled) return;
    var t = ev.changedTouches && ev.changedTouches[ev.changedTouches.length - 1];
    if (!t) return;
    if (absoluteMouse) {
      // v62: a finger drag steers the pointer to wherever the finger IS,
      // not by how far it moved, so a drag can never lag or overshoot.
      var kp = clientToKernel(t.clientX, t.clientY);
      sendAbsolute(kp[0], kp[1]);
      lastTouchX = t.clientX; lastTouchY = t.clientY;
      ev.stopImmediatePropagation();
      return;
    }
    if (lastTouchX !== null) {
      var lscale = screenCanvas.getBoundingClientRect().width / LOGICAL_W;
      var dx = (t.clientX - lastTouchX) / lscale, dy = (t.clientY - lastTouchY) / lscale;
      emulator.bus.send("mouse-delta", [dx, -dy]);
      trackCursorDelta(dx, dy);
    }
    lastTouchX = t.clientX; lastTouchY = t.clientY;
    ev.stopImmediatePropagation();
  }, { capture: true, passive: true });
  screenContainer.addEventListener("touchstart", function (ev) {
    var t = ev.changedTouches && ev.changedTouches[0];
    if (t) { lastTouchX = t.clientX; lastTouchY = t.clientY; tapStartX = t.clientX; tapStartY = t.clientY; tapStartT = Date.now(); }
  }, { capture: true, passive: true });

  // Real tap-to-click. Before this, touch could only ever *move* the
  // kernel's cursor: no mouse button was sent on a tap at all, so a phone
  // visitor could push the pointer around the desktop and never once open
  // an app. The demo was, in practice, look-only on mobile.
  //
  // A tap also has to land where the finger actually is, which a relative
  // PS/2-style cursor can't do by itself: it only understands deltas, and
  // nothing on this page knows where the kernel currently thinks its
  // cursor is. So we track it: the kernel starts its cursor at (400,300)
  // (gui_run's own initial mx/my) and clamps to the 800x600 screen, so
  // mirroring both here keeps a shadow copy that stays in step with every
  // delta we send. A tap then becomes "move by the difference, then
  // click", which lands on the icon under the finger.
  // A PS/2 packet carries a 9-bit signed delta, so anything bigger than
  // ~255 has to go as several packets or the emulator clamps it and the
  // cursor lands somewhere else entirely. Found the hard way: a single
  // "jump 600px" send arrived as -256 and the cursor stuck to an edge.
  function splitDelta(dx, dy, out) {
    var step = 200;
    while (Math.abs(dx) > 0.5 || Math.abs(dy) > 0.5) {
      var px = Math.max(-step, Math.min(step, dx));
      var py = Math.max(-step, Math.min(step, dy));
      out.push([px, -py]); // y inverted, v86's convention
      dx -= px; dy -= py;
    }
    return out;
  }

  // Packets have to be *paced*, not fired in a burst. The guest reads one
  // PS/2 packet per IRQ12, and firing ten at once overruns that queue: the
  // tail is simply dropped, which showed up as the cursor homing to the
  // corner correctly and then never travelling to the target at all,
  // caught by screenshotting the real emulator mid-tap rather than
  // trusting that "the sends all returned". One packet per frame is still
  // far faster than any human moves a mouse.
  function sendPaced(packets, done) {
    var i = 0;
    (function step() {
      if (i >= packets.length) { if (done) done(); return; }
      emulator.bus.send("mouse-delta", packets[i++]);
      setTimeout(step, 16);
    })();
  }

  // Tapping a specific icon means putting a *relative* cursor at an
  // absolute place, which nothing on this page can do by asking: the
  // kernel never reports where its cursor is. The first attempt tracked a
  // shadow copy in JS, which desynced the moment anything else moved the
  // cursor (v86's own touch handlers do exactly that) and then every tap
  // landed somewhere wrong, confirmed by reading the kernel's real serial
  // log: the cursor had been slammed to the bottom edge while the shadow
  // still believed it was centred. The fix at the time was homing to a
  // known corner before every single tap, self-correcting, no state to
  // drift, at the real cost of a visible corner-to-target dart on every
  // tap, still reported directly ("house jumps around, glitch") even
  // after the click itself landed correctly.
  //
  // v52.6: back to shadow tracking, but for real this time: the drift
  // source above was v86's own internal touch listeners double-moving
  // the cursor behind our back, and that path is now actually closed
  // (touchend's own `ev.stopImmediatePropagation()`, added since), not
  // just assumed fixed. `trackedKx/Ky` mirrors the kernel's real cursor
  // (starts at gui_run's own literal 400,300, the same value the old
  // homing comment already named) and updates after every send this file
  // makes, both taps and drags, so it can't fall out of sync with
  // anything WE do.
  //
  // Drift insurance, made smarter after a direct follow-up ("work on
  // drift insurance") rather than just tightened: the first version
  // forced a real re-home (the visible corner-dart this whole change
  // exists to avoid) every 8th call regardless of caller, which meant a
  // real visitor tapping around could still hit that jarring resync on
  // an ordinary tap, exactly the bug being fixed. Real taps (`allowResync`
  // left false, the default) now NEVER force one, full stop, zero risk of
  // the glitch for an actual visitor. Periodic resyncs still happen, just
  // only from the idle tour's own calls (`allowResync: true`), where
  // cursor motion is already the point of what's on screen, a resync
  // dart there reads as part of the demo, not a bug in it.
  //
  // v62: with the absolute backdoor live none of the above applies. One
  // "mouse-absolute" send IS the position; the only wait left is for the
  // guest to poll it (the kernel polls the backdoor from its GUI loop,
  // woken at worst every 10ms by its own 100Hz timer, since v86 raises no
  // IRQ for an absolute move), so `done` fires after two frames instead
  // of after a paced multi-packet walk. The paced relative path below is
  // untouched and still runs when the kernel never advertised support.
  var toursSinceResync = 0;
  function moveCursorTo(kx, ky, done, allowResync) {
    kx = Math.max(0, Math.min(LOGICAL_W - 1, kx));
    ky = Math.max(0, Math.min(LOGICAL_H - 1, ky));
    if (absoluteMouse) {
      sendAbsolute(kx, ky);
      if (done) setTimeout(done, 32);
      return;
    }
    var packets = [];
    var forceResync = allowResync && (toursSinceResync >= 4);
    if (forceResync) {
      splitDelta(-(LOGICAL_W + 400), -(LOGICAL_H + 400), packets); // clamps to (0,0) from anywhere on screen
      splitDelta(kx, ky, packets);
      toursSinceResync = 0;
    } else {
      splitDelta(kx - trackedKx, ky - trackedKy, packets);
      if (allowResync) toursSinceResync++;
    }
    trackedKx = kx; trackedKy = ky;
    sendPaced(packets, done);
  }
  var tapStartX = 0, tapStartY = 0, tapStartT = 0;
  screenContainer.addEventListener("touchend", function (ev) {
    lastTouchX = lastTouchY = null;
    if (!focused || !emulator.mouse_adapter || !emulator.mouse_adapter.emu_enabled) return;
    var t = ev.changedTouches && ev.changedTouches[0];
    if (!t) return;
    ev.stopImmediatePropagation(); // v86 attaches its own touch listeners and injects a second, unscaled delta otherwise, which is what desynced taps before
    var moved = Math.abs(t.clientX - tapStartX) + Math.abs(t.clientY - tapStartY);
    // A tap, not a drag: a short press that barely moved. Drags are the
    // cursor-steering gesture above and must not also fire a click.
    if (moved > 12 || Date.now() - tapStartT > 500) return;
    // v62: a browser follows a tap with synthesized mousedown/mouseup
    // (touch compatibility events), and v86's own adapter listens for
    // those on `window` and turns them into a SECOND click, at the same
    // instant as the tap. Caught in the kernel's own serial trace under
    // mobiletest.mjs: every tap arrived as two press/release pairs. On
    // the old path the extra pair was (accidentally) always lost to the
    // kernel reading both packets in one poll; with the driver no longer
    // losing clicks, it would open an app and immediately close it.
    // preventDefault on touchend is the standard way to suppress those
    // compatibility events, and it never affects scrolling (that's
    // touchstart/touchmove, deliberately left passive above). Only for a
    // handled tap: drags return above without touching the default.
    ev.preventDefault();
    // v41: the kernel's cursor lives in LOGICAL 800x600 coordinates no
    // matter what physical mode it opened (it's 1600x1200 now, drawn 2x),
    // so map by fraction of the canvas box, not by physical pixels.
    var rect = screenCanvas.getBoundingClientRect();
    var kx = (t.clientX - rect.left) / rect.width * LOGICAL_W;
    var ky = (t.clientY - rect.top) / rect.height * LOGICAL_H;
    if (kx < 0 || ky < 0 || kx > LOGICAL_W - 1 || ky > LOGICAL_H - 1) return;
    // Click only once the cursor has actually finished travelling: the
    // movement is paced across several frames now, and clicking before it
    // lands means clicking wherever it happens to be partway there.
    moveCursorTo(Math.round(kx), Math.round(ky), function () {
      setTimeout(function () {
        // Down then up, with a real gap: the kernel polls the mouse from
        // its own loop, so a press and release inside one poll can be
        // missed entirely.
        emulator.bus.send("mouse-click", [true, false, false]);
        setTimeout(function () { emulator.bus.send("mouse-click", [false, false, false]); }, 80);
      }, absoluteMouse ? 40 : 120); // v62: nothing is still travelling in absolute mode, only the guest's next poll to wait for
    });
  }, { capture: true, passive: false }); // v62: passive:false so the tap's preventDefault above is honoured (touchend has no scroll to block)

  // Toggle between the text and graphical screen elements: v86 keeps both
  // in the DOM and expects the embedder to show whichever is active. The
  // real boot banner is genuine and correct, but it isn't the demo, so it
  // stays hidden through the whole boot-to-gui transition rather than
  // flashing on screen for the few hundred ms that takes; it only ever
  // surfaces as a fallback if graphical mode genuinely never arrives
  // (auto-gui failed for some reason), not as the default path.
  var bootStart = Date.now();
  var bootLogo = document.getElementById("boot-logo");
  setInterval(function () {
    var vga = emulator.v86 && emulator.v86.cpu.devices.vga;
    if (!vga) return;
    var graphical = !!vga.graphical_mode;
    var stuckInText = !graphical && Date.now() - bootStart > 4000;
    if (screenCanvas) screenCanvas.style.display = graphical ? "block" : "none";
    if (screenText) screenText.style.display = graphical ? "none" : (stuckInText ? "block" : "none");
    // Same real signal the line above already uses, no new detection:
    // once the kernel's own framebuffer is actually up, the boot logo's
    // job is done.
    if (bootLogo && graphical) bootLogo.hidden = true;
    resizeCanvas(); // the canvas's real pixel resolution only exists once graphical mode sets it, re-check every tick until it does
  }, 200);

  // v44: the idle tour. Left alone for a few seconds, the demo shows
  // itself off: the cursor glides to a dock app, opens it, lets it sit,
  // closes it, moves on. Every step goes through the exact same bus path
  // a real tap uses (moveCursorTo + a click), so the tour is proof the
  // real input path works, not a separate animation that could drift from
  // it. The instant a visitor clicks, taps or types, focusIn() sets
  // `focused` and the tour stops between steps and never restarts.
  //
  // v71: rebuilt around a direct request ("cycle through every real app,
  // open it, use it for ~15s, close it from its own X, open the next,
  // then restart"), the honest version of "show off every app" now that
  // roadmap.md's "Multi-window, honestly scoped" entry confirms real
  // window stacking does not exist yet: every app here is still a single,
  // full takeover, and the dock stays live only to switch which ONE app
  // is showing (v69), never to stack a second one on top. Sequential
  // open/use/close/next is the real, buildable shape of that, not a
  // simulated multi-window fake this kernel can't actually do.
  //
  // Only the 8 apps really pinned to the dock (GUI_DOCK_DEFAULT in
  // kernel.c: Files, Mail, Calendar, Notes, Reminders, Terminal, Chat,
  // Weather) are toured. Every one of them gets real window chrome,
  // including a real closable X, from gui_launch_from_dock. The rest of
  // the real app roster (Contacts, Calculator, and 9 ported fleet-app
  // viewers, 11 apps as of v70) lives behind the Apps-folder launchpad
  // tile instead, and gui_launch_apps() opens those with gui_launch()
  // directly, not gui_launch_from_dock, so they run full-screen with NO
  // window chrome and no X at all, confirmed by reading kernel.c, not
  // assumed. Scripting a close click for a button that doesn't exist
  // there would misrepresent what this kernel can do, so those 11 stay
  // out of the tour; see roadmap.md for the real trace.
  //
  // v71 real bug found and fixed here, not glossed over: the OLD tour
  // always sent its "close" click at the exact dock-tile position it had
  // just opened the app from, since it never moved the cursor between
  // open and close. That used to just return to the desktop; since v69
  // shipped (a click that lands on a dock tile while an app is open
  // switches straight to that tile's app instead of returning to a
  // neutral desktop), that same close click now instantly REOPENS the
  // app that was just closed, and the tour only actually moved on once
  // the NEXT step's own "open" click landed on the following app's tile
  // and got read as ITS close. Confirmed live against the deployed site
  // with no interaction injected at all (the real idle tour left running
  // 150s, about 3 full loops, `tourwatch-qa.mjs`, a throwaway QA script
  // built for this pass): every single transition showed the just-closed
  // app flash back open for about 1.9s before the real switch landed
  // (e.g. a full Files -> Terminal handoff took until t=14.2s across
  // four separate screen states, t=7.5s open / t=12.0s close / t=12.5s
  // Files reopens itself / t=14.2s Terminal finally opens, instead of a
  // clean two-step open-then-open). Harmless in that it never got stuck
  // (v69's tail-jump always resolves it eventually, and mouseOn/focused
  // state stayed healthy the whole 150s run, no freeze, no page error),
  // but it doubled the real work every transition did and put a visible
  // glitch-flash in what's supposed to read as a clean demo. Closing via
  // the fixed CLOSE_X/CLOSE_Y position below instead of the dock tile
  // fixes it as a side effect, not a separate patch: that position is
  // never inside gui_dock_hit_test's own rect (checked directly against
  // its real bounds in kernel.c), so the close click genuinely reaches
  // open desktop every time and v69's tail-jump never fires during the
  // tour at all.
  //
  // Every app in this kernel closes on ANY click while it's open,
  // regardless of where it lands (gui_wait_close / get_key_or_click's
  // shared "click closes" contract, read directly out of kernel.c for
  // Files/Mail/Calendar/Notes/Reminders/Terminal/Chat/Weather, not
  // assumed), so a real per-app interaction here can never send a mouse
  // click mid-dwell: it would close the app instantly. Real interaction
  // is keyboard-only below, using exactly the keys each app's own loop
  // actually reads (confirmed per app in kernel.c/mail.h/calendar.h/
  // reminders.h before scripting it, not guessed): typed text for
  // Notes/Chat/Terminal (unchanged since v51), 'c' + three Enter-
  // confirmed fields for Mail's real compose flow, 'd'/']'/Enter for
  // Calendar's real month/day navigation and event-add (a real day
  // still can't be CLICKED in this kernel, there is no per-cell hit test
  // in gui_launch_calendar, only keys move the day cursor; a click there
  // closes Calendar same as everywhere else, so despite how this was
  // first phrased, driving it by click would just close it before it
  // could show anything), and 'a' + an Enter-confirmed line for
  // Reminders' real add flow. Files and Weather have no interactive
  // affordance in this kernel at all beyond gui_wait_close (confirmed
  // the same way), so they get a real dwell with nothing scripted,
  // exactly as honest as scripting a fake interaction would be dishonest.
  // v0.71.3: trimmed from all 8 dock apps to 3, direct request ("the
  // autoplay demo should just be opening apps like one or two, or maybe
  // three apps, and then closing them, and when they're open, just
  // interact with them a little bit"). The full 8-app cycle ran forever
  // rather than reading as a quick, repeating sample; Files (no script,
  // a real dwell), Mail (real compose keystrokes) and Calendar (real
  // month/day navigation) stay, giving one no-interaction app and two
  // scripted ones inside a short loop instead of an 8-stop marathon.
  var TOUR_APPS = [
    { name: 'Files', slot: 1 },
    { name: 'Mail', slot: 2, script: [
      { type: 'keys', text: 'c', speed: 200 },
      { type: 'wait', ms: 500 },
      { type: 'keys', text: 'demo@joshuatree.os\n', speed: 55 },
      { type: 'wait', ms: 350 },
      { type: 'keys', text: 'A real OS, from scratch.\n', speed: 55 },
      { type: 'wait', ms: 350 },
      { type: 'keys', text: 'Every field here really writes to disk.\n', speed: 55 }
    ] },
    { name: 'Calendar', slot: 3, script: [
      { type: 'keys', text: 'dd', speed: 400 }, // step forward two months, a real render each time
      { type: 'wait', ms: 400 },
      { type: 'keys', text: ']]', speed: 400 }, // move the day cursor
      { type: 'wait', ms: 400 },
      { type: 'keys', text: '\n', speed: 200 }, // opens cal_day_view for the selected day
      { type: 'wait', ms: 500 },
      { type: 'keys', text: 'Shipped by an AI, for real.\n', speed: 55 } // saves and returns to the month view
    ] }
  ];
  // gui_launch_from_dock's own fixed traffic-light X (x=70,y=40 non-apps-
  // folder window, red circle at x+24,y+16), the same LOGICAL coordinate
  // mobiletest.mjs already taps for its Notes-close check, identical for
  // every dock-launched app since that rect never depends on which icon
  // opened it. Never inside the dock's own hit-test rect (kernel.c's
  // gui_dock_hit_test bounds its y range to the dock band near the
  // bottom of a 540px-tall logical screen; 56 is nowhere near it), which
  // is exactly what makes it the real fix for the v69 interaction above.
  var CLOSE_X = 94, CLOSE_Y = 56;
  var DWELL_MS = 15000; // "about 15 seconds" per app, direct request
  var tourTimer = 0, tourRunning = false, tourGen = 0;
  function stopAutoplay() { if (tourTimer) { clearTimeout(tourTimer); tourTimer = 0; } tourRunning = false; tourGen++; /* invalidates any in-flight tourLoop */ }
  function sleep(ms) { return new Promise(function (res) { tourTimer = setTimeout(res, ms); }); }
  function moveCursorToAsync(kx, ky, allowResync) { return new Promise(function (res) { moveCursorTo(kx, ky, res, allowResync); }); }
  async function clickAt(kx, ky) {
    // allowResync=true: this is the tour's own cursor motion, the one
    // place a periodic real resync (relative-pointer fallback only) is
    // invisible rather than a bug, unchanged from the pre-v71 tour.
    await moveCursorToAsync(kx, ky, true);
    await sleep(120);
    emulator.bus.send("mouse-click", [true, false, false]);
    await sleep(80);
    emulator.bus.send("mouse-click", [false, false, false]);
  }
  async function runScript(script, gen) {
    if (!script) return;
    for (var i = 0; i < script.length; i++) {
      if (focused || tourGen !== gen) return;
      var step = script[i];
      if (step.type === "wait") await sleep(step.ms);
      else if (step.type === "keys" && emulator.keyboard_send_text) await emulator.keyboard_send_text(step.text, step.speed || 55);
    }
  }
  // Dock geometry in LOGICAL kernel pixels, the same arithmetic as
  // kernel.c's gui_dock_icon/gui_dock_x0/gui_slot_x: 10 slots, tiles
  // dock_scale_pct (default 7) percent of the height capped by the 740px
  // DOCK_BUDGET, gap 6, pad 10, bottom margin 24. A fresh v86 boot has no
  // SETTINGS.TXT, so the default scale is what's actually on screen.
  function dockSlotPos(slot) {
    var count = 10, gap = 6, pad = 10, marginBot = 24, budget = 740;
    var icon = Math.floor(LOGICAL_H * 7 / 100);
    var maxByWidth = Math.floor((budget - 2 * pad - (count - 1) * gap) / count);
    if (icon > maxByWidth) icon = maxByWidth;
    if (icon < 16) icon = 16;
    var dockW = count * icon + (count - 1) * gap + 2 * pad;
    var x0 = Math.floor((LOGICAL_W - dockW) / 2) + pad + Math.floor(icon / 2);
    var cy = LOGICAL_H - marginBot - pad - Math.floor(icon / 2);
    return [x0 + slot * (icon + gap), cy];
  }
  async function tourLoop(gen) {
    tourRunning = true;
    while (!focused && tourGen === gen) {
      for (var i = 0; i < TOUR_APPS.length; i++) {
        if (focused || tourGen !== gen || !adaptersReady) return;
        var app = TOUR_APPS[i];
        var pos = dockSlotPos(app.slot);
        // Drive the emulator's own input even though the visitor hasn't
        // focused: both adapters are gated for real people, not for us.
        // v71 real bug found here, separate from the v69 one above and
        // pre-dating this rework (present since v51, when typed-text tour
        // steps were first added): libv86.js's keyboard adapter gates
        // EVERY key it sends, including the programmatic
        // simulate_char/simulate_press path keyboard_send_text calls
        // (read directly out of libv86.js: `g()` calls `b(t)`, and `b(t)`
        // returns false whenever `!x.emu_enabled`), on the same
        // `emu_enabled` flag embed.js sets false on "emulator-ready" and
        // only ever set true inside focusIn(). The tour already force-
        // enables mouse_adapter.emu_enabled (the line below, present
        // since v44) but never did the keyboard equivalent, so every
        // scripted key the tour ever sent while unfocused was silently
        // swallowed at the source, not a rendering or timing issue.
        // Invisible until now because Terminal's static "Joshua Tree
        // terminal. Type help." banner prints unconditionally on open
        // (real content, but not evidence typed input landed) and Notes/
        // Chat's typed text was never actually screenshotted mid-dwell in
        // any prior pass. Confirmed two ways: `tracewatch.mjs` (a
        // throwaway QA script built for this pass) screenshotted the
        // instant after every `keyboard_send_text` call resolved and
        // showed zero visible change across 8 real sends into Mail and
        // Calendar; a second probe (`probe.mjs`) sending the identical
        // 'c' key through a REAL focus tap first (`focused=true`, the
        // same path a real visitor takes) opened Mail's real compose
        // prompt immediately. Fixed by enabling both adapters here, the
        // real fix, not a workaround: the tour is deliberately driving
        // input as if it were a focused user, so it should hold the same
        // two flags a focused user's first click sets.
        emulator.mouse_adapter.emu_enabled = true;
        emulator.keyboard_adapter.emu_enabled = true;
        await clickAt(pos[0], pos[1]); // opens app i, a real dock click
        if (focused || tourGen !== gen) return;
        var dwellStart = Date.now();
        await sleep(600); // let the app's first frame draw before typing into it
        if (focused || tourGen !== gen) return;
        await runScript(app.script, gen);
        if (focused || tourGen !== gen) return;
        var remaining = DWELL_MS - (Date.now() - dwellStart);
        if (remaining > 0) await sleep(remaining);
        if (focused || tourGen !== gen) return;
        await clickAt(CLOSE_X, CLOSE_Y); // closes via the app's own real X, never the dock tile that opened it
        if (focused || tourGen !== gen) return;
        await sleep(1200); // a beat before the next app opens, reads as a real transition not a jump-cut
      }
      // Every app already closed itself before the next opened, the only
      // real shape this kernel's single-window model supports (see the
      // v71 header comment above), so the loop is already at "everything
      // closed"; a slightly longer pause here just marks it as a
      // deliberate loop boundary rather than app #9.
      await sleep(2500);
    }
  }
  function startTourWhenReady() {
    if (focused) return;
    tourLoop(tourGen).catch(function () { /* a torn-down emulator mid-await (e.g. a real navigation) shouldn't spam the console */ });
  }
  // Boot takes a few seconds; the tour waits for graphical mode plus a
  // beat, and never starts at all once the visitor has focused. Also respects
  // prefers-reduced-motion: autoplay motion should not start if the visitor
  // has requested reduced motion.
  var tourArmed = false;
  var prefersReducedMotion = window.matchMedia && window.matchMedia('(prefers-reduced-motion: reduce)').matches;
  setInterval(function () {
    if (tourArmed || focused || prefersReducedMotion) return;
    var vga = emulator.v86 && emulator.v86.cpu.devices.vga;
    if (vga && vga.graphical_mode) { tourArmed = true; tourTimer = setTimeout(startTourWhenReady, 6000); }
  }, 500);

  window.__joshuaTreeEmulator = emulator; // for debugging from the console, harmless to leave
})();
