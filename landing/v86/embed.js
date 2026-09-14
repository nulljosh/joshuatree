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
    click: function () {
      emulator.bus.send("mouse-click", [true, false, false]);
      setTimeout(function () { emulator.bus.send("mouse-click", [false, false, false]); }, 60);
    },
    move: function (dx, dy) { emulator.bus.send("mouse-delta", [dx, -dy]); }
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
  // element's actual CSS size to the real letterboxed dimensions, not
  // relying on object-fit at all, makes the DOM box and the visible
  // image the same rectangle, so coordinate math anywhere downstream
  // (this file or v86's own mouse adapter) is correct by construction
  // instead of needing to know about letterboxing at all.
  var currentScale = 1;
  function resizeCanvas() {
    if (!screenCanvas) return;
    var box = screenContainer.getBoundingClientRect();
    var w = screenCanvas.width || 800, h = screenCanvas.height || 600;
    currentScale = Math.min(box.width / w, box.height / h) || 1;
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
    var lscale = screenCanvas.getBoundingClientRect().width / LOGICAL_W; // CSS px per LOGICAL kernel px (v41: canvas is 1600 physical, cursor is 800 logical)
    var dx = ev.movementX / lscale, dy = ev.movementY / lscale;
    emulator.bus.send("mouse-delta", [dx, -dy]); // y inverted, matching v86's own convention exactly
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
    if (lastTouchX !== null) {
      var lscale = screenCanvas.getBoundingClientRect().width / LOGICAL_W;
      var dx = (t.clientX - lastTouchX) / lscale, dy = (t.clientY - lastTouchY) / lscale;
      emulator.bus.send("mouse-delta", [dx, -dy]);
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
  // still believed it was centred.
  //
  // Homing instead of tracking removes the whole class of bug: push far
  // enough up-left that the kernel's own clamp pins the cursor at exactly
  // (0,0) no matter where it was, then move to the target from a known
  // origin. Self-correcting every single tap, no state to drift.
  function moveCursorTo(kx, ky, done) {
    var packets = [];
    splitDelta(-(LOGICAL_W + 400), -(LOGICAL_H + 400), packets); // clamps to (0,0) from anywhere on screen
    splitDelta(kx, ky, packets);
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
      }, 120);
    });
  }, { capture: true, passive: true });

  // Toggle between the text and graphical screen elements: v86 keeps both
  // in the DOM and expects the embedder to show whichever is active. The
  // real boot banner is genuine and correct, but it isn't the demo, so it
  // stays hidden through the whole boot-to-gui transition rather than
  // flashing on screen for the few hundred ms that takes; it only ever
  // surfaces as a fallback if graphical mode genuinely never arrives
  // (auto-gui failed for some reason), not as the default path.
  var bootStart = Date.now();
  setInterval(function () {
    var vga = emulator.v86 && emulator.v86.cpu.devices.vga;
    if (!vga) return;
    var graphical = !!vga.graphical_mode;
    var stuckInText = !graphical && Date.now() - bootStart > 4000;
    if (screenCanvas) screenCanvas.style.display = graphical ? "block" : "none";
    if (screenText) screenText.style.display = graphical ? "none" : (stuckInText ? "block" : "none");
    resizeCanvas(); // the canvas's real pixel resolution only exists once graphical mode sets it, re-check every tick until it does
  }, 200);

  // stopAutoplay is kept as a no-op call site for focusIn() above; there's
  // no scripted CLI demo left to cancel now that boot drives straight into
  // the GUI, but focusIn() calling it costs nothing and keeps that code
  // simple if a mouse-driven idle tour gets added here later.
  function stopAutoplay() {}

  window.__joshuaTreeEmulator = emulator; // for debugging from the console, harmless to leave
})();
