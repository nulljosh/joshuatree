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
// v0.76.10: pure, tested separately from the emulator wiring below (see
// tools/checks/rtc-timezone-check.mjs) since a real end-to-end check needs
// a real browser + v86 boot this CI doesn't have. libv86.js's own CMOS/RTC
// device answers every read in UTC, hardcoded, no local-time option --
// confirmed by reading its cmos_port_read directly, which always calls
// getUTCHours/getUTCDate/etc. Shifts a UTC-based epoch ms value by the
// browser's own local UTC offset so that reading the RESULT's UTC fields
// (exactly what the RTC device does) reports the real local wall clock
// instead. tzOffsetMinutes defaults to the real browser's own
// Date.getTimezoneOffset(); the test passes it explicitly since Node has
// no visitor to ask.
function localRtcTime(epochMs, tzOffsetMinutes) {
  if (tzOffsetMinutes === undefined) tzOffsetMinutes = new Date().getTimezoneOffset();
  return epochMs - tzOffsetMinutes * 60000;
}
if (typeof module !== "undefined") module.exports = { localRtcTime: localRtcTime };

// Guarded so rtc-timezone-check.mjs can import this file under plain Node
// (to unit-test localRtcTime above) without executing the real emulator
// wiring below, which assumes a real DOM.
if (typeof document !== "undefined") (function () {
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
  // v0.73.5: fetched once and reused by the idle tour's reboot sequence
  // below (see the comment above the reboot block in tourLoop) to
  // re-inject the kernel image after each lap's reset_memory(); this
  // kernel boots with no BIOS ROM at all (`multiboot: {url: ...}` below,
  // no `bios:`/`vga_bios:` options), and v86's own reboot path
  // (`S.prototype.reboot_internal`, read directly in libv86.js) only ever
  // calls `load_bios()`, which is a complete no-op with no BIOS main
  // image set (`if(a){...}` where `a=this.bios.main`, undefined here).
  // The multiboot ELF is only ever loaded once, by a one-shot
  // `this.reg32[0]=this.io.port_read32(244)` trick that runs inline
  // during `S.prototype.init` (the V86 constructor), never again on any
  // later reboot. Confirmed with real instrumentation, not assumed: a
  // live probe wrapping `reset_memory`/`restart` and sampling
  // `emulator.v86.cpu.mem8` at the kernel's load address (0x100000)
  // showed the region reads back all-zero after every `restart()` call
  // (never reloaded), and the kernel's own serial log (`serialLog`)
  // never printed a second "=== kmain boot start ===" line across two
  // full reboot cycles, i.e. kmain, and therefore ramfs_init, truly never
  // ran again after the first boot. `S.prototype.load_multiboot` (see
  // libv86.js) is the same loader the constructor's one-shot trick calls
  // internally, and it's exposed as a public method on the CPU object;
  // calling it again after `reset_memory()`+`restart()` redoes exactly
  // what the very first boot did, a real fix at the actual point of the
  // gap (v86 has no repeatable "no-BIOS multiboot reboot" path of its
  // own), not a kernel-side workaround for something the emulator itself
  // never implemented.
  var kernelElfBuffer = null;
  var kernelElfFetch = fetch("v86/kernel.elf").then(function (r) { return r.arrayBuffer(); }).then(function (buf) {
    kernelElfBuffer = buf;
    return buf;
  }).catch(function () { return null; });
  var screenContainer = document.getElementById("screen_container");
  var screenText = document.getElementById("screen_text");
  var screenCanvas = document.getElementById("screen_canvas");
  var overlay = document.getElementById("v86-overlay");

  // v0.76.26 fix: prevent Escape key from reaching the kernel when a visitor
  // presses it (which triggers kernel.c's gui_run shell-exit feature, leaving
  // the demo stuck at a bare shell prompt). This listener must be registered on
  // `window` (not `container`), in capture phase, and BEFORE the V86 constructor
  // attaches its own keyboard listener -- all capture-phase listeners on the same
  // element (`window`) fire in FIFO registration order, so registering here first
  // ensures this intercepts Escape before v86's listener processes it.
  // The tour's own intentional Escape sends use keyboard_send_keys() which bypasses
  // normal event listeners, so this only blocks real visitor keypresses, not
  // scripted tour automation.
  window.addEventListener("keydown", function (ev) {
    if (ev.key === "Escape" || ev.keyCode === 27 || ev.code === "Escape") {
      ev.preventDefault();
      ev.stopImmediatePropagation();
    }
  }, true); // capture phase, BEFORE v86's own global listener (both on window, FIFO order)

  var emulator = new V86({
    wasm_path: "v86/v86.wasm",
    memory_size: 32 * 1024 * 1024,
    vga_memory_size: 16 * 1024 * 1024, // v41: 1600x1200x32bpp is 7.68MB, 8 was one bad rounding away from failing
    screen_container: screenContainer,
    multiboot: { url: "v86/kernel.elf" },
    autostart: true,
    // Real network backend for the emulated NIC: without this, v86's NIC
    // (ne2k by default, see drivers/ne2k.c) is wired to nothing, every
    // packet the guest sends just vanishes, regardless of how correct the
    // in-kernel driver is. "fetch" is v86's own browser-side relay mode
    // (class Xb in the vendored libv86.js): it accepts any TCP connection
    // to port 80, reads the guest's raw HTTP request text straight off
    // the wire, and answers it with a real browser fetch() to whatever
    // Host: header the guest actually sent, so the guest's own raw
    // ARP/IP/TCP/HTTP stack (drivers/net.c, drivers/http.c) never has to
    // change at all. roadmap.md's "fetch-mode network relay" entry
    // investigated this exact flag back when the kernel had no driver for
    // any NIC v86 emulates at all (RTL8139-only) and correctly called it
    // a dead end at the time; drivers/ne2k.c is what closes that gap.
    //
    // vm_ip/router_ip matter here, not just cosmetic: Xb's own defaults
    // are 192.168.86.100/192.168.86.1, but every address this kernel's
    // network stack hardcodes (net_init's 0x0A00020F, drivers/net.c's
    // DEFAULT_GATEWAY_IP) assumes QEMU SLIRP's 10.0.2.0/24, the same
    // subnet native `make run`/geo-check.sh already boot into. Left at
    // Xb's own defaults, the kernel's ARP request for its hardcoded
    // gateway (10.0.2.2) never matches Xb's real router_ip and times out
    // before any TCP even starts, confirmed by an earlier live headless
    // run of this exact test: ne2k_init succeeded, net_init found the
    // card, and the run still failed with no geo=/wxurl=/wx= serial
    // lines, exactly what a dead ARP resolve looks like. Real DNS answers
    // don't matter here either way (Xb's "static" dns_method answers
    // every A-record query with the same fake 192.168.87.1, confirmed
    // reading Nb's dns_method==="static" branch in libv86.js): the relay
    // only cares about the Host: header the guest sends, not what it
    // resolved, so the mismatch there is by design and not something to
    // fix on the kernel side.
    // type: "ne2k" has to be explicit here: libv86.js only defaults
    // net_device to {type:"ne2k"} when net_device itself is falsy
    // (`e.net_device=b.net_device||{type:"ne2k"}`), so passing our own
    // net_device object without it left type undefined and silently
    // dropped the emulated NIC from the PCI bus entirely, confirmed live:
    // the very next run of this same test after adding relay_url/vm_ip/
    // router_ip regressed from "net_init: ne2k found" back to "net_init:
    // no NIC found".
    // v0.76.10: cors_proxy is Xb's own first-class option for exactly the
    // CORS block found root-causing why satellite/map/geo never rendered
    // (roadmap.md has the full trace) -- Xb.prototype.fetch already does
    // `this.cors_proxy && (a = this.cors_proxy + encodeURIComponent(a))`
    // before fetching, read directly in the vendored libv86.js, not
    // guessed. Routes every guest HTTP request through this same origin's
    // own /api/proxy Worker route (worker.js, allowlisted to exactly the
    // three real hosts this kernel ever asks for), which fetches
    // server-to-server -- never CORS-limited -- instead of the browser
    // fetching ip-api.com/opentopomap.org/google directly and getting
    // rejected before the request even leaves the page.
    net_device: { type: "ne2k", relay_url: "fetch", vm_ip: "10.0.2.15", router_ip: "10.0.2.2", cors_proxy: "/api/proxy?url=" },
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
    // Direct report: the menu bar clock shows the wrong day, "should say
    // today but says tomorrow." Real, root-caused, not a kernel bug:
    // libv86.js's own CMOS/RTC device (class hb) answers every read with
    // getUTCHours/getUTCDate/getUTCMonth/etc, hardcoded, no config option
    // to make it answer in local time instead (confirmed by reading hb's
    // own cmos_port_read directly). Real hardware and QEMU's CLI usually
    // hand the guest local time (inherited from the host's own RTC/BIOS
    // config), but v86 has no host machine's RTC to inherit from, so it
    // always reports UTC no matter what timezone the visitor is actually
    // in. kernel.c's own cmos() reads those raw registers and displays
    // them as-is (a fair assumption on real hardware, where that's just
    // what a real BIOS-set RTC means), with no timezone layer of its own
    // to correct a browser-only quirk in what the RTC hands back -- for
    // anyone west of UTC, from roughly (24 - their own UTC offset in
    // hours) local time onward each evening, UTC has already rolled to
    // the next calendar day while it's still today for them, showing
    // "tomorrow" exactly as reported.
    // Fix: shift the live RTC counter once, right after boot, by the
    // browser's own local UTC offset (Date.getTimezoneOffset(), the one
    // API that actually knows the visitor's real timezone -- the kernel
    // has no way to learn this on its own, there's no NTP/timezone
    // protocol wired up). Every later CMOS read stays live off this same
    // shifted counter (hb's own timer() just keeps adding real elapsed
    // time to it), so it doesn't drift back to UTC after the first read.
    var rtc = emulator.v86 && emulator.v86.cpu && emulator.v86.cpu.devices && emulator.v86.cpu.devices.rtc;
    if (rtc) rtc.rtc_time = localRtcTime(rtc.rtc_time);
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
  var idleRestartTimeout = 0;
  var lastInteractionTime = Date.now();
  function resetIdleRestart() {
    if (idleRestartTimeout) clearTimeout(idleRestartTimeout);
    if (!focused) return; // only auto-reset if visitor has taken control
    idleRestartTimeout = setTimeout(async function () {
      // Retail-kiosk style: after 4 seconds of inactivity, close windows and restart the tour.
      // Only trigger if 4+ seconds have passed since the last user interaction (click, movement, key).
      var timeSinceActivity = Date.now() - lastInteractionTime;
      if (focused && !tourRunning && timeSinceActivity >= 4000) { // only if still focused, tour not running, and truly idle
        // Trigger a soft reset: close any open windows by rebooting the emulator
        // then restart the tour
        if (bootLogo) bootLogo.hidden = false;
        if (!kernelElfBuffer) { try { kernelElfBuffer = await kernelElfFetch; } catch (e) {} }
        if (emulator.v86 && emulator.v86.cpu && emulator.v86.cpu.reset_memory) emulator.v86.cpu.reset_memory();
        emulator.restart();
        if (kernelElfBuffer && emulator.v86 && emulator.v86.cpu && emulator.v86.cpu.load_multiboot) emulator.v86.cpu.load_multiboot(kernelElfBuffer);
        // Reset focused and tourArmed to allow the idle tour to restart
        focused = false;
        tourArmed = false;
      }
      idleRestartTimeout = 0;
    }, 4000);
  }
  function focusIn() {
    if (focused || !adaptersReady) return;
    focused = true;
    emulator.keyboard_adapter.emu_enabled = true;
    emulator.mouse_adapter.emu_enabled = true;
    if (overlay) overlay.classList.add("hidden");
    stopAutoplay();
    resetHeadline(); // v0.76.29: reset headline when visitor takes control
    lastInteractionTime = Date.now();
    resetIdleRestart();
  }
  function trackActivity() {
    lastInteractionTime = Date.now();
    if (focused) resetIdleRestart();
  }
  function trackClick() {
    // Track when a click is sent to the kernel so the idle-restart doesn't trigger mid-interaction
    if (focused) {
      lastInteractionTime = Date.now();
      resetIdleRestart();
    }
  }
  container.addEventListener("mousedown", focusIn);
  container.addEventListener("touchstart", focusIn, { passive: true });
  container.addEventListener("keydown", focusIn);
  container.addEventListener("mousemove", trackActivity);
  container.addEventListener("touchmove", trackActivity, { passive: true });
  // v0.76.26: after focusIn() sets focused=true, subsequent keydown events
  // would return early from focusIn() without updating lastInteractionTime,
  // causing the idle-reset to fire after 4s of typing without mouse movement.
  // These listeners ensure ALL continued interaction updates the activity
  // timestamp, regardless of what focusIn() does.
  container.addEventListener("keydown", trackActivity);
  container.addEventListener("keyup", trackActivity);
  container.addEventListener("click", trackActivity);
  // v0.77.1: wheel scrolling for Apps folder. Normalize browser wheel events
  // (deltaY, wheelDelta, detail across different browsers) to the emulator's
  // expected convention: positive delta = scroll down = negative wheel value
  // for the kernel's scroll_offset (see kernel.c's gui_launch_apps_folder).
  screenContainer.addEventListener("wheel", function (ev) {
    if (!focused || !emulator.mouse_adapter || !emulator.mouse_adapter.emu_enabled) {
      trackActivity();
      return;
    }
    trackActivity();
    // Modern wheel events use deltaY (positive=down), older ones use wheelDelta
    // (positive=up, opposite sign). Normalize to a -1/0/+1 scale matching
    // what v86's own mouse adapter does (see libv86.js's mouse_adapter init).
    var delta = 0;
    if ("deltaY" in ev) {
      // Wheel event (modern), deltaY > 0 = down
      delta = ev.deltaY > 0 ? -1 : (ev.deltaY < 0 ? 1 : 0);
    } else if ("wheelDelta" in ev) {
      // Legacy mousewheel event, wheelDelta > 0 = up (opposite)
      delta = ev.wheelDelta > 0 ? 1 : (ev.wheelDelta < 0 ? -1 : 0);
    } else if ("detail" in ev) {
      // Older Netscape-style, detail > 0 = down
      delta = ev.detail > 0 ? -1 : (ev.detail < 0 ? 1 : 0);
    }
    if (delta !== 0) {
      emulator.bus.send("mouse-wheel", [delta, 0]);
      ev.preventDefault();
    }
  }, { capture: true, passive: false });

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
      // v0.76.28: the H1 headline itself became a real <a href="#changelog">
      // link; a click on it was being swallowed by this same handler,
      // which calls showDemo() unconditionally -- showDemo() adds
      // "demo-focused", and the CSS rule right above (.hero.demo-focused
      // .hero-copy { opacity: 0 !important }) hides hero-copy itself,
      // including the link that was just clicked, before the browser's
      // own navigation to #changelog ever completes. Skip the
      // dismiss-card behavior for a real link click; let it navigate.
      var heroCopyClickThrough = function (ev) { return !!(ev.target.closest && ev.target.closest("a")); };
      heroCopyEl.addEventListener("mousedown", function (ev) { if (heroCopyClickThrough(ev)) return; showDemo(); ev.stopPropagation(); });
      heroCopyEl.addEventListener("touchstart", function (ev) { if (heroCopyClickThrough(ev)) return; showDemo(); ev.stopPropagation(); }, { passive: true });
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
      trackClick();
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
        trackClick();
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
  // then restart"), the honest version of "show off every app" at a time
  // when roadmap.md's "Multi-window, honestly scoped" entry still
  // confirmed real window stacking did not exist yet: every app was a
  // single, full takeover, and the dock stayed live only to switch which
  // ONE app was showing (v69), never to stack a second one on top.
  // Sequential open/use/close/next was the real, buildable shape of that
  // at the time, not a simulated multi-window fake this kernel couldn't
  // actually do.
  //
  // STALE AS OF v0.73.0-v0.75.0: real multi-window shipped (gui_multiwin_*
  // in kernel.c, GUI_MULTIWIN_MAX=2, real click-to-focus/z-order,
  // tools/checks/multiwindow-check.py). Direct report (Sep 2026): the
  // landing header now says "Introducing Multi-Window." but this tour
  // still only ever showed one window at a time, closing each before the
  // next opened -- the header claimed a real capability the demo never
  // actually demonstrated. multiWindowRound() below (added the same pass)
  // fixes that for the 3 dock apps that are genuinely multi-window
  // capable in this kernel (Files/Weather/Reminders; see
  // gui_multiwin_supported in kernel.c) by opening two of them together,
  // without closing the first, and driving the exact real click-to-focus
  // switch tools/checks/multiwindow-check.py already proves against the
  // real kernel. GUI_MULTIWIN_MAX is a real, current cap of 2 concurrent
  // windows, not 3 -- this tour never asks for a 3rd, since the kernel
  // itself has no 3rd slot to give it. TOUR_APPS below keeps only the
  // genuinely single-window-only apps (Notes/Terminal/Chat; confirmed via
  // gui_multiwin_supported returning false for their icons) plus Mail and
  // Calendar solo (real multiwin apps too, but shown one at a time here
  // since the 2-window slots are spent on the Files/Weather/Reminders
  // rounds -- still real, just not simultaneous in every lap).
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
  // v0.72.2: restored to all 8 dock apps, direct request ("looks through
  // every app properly, only the first half in the dock"), after v0.71.3
  // had trimmed this to 3 (Files/Mail/Calendar) to stop the full cycle
  // reading as a marathon. Reconciled both asks by keeping every app but
  // cutting DWELL_MS well below, so the whole loop is still a quick,
  // repeating sample rather than either extreme.
  // Mail and Calendar pulled out to named vars (not TOUR_APPS entries)
  // so tourLoop can interleave them with the multi-window rounds instead
  // of running the whole array as one block after both rounds -- direct
  // feedback that leading with pure window-management read as "not much
  // interaction". TOUR_APPS keeps only the genuinely single-window-only
  // apps (Notes/Terminal/Chat; gui_multiwin_supported in kernel.c is
  // false for all three).
  var MAIL_APP = { name: 'Mail', slot: 2, script: [
    { type: 'keys', text: 'c', speed: 200 },
    { type: 'wait', ms: 500 },
    { type: 'keys', text: 'demo@joshuatree.os\n', speed: 55 },
    { type: 'wait', ms: 350 },
    { type: 'keys', text: 'A real OS, from scratch.\n', speed: 55 },
    { type: 'wait', ms: 350 },
    { type: 'keys', text: 'Every field here really writes to disk.\n', speed: 55 }
  ] };
  var CALENDAR_APP = { name: 'Calendar', slot: 3, script: [
    { type: 'keys', text: 'dd', speed: 400 }, // step forward two months, a real render each time
    { type: 'wait', ms: 400 },
    { type: 'keys', text: ']]', speed: 400 }, // move the day cursor
    { type: 'wait', ms: 400 },
    { type: 'keys', text: '\n', speed: 200 }, // opens cal_day_view for the selected day
    { type: 'wait', ms: 500 },
    { type: 'keys', text: 'Shipped by an AI, for real.\n', speed: 55 } // saves and returns to the month view
  ] };
  var TOUR_APPS = [
    { name: 'Notes', slot: 4, script: [
      { type: 'keys', text: 'Kernel, GUI, browser, terminal, and a dozen real apps, none of it borrowed.', speed: 55 }
    ] },
    { name: 'Terminal', slot: 6, script: [
      { type: 'keys', text: 'help\n', speed: 55 },
      { type: 'wait', ms: 900 },
      { type: 'keys', text: 'uptime\n', speed: 55 }
    ] },
    // v0.76.11: real bug found and fixed here, present since v51 and never
    // actually looked at on screen (per the v71 rework's own header
    // comment: "Notes/Chat's typed text was never actually screenshotted
    // mid-dwell in any prior pass"). Sending the raw string straight into
    // Chat's OUTER inbox view (no leading 'n') let two of its own letters
    // get read as real commands mid-string: the kernel's gui_launch_chat_app
    // only recognizes bare 'n' (compose) and 'c' (clear history) at that
    // screen -- "what CAN you do?" hits 'c' first (chat_clear(), a real,
    // unintended side effect on every single tour lap) then immediately
    // 'n' (enters the compose prompt), silently swallowing every character
    // typed after that point ("you do?") as if it were a real draft, not
    // display text. Confirmed live: a real screenshot after this exact
    // script showed the compose prompt open with "you do" typed into it,
    // never the intended on-screen text. Real fix: press 'n' FIRST (a
    // clean, deliberate entry into the compose prompt, where every
    // character just appends to the buffer -- no collision risk once
    // inside it), type the same real sentence, then a real Escape (not
    // Enter -- preserving the original "no \n" intent: this embed has no
    // NIC route to a real LLM host, so completing an actual send was
    // never safe to attempt) cancels back to the outer inbox view, the
    // same clean state runSoloApp's own single CLOSE_X click already
    // expects to close from.
    { name: 'Chat', slot: 7, script: [
      { type: 'keys', text: 'n', speed: 200 },
      { type: 'wait', ms: 400 },
      { type: 'keys', text: 'what can you do?', speed: 55 },
      { type: 'wait', ms: 500 },
      { type: 'raw', codes: [27], speed: 80 } // Escape cancels back to the outer inbox view
    ] }
  ];
  // v0.76.12: the real multi-window demo. Files (slot 1) and Weather
  // (slot 8) have no per-app keyboard interaction in this kernel (both are
  // gui_wait_close-only static viewers, confirmed by reading kernel.c --
  // scripting a fake keystroke into either would be exactly the dishonest
  // "simulated interaction" this tour has never done for any app), so
  // their entries carry no `script`; Reminders (slot 5) is real-input
  // capable and keeps the exact same add-a-reminder script the old
  // sequential entry used, just now run while a second window (Files) is
  // genuinely open alongside it, not before/after it.
  var MW_FILES = { name: 'Files', slot: 1 };
  var MW_WEATHER = { name: 'Weather', slot: 8 };
  var MW_REMINDERS = { name: 'Reminders', slot: 5, script: [
    { type: 'keys', text: 'a', speed: 200 },
    { type: 'wait', ms: 500 },
    { type: 'keys', text: 'Ship the demo tour rework\n', speed: 55 },
    { type: 'wait', ms: 400 },
    { type: 'keys', text: 'a', speed: 200 },
    { type: 'wait', ms: 400 },
    { type: 'keys', text: 'Add mouse wheel support to the Apps folder\n', speed: 55 },
    { type: 'wait', ms: 400 },
    { type: 'keys', text: 'a', speed: 200 },
    { type: 'wait', ms: 400 },
    { type: 'keys', text: 'Real hardware port of the kernel\n', speed: 55 }
  ] };
  // gui_launch_from_dock's own fixed traffic-light X (x=70,y=40 non-apps-
  // folder window, red circle at x+24,y+16), the same LOGICAL coordinate
  // mobiletest.mjs already taps for its Notes-close check, identical for
  // every dock-launched app since that rect never depends on which icon
  // opened it. Never inside the dock's own hit-test rect (kernel.c's
  // gui_dock_hit_test bounds its y range to the dock band near the
  // bottom of a 540px-tall logical screen; 56 is nowhere near it), which
  // is exactly what makes it the real fix for the v69 interaction above.
  var CLOSE_X = 94, CLOSE_Y = 56;
  var DWELL_MS = 7000; // v0.72.2: cut from 15s once the app count went back to 8, keeps the full loop under a minute
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
    trackClick();
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
      // v0.76.11: a raw keyCode send, the same path demoSatelliteWallpaper's
      // own Escape send already uses (simulate_char's tables have no entry
      // for Escape, so the usual "keys" step would silently no-op for it).
      // Added for Chat's own fix below: cancelling out of its compose
      // prompt with a real Escape, not a typed character.
      else if (step.type === "raw" && emulator.keyboard_send_keys) await emulator.keyboard_send_keys(step.codes, step.speed || 80);
    }
  }
  // v0.76.12: real two-window demo, the exact click sequence
  // tools/checks/multiwindow-check.py already proves against the real
  // kernel (gui_multiwin_open/gui_multiwin_focus/gui_multiwin_hit_test in
  // kernel.c), not a new/unverified interaction shape. Two fixed points
  // do all of it, both real consequences of gui_multiwin_geom's own fixed
  // per-window rects (window 0 = x70,y40,w820,h385; window 1 = offset
  // +60,+60, never recomputed after either window opens):
  //   MW_A_POINT  (94,56)   sits ONLY inside window 0's rect (94<130, the
  //               start of window 1's rect), so a click there always
  //               targets "whichever app opened first" regardless of
  //               which is currently on top -- closes it if it's topmost,
  //               otherwise raises it to the front (gui_multiwin_focus).
  //   MW_TOP_POINT (154,116) sits inside BOTH windows' overlap, so a click
  //               there always resolves (topmost-first hit test) to
  //               whichever window is currently on top, and closes it.
  // first opens as window 0, second opens as window 1 alongside it --
  // both genuinely on screen together, the real thing the old sequential
  // open/close/open/close tour could never show. Only 2 concurrent
  // windows is the real, current kernel cap (GUI_MULTIWIN_MAX in
  // kernel.c); this never asks for a 3rd.
  var MW_A_POINT_X = CLOSE_X, MW_A_POINT_Y = CLOSE_Y; // = 94,56, same rect appclose-check.py/app-interact-check.py already depend on
  var MW_TOP_POINT_X = 154, MW_TOP_POINT_Y = 116;
  async function multiWindowRound(gen, first, second) {
    if (focused || tourGen !== gen || !adaptersReady) return;
    updateHeadline(first.name); // v0.76.29: update headline as multi-window round starts with first app
    emulator.mouse_adapter.emu_enabled = true;
    emulator.keyboard_adapter.emu_enabled = true;
    var posA = dockSlotPos(first.slot), posB = dockSlotPos(second.slot);

    await clickAt(posA[0], posA[1]); // opens `first` as window 0
    if (focused || tourGen !== gen) return;
    await sleep(600);
    await runScript(first.script, gen); // real interaction while it's the only (topmost) window
    if (focused || tourGen !== gen) return;

    await clickAt(posB[0], posB[1]); // opens `second` as window 1 ALONGSIDE it -- first stays open, the real point being demonstrated
    if (focused || tourGen !== gen) return;
    // v0.76.29: update headline when second app comes to the foreground
    updateHeadline(second.name);
    await sleep(600);
    await runScript(second.script, gen); // real interaction with the now-topmost window, first still genuinely on screen behind it
    if (focused || tourGen !== gen) return;
    await sleep(1400); // a real beat with both windows visibly open together -- the actual point of this round

    // v0.76.14: direct report, "landing page demo still flashing" --
    // real root cause, no double buffer in this kernel yet (a known,
    // already-tracked architecture gap, roadmap.md's own "compositor in
    // gui_run" entry), so every multiwin open/close/focus event forces a
    // full desktop repaint (kernel.c's `launched=1` path: wallpaper photo
    // blit + dock + every open window, every time). This round used to
    // also click-to-focus `first` back to the top before closing it --
    // a real, legitimate demonstration of z-order switching, but a 5th
    // full-desktop repaint packed into the same ~13s window, on top of
    // the 4 this round already needs. Cut here: `second` closes first
    // (MW_TOP_POINT always resolves to whichever window is currently
    // topmost), which leaves `first` as the sole remaining window --
    // and the sole remaining window is topmost by definition, so
    // MW_A_POINT correctly closes it next without ever needing the
    // focus step. Real click-to-focus/z-order switching is still proven
    // by tools/checks/multiwindow-check.py against the real kernel; this
    // tour just no longer re-demonstrates it at the cost of an extra
    // flash every single lap.
    await clickAt(MW_TOP_POINT_X, MW_TOP_POINT_Y); // closes whichever of the two is currently topmost (`second`)
    if (focused || tourGen !== gen) return;
    await sleep(900);
    await clickAt(MW_A_POINT_X, MW_A_POINT_Y); // `first` is now the sole open window, topmost by definition: closes it too
    if (focused || tourGen !== gen) return;
    await sleep(1200);
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
  // v0.72.3: real bug reported live ("every time it runs a demo and then
  // restarts the demo, it adds like emails to the list, duplicate emails").
  // Root cause: this kernel's browser demo has no real disk (v0.71.0), so
  // Mail/Reminders/Notes/Calendar all live in an in-memory ramfs. The tour
  // above never reboots the emulator between loops, it only closes and
  // reopens app windows, so every full 8-app cycle composes another real
  // email and adds another real reminder into the SAME ramfs the last
  // cycle left behind, forever, for as long as the tab stays open.
  //
  // First attempt, reverted: calling `emulator.restart()` alone
  // (`Q.prototype.restart=function(){this.v86.restart()}` -> `S.prototype.
  // reboot_internal`, a real CPU reset + BIOS reload) looked like the fix,
  // but `reboot_internal` (read directly in libv86.js) only resets CPU
  // registers and a few specific devices, it never calls `S.prototype.
  // reset_memory` (`this.mem8.fill(0)`). Proved live: a real 2-lap
  // Playwright run (periodic screenshots, not just pixel heuristics) with
  // only `restart()` showed lap 2's Files listing more files
  // (README/NOTES/MAIL/EVENTS/REMINDERS/CHAT.TXT) than lap 1's fresh
  // README/NOTES.TXT, i.e. ramfs's old content survived the "reboot"
  // untouched. A follow-up kernel-side attempt (zeroing boot.S's .bss
  // before kmain, the standard freestanding-kernel fix for code that
  // implicitly relies on RAM starting zero) was ALSO reverted: it passed
  // `check.sh` and a real QEMU `system_reset` cleanly, but hung the v86
  // browser path specifically after lap 1 (confirmed with a live
  // instruction-counter probe: the CPU kept executing, ~3.5B instructions
  // counted, so it wasn't frozen, but `vga.graphical_mode` never returned
  // and the framebuffer never changed again for 90+ seconds) -- some v86-
  // specific interaction with the reboot's BIOS/option-ROM replay this
  // session couldn't fully root-cause in the time available, so it was
  // pulled rather than shipped half-working.
  //
  // Third attempt (v0.73.4), real but incomplete: `emulator.v86.cpu`
  // exposes `reset_memory` directly (`S.prototype.
  // reset_memory=function(){this.mem8.fill(0)}`), the one call
  // `reboot_internal` skips, so it was called immediately before
  // `restart()` to zero guest RAM first. This looked complete (a real,
  // reliable non-hanging reboot, verified live) but a live 2-lap
  // Playwright run still showed lap 2's Files listing carrying lap 1's
  // accumulated content. Root-caused for real in the v0.73.5 pass with
  // direct instrumentation (see the comment above `kernelElfBuffer`
  // near the top of this file): this kernel boots with no BIOS ROM at
  // all, and v86's own `reboot_internal` only ever calls `load_bios()`,
  // which no-ops completely with no BIOS main image set. The multiboot
  // kernel image is only ever loaded ONCE, by a one-shot trick that
  // runs inline during the V86 constructor and is never re-run by any
  // later reboot; `reset_memory()` genuinely zeroes RAM, but nothing
  // ever puts the kernel back afterward, so `mem8` reads back all-zero
  // forever after the first reboot (confirmed directly, not guessed:
  // `emulator.v86.cpu.mem8` sampled at the kernel's 0x100000 load
  // address after `restart()`, and the kernel's own serial log, which
  // never printed a second "=== kmain boot start ===" across two full
  // reboot cycles). The real fix: re-run the same loader v86's own
  // constructor uses, `S.prototype.load_multiboot` (a public method on
  // the CPU object), immediately after `reset_memory()` + `restart()`,
  // using the kernel ELF bytes fetched once at page load
  // (`kernelElfBuffer`/`kernelElfFetch` above). This is the actual root
  // cause fix, at the real point of the gap (v86 has no built-in way to
  // replay a no-BIOS multiboot boot on reset), not a kernel-side
  // workaround for a gap the emulator itself never covered.
  // v0.75, direct user report: the landing page keeps showing the default
  // warm-tinted map wallpaper, never the Satellite theme this demo is
  // supposed to show off. Root cause: wall_theme defaults to WALL_WARM
  // (kernel.c), Satellite is real and reachable but opt-in only, through
  // Settings' own Wallpaper row -- and this tour never visited Settings at
  // all, so a passive visitor had no way to ever see it. Blocked on a real
  // kernel bug until now: Settings' click handler only ever acted on
  // whichever row a keyboard arrow-key press had left selected (`sel`
  // starts at row 0, Wind), so a click-only visitor -- this tour included,
  // it never sends arrow keys -- could never reach the Wallpaper row by
  // clicking alone. Fixed kernel-side (settings_row_at, see roadmap.md);
  // this step is what actually uses that fix.
  //
  // Settings has no close-X chrome (only Escape closes it, kernel.c's
  // gui_launch_settings), and Escape's ASCII code (27) isn't in
  // libv86.js's own simulate_char lookup tables (confirmed by reading
  // them directly: neither table B nor C has a key 27), so the usual
  // `keys`/simulate_char script step this file uses everywhere else would
  // silently no-op here. keyboard_send_keys([27], ...) sends the raw
  // keyCode instead, through simulate_press's own keyCode-indexed
  // scancode table (F[27]===1, the real PS/2 Escape scancode, confirmed
  // by reading that table too), the same path already proven reliable
  // for this file's existing '\n' (Enter) sends.
  var LOGO_X = 16, LOGO_Y = 13; // gui_draw_logo(16, GUI_MENUBAR_H/2+2, ...); logo_here's own hit rect is x in [4,28], y < GUI_MENUBAR_H
  var SETTINGS_MENU_X = 94, SETTINGS_MENU_Y = 111; // Apple-menu row 3 ("Settings"): y starts at GUI_MENUBAR_H+GUI_MENU_PAD_V=34, three prior 22px rows -> [100,122)
  var ABOUT_MENU_X = 94, ABOUT_MENU_Y = 45; // Apple-menu row 0 ("About Joshua Tree"): y starts at GUI_MENUBAR_H+GUI_MENU_PAD_V=34, centre of the first 22px row
  // v0.76.12: direct request ("at the end, after showing all the apps,
  // click 'About this computer' and show basic CPU/storage stats, like
  // About This Mac"). kernel.c's gui_launch_about() already exists and
  // already shows real stats read straight out of the running kernel
  // (pmm_total_frames/pmm_free_frames for memory, ticks() for uptime,
  // gui_wait_close's own "any click closes" contract) -- this was never
  // in the tour at all, so a real visitor had no way to discover it.
  // Real bug found and fixed alongside this (kernel.c): the panel's own
  // version line was hardcoded to "Version 0.42.1" and had been for
  // dozens of real releases since, even though a real build-time
  // JT_VERSION_STR macro already existed and was already used elsewhere
  // (the boot serial log) -- just never wired into this one other place
  // a version number is shown to a real visitor. Fixed to use the same
  // macro, so this dwell now shows the real current version, not a
  // 30-versions-stale one.
  async function demoAboutPanel(gen) {
    if (focused || tourGen !== gen || !adaptersReady) return;
    emulator.mouse_adapter.emu_enabled = true;
    emulator.keyboard_adapter.emu_enabled = true;
    await clickAt(LOGO_X, LOGO_Y); // opens the Apple menu
    if (focused || tourGen !== gen) return;
    await sleep(300);
    await clickAt(ABOUT_MENU_X, ABOUT_MENU_Y); // selects "About Joshua Tree"
    if (focused || tourGen !== gen) return;
    await sleep(3500); // real dwell showing real memory/uptime/version stats
    if (focused || tourGen !== gen) return;
    await clickAt(400, 300); // any click closes (gui_wait_close's own contract), same as every other single-view app
    await sleep(800);
  }
  // v0.76.13: direct report ("landing page still shows no satellite
  // wallpaper"), root-caused for real this time -- not the CORS proxy
  // (already proven live, see roadmap.md's live-smoke entry), this
  // function itself. It was written when kernel.c's own wall_theme
  // defaulted to WALL_WARM(1): "a tap always steps forward" through
  // Photo(0)->Warm(1)->Cool(2)->Raw(3)->Sat(4)->wraps to Photo, so 3
  // clicks from Warm correctly landed on Satellite. v0.76.7 flipped the
  // real kernel default to WALL_SAT(4) (confirmed directly in kernel.c)
  // -- this function's own 3-click loop was never updated to match, so
  // every single tour lap since v0.76.7 shipped has been starting
  // already ON Satellite and clicking 3 times PAST it: 4->0(Photo)->
  // 1(Warm)->2(Cool), leaving the real desktop on the Cool map theme, not
  // Satellite, every lap, this whole time. A real, silent regression a
  // kernel-side default change caused in landing-side script, not caught
  // because no test ever asserted what theme the tour's OWN clicking
  // leaves the desktop on afterward.
  //
  // The fix is to stop clicking the theme forward at all: kernel.c's own
  // gui_run loop already calls wall_fetch()/wall_apply() automatically on
  // its very first hlt-loop tick once geo_have is true (right after the
  // same weather_fetch() this loop already runs unconditionally on
  // first pass), so as of the real, live CORS proxy fix, Satellite tiles
  // now load on their own within seconds of boot with ZERO settings
  // interaction required. This step now just opens Settings long enough
  // to show the Wallpaper row genuinely already reading "Satellite" (real
  // proof the automatic default+fetch worked), then closes -- it no
  // longer touches the theme at all.
  async function demoSatelliteWallpaper(gen) {
    if (focused || tourGen !== gen || !adaptersReady) return;
    emulator.mouse_adapter.emu_enabled = true;
    emulator.keyboard_adapter.emu_enabled = true;
    await clickAt(LOGO_X, LOGO_Y); // opens the Apple menu (first click only opens, matches a real user's press+release)
    if (focused || tourGen !== gen) return;
    await sleep(300);
    await clickAt(SETTINGS_MENU_X, SETTINGS_MENU_Y); // selects "Settings"
    if (focused || tourGen !== gen) return;
    await sleep(2000); // real dwell showing the pane -- long enough to actually read the Wallpaper row's real live label, no clicks needed since Satellite is the real default now
    if (focused || tourGen !== gen) return;
    if (emulator.keyboard_send_keys) await emulator.keyboard_send_keys([27], 80); // Escape closes Settings
    await sleep(800);
  }
  function currentGraphical() {
    var vga = emulator.v86 && emulator.v86.cpu.devices.vga;
    return vga ? !!vga.graphical_mode : false;
  }
  // Same signal the outer boot-detection setInterval below already polls
  // (`vga.graphical_mode`), reused here rather than inventing a fixed
  // sleep-and-hope: a reboot is not instant, and clicking a dock icon
  // before the kernel is actually back in its GUI would either land on
  // nothing or hit a stale/garbage framebuffer mid-boot. `timeoutMs` is a
  // failsafe only (a slow/hung reboot shouldn't wedge the tour forever);
  // the real completion signal is always the polled flag matching `want`.
  async function waitForGraphicalMode(gen, want, timeoutMs) {
    var start = Date.now();
    while (Date.now() - start < timeoutMs) {
      if (focused || tourGen !== gen) return false; // a real visitor clicked in mid-reboot: bail, don't fight them for control
      if (currentGraphical() === want) return true;
      await sleep(150);
    }
    return false;
  }
  // Extracted from the old flat sequential loop (v71-v0.76.11 shape):
  // open a single-window app, run its own script, dwell, close via its
  // own real X. Still the only shape Notes/Terminal/Chat support (not
  // gui_multiwin_supported in kernel.c), and still used for Mail/Calendar
  // solo below, just no longer inline in a for-loop so it can be
  // interleaved with the multi-window rounds instead of run as one block
  // after them.
  // v0.76.30: typewriter effect for dynamic headlines. Lightweight manual implementation
  // (no external library dependency, vanilla JS, works in the landing page's static context).
  var typewriterInterval = null;
  var HEADLINE_PREFIX = 'Introducing ';
  function typewriterEffect(element, variablePart, callback) {
    // v0.76.31: direct request, "Introducing" is static, only the
    // variable part (app name + period) animates -- retyping the whole
    // fixed prefix every app switch was noisy and pointless motion.
    if (!element) { if (callback) callback(); return; }
    if (typewriterInterval) clearInterval(typewriterInterval);

    element.textContent = HEADLINE_PREFIX;
    var index = 0;
    var chars = variablePart.split('');

    // Type out the variable part at 60ms per character
    typewriterInterval = setInterval(function() {
      if (index < chars.length) {
        element.textContent += chars[index];
        index++;
      } else {
        clearInterval(typewriterInterval);
        typewriterInterval = null;
        if (callback) callback();
      }
    }, 60);
  }

  function updateHeadline(appName) {
    // v0.76.30: dynamic headline with typewriter animation.
    // Types out "Introducing <AppName>." when app opens, creating a real sense
    // of discovery rather than instant replacement. Lightweight effect, no external deps.
    var h1Link = document.querySelector('h1 a');
    if (h1Link) {
      typewriterEffect(h1Link, appName + '.');
    }
  }
  function resetHeadline() {
    // Default headline when no tour is running or before the tour starts.
    // Uses typewriter effect for visual consistency.
    var h1Link = document.querySelector('h1 a');
    if (h1Link) {
      typewriterEffect(h1Link, 'Joshua Tree.');
    }
  }

  async function runSoloApp(gen, app) {
    if (focused || tourGen !== gen || !adaptersReady) return;
    updateHeadline(app.name); // Update headline as app opens
    var pos = dockSlotPos(app.slot);
    // Drive the emulator's own input even though the visitor hasn't
    // focused: both adapters are gated for real people, not for us. v71
    // real bug found here: libv86.js's keyboard adapter gates EVERY key
    // it sends, including programmatic keyboard_send_text calls, on the
    // same `emu_enabled` flag embed.js sets false on "emulator-ready" and
    // only ever true inside focusIn(). The tour already force-enables
    // mouse_adapter.emu_enabled but never did the keyboard equivalent, so
    // every scripted key the tour ever sent while unfocused was silently
    // swallowed at the source. Fixed by enabling both here.
    emulator.mouse_adapter.emu_enabled = true;
    emulator.keyboard_adapter.emu_enabled = true;
    await clickAt(pos[0], pos[1]); // opens the app, a real dock click
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
  async function tourLoop(gen) {
    tourRunning = true;
    while (!focused && tourGen === gen) {
      // v0.76.29: reset headline at the start of each lap to a default
      resetHeadline();
      await sleep(800); // brief pause before the first app shows, so the headline is visible
      // Direct report (Sep 2026): "the demo doesn't show much application
      // interaction anymore -- it's too focused on the multi window."
      // Real: leading with both multi-window rounds back to back (2
      // rounds x 5 window-management clicks each, zero typed content in
      // the Files+Weather round since neither app takes keyboard input)
      // put ~13 real seconds of pure window-shuffling in front of any
      // actual typing, every single lap. Fixed by interleaving instead of
      // blocking: a real interactive solo app leads, then a multi-window
      // round, then another solo app, then the second (quieter) round,
      // then the rest. Real content is never more than one scene away.
      if (focused || tourGen !== gen || !adaptersReady) return;
      await runSoloApp(gen, MAIL_APP);
      if (focused || tourGen !== gen) return;
      await multiWindowRound(gen, MW_FILES, MW_REMINDERS); // has real typed interaction (Reminders)
      if (focused || tourGen !== gen) return;
      await sleep(1500);
      if (focused || tourGen !== gen) return;
      await runSoloApp(gen, CALENDAR_APP);
      if (focused || tourGen !== gen) return;
      await multiWindowRound(gen, MW_FILES, MW_WEATHER); // both static viewers, no typed interaction -- kept short, see multiWindowRound's own dwell timings
      if (focused || tourGen !== gen) return;
      await sleep(1500);
      for (var i = 0; i < TOUR_APPS.length; i++) {
        if (focused || tourGen !== gen || !adaptersReady) return;
        await runSoloApp(gen, TOUR_APPS[i]);
      }
      // Direct request: "after showing all the apps", so this runs right
      // here, once every real dock app has had its turn and before the
      // satellite wallpaper reveal below.
      await demoAboutPanel(gen);
      if (focused || tourGen !== gen) return;
      // Every app already closed itself before the next opened, the only
      // real shape this kernel's single-window model supports (see
      // runSoloApp above), so the loop is already at "everything closed";
      // a slightly longer pause here just marks it as a deliberate loop
      // boundary rather than app #9.
      await sleep(2500);
      if (focused || tourGen !== gen) return;
      // v0.75: show off the Satellite wallpaper theme once per lap, right
      // before the reboot below wipes state back to kernel.c's own
      // compiled-in default (reset_memory() zeroes RAM, ramfs' persisted
      // SETTINGS.TXT included, so this genuinely has to run every lap,
      // not just once). That default has been WALL_SAT since v0.76.7 --
      // see this function's own header comment for the real v0.76.13 fix
      // to a regression that default change caused here.
      await demoSatelliteWallpaper(gen);
      if (focused || tourGen !== gen) return;
      if (focused || tourGen !== gen) return;
      // The real fix (see the comment above waitForGraphicalMode): reboot
      // the emulator here, at the loop boundary, so the next full cycle
      // starts from a genuinely fresh ramfs instead of piling more mail/
      // reminders onto what every prior cycle already left behind.
      if (bootLogo) bootLogo.hidden = false; // same overlay the initial boot shows; a mid-restart black screen would otherwise look broken, not intentional
      if (!kernelElfBuffer) { try { kernelElfBuffer = await kernelElfFetch; } catch (e) { /* handled below: no buffer means the re-inject step is skipped, not fatal */ } }
      if (emulator.v86 && emulator.v86.cpu && emulator.v86.cpu.reset_memory) emulator.v86.cpu.reset_memory(); // wipe RAM (ramfs included)
      emulator.restart(); // resets CPU registers/devices; harmless no-op on the BIOS reload since there is no BIOS
      // The real fix (see the header comment above `kernelElfBuffer`): v86's
      // own reboot never reloads a no-BIOS multiboot kernel, so redo the
      // same load the constructor's one-shot trick did, putting kmain back
      // in memory so it actually runs again (and ramfs_init with it).
      if (kernelElfBuffer && emulator.v86 && emulator.v86.cpu && emulator.v86.cpu.load_multiboot) emulator.v86.cpu.load_multiboot(kernelElfBuffer);
      await waitForGraphicalMode(gen, false, 3000); // best-effort: the reboot leaving graphical mode (BIOS/kernel text-mode init) briefly, same transition the very first boot goes through
      if (focused || tourGen !== gen) return;
      await waitForGraphicalMode(gen, true, 20000); // the real wait: don't click a dock icon until the kernel has actually reached its GUI again
      if (focused || tourGen !== gen) return;
      await sleep(600); // let the fresh desktop's first frame draw, same beat already used after every dock click above
    }
  }
  function startTourWhenReady() {
    if (focused) return;
    tourLoop(tourGen)
      .finally(function () { tourArmed = false; }) // reset tourArmed if tourLoop exits for any reason, so the tour can restart
      .catch(function () { /* a torn-down emulator mid-await (e.g. a real navigation) shouldn't spam the console */ });
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
