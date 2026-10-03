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
  // Phone: a narrow visitor boots "phone samantha" instead, which opens a
  // real 430x760 portrait mode at scale 1 (kernel.c's boot_to_phone), not
  // the desktop's 960x540 shrunk to fit -- so this file's own logical
  // space has to switch to match, 1:1, instead of downscaling a desktop.
  var IS_PHONE = typeof matchMedia === "function" && matchMedia("(max-width: 520px)").matches;
  var LOGICAL_W = IS_PHONE ? 430 : 960, LOGICAL_H = IS_PHONE ? 760 : 540;
  // Pixel-exact desktop: at dpr 1 with a big enough box, ask the kernel for a
  // physical mode equal to the box in device px ("res=WxH", W multiple of 8,
  // both even; kernel opens logical W/2 x H/2 at scale 2) so no CSS scaling.
  var RES_TOKEN = "", RES_W = 0, RES_H = 0;
  (function () {
    var dpr0 = window.devicePixelRatio || 1;
    var sc0 = document.getElementById("screen_container") || container;
    var bw = sc0.clientWidth * dpr0, bh = sc0.clientHeight * dpr0;
    if (IS_PHONE || dpr0 !== 1 || bw < 1600 || bh < 900) return;
    var rw = Math.min(3840, Math.floor(bw / 8) * 8), rh = Math.min(2160, Math.floor(bh / 2) * 2);
    RES_W = rw; RES_H = rh; RES_TOKEN = "res=" + rw + "x" + rh + " ";
    LOGICAL_W = rw / 2; LOGICAL_H = rh / 2;
  })();
  // 1.7.16: one cmdline for the first boot AND every tour-lap reboot. reinjectKernel
  // used to reload the kernel with no cmdline, so after lap 1 a phone visitor got a
  // letterboxed desktop (no "phone"), and everyone lost facehost.
  var BOOT_CMDLINE = (IS_PHONE ? "phone samantha " : "") + RES_TOKEN + (/[?&]portfolio\b/.test(location.search) ? "portfolio samantha " : "") + (/[?&]samantha\b/.test(location.search) ? "samantha " : "") + "facehost=joshuatree.heyitsmejosh.com";
  var GLIDE_MAX_MS = 700; // longest tour cursor glide, see moveCursorTo
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
  var toolCounts = {}; // every "chattool=<tool>:" line, counted as it arrives, so a check never depends on the window still holding it
  var faceReady = false;   // ring-3 Joshua wrote "samface: idle ready": his idle frames are in, the live face can take over from the intro video
  var speakCount = 0, lastSpeakBytes = 0; // every "speak: status=200 bytes=N" line, counted as it arrives (serialLog is a head+rolling-tail window, see the serial0 listener)
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
  // v0.82.x: real report, "landing page demo is taking a while to load even
  // when the rest of the page already finishes loading, especially on
  // mobile." Root cause: this fetch (2.9MB) used to start right here,
  // unconditionally, the instant this script was parsed, racing the rest
  // of the page's own load (hero fonts, CSS, feature-section images) for
  // the same real, often mobile-constrained, pipe, whether or not the
  // visitor could even see the demo yet -- same story for `new V86(...)`
  // a bit further down, which itself fetches v86.wasm (2MB). Both now
  // start inside startEmulator() below, only once the demo container is
  // visible or nearly so (see the IntersectionObserver gate right after
  // it), the same "don't do the expensive thing until it's relevant"
  // pattern index.html's own reveal-on-scroll already uses for its
  // sections. `kernelElfFetch` stays declared here (not inside
  // startEmulator) since a couple of far-later call sites
  // (idleRestartTimeout's soft-reset, tourLoop's own reboot) read it by
  // closure and only ever run once startEmulator has actually set it.
  var kernelElfFetch = null;
  var screenContainer = document.getElementById("screen_container");
  var screenText = document.getElementById("screen_text");
  var screenCanvas = document.getElementById("screen_canvas");
  var overlay = document.getElementById("v86-overlay");

  // 1.7.14: mobile audio debugging. Samantha's voice still doesn't play on
  // a real iPhone even after 1.7.6's touchend/click unlock, and we can't
  // test a real phone from here. ?audiodebug shows a small fixed overlay
  // with everything needed to tell "AudioContext is locked" apart from
  // "the context runs but v86 never got any samples" (kernel/SB16 side)
  // from a screenshot the visitor sends back. Plain text, no emoji, no
  // libraries: this has to survive on whatever ships in the PR, untouched
  // by anything else on the page.
  var AUDIO_DEBUG = /[?&]audiodebug\b/.test(location.search);
  var audioDebugState = { unlockAttempts: 0, lastUnlockEvent: "-", chunkCount: 0, lastLevel: 0 };
  var audioDebugEl = null;
  // 1.8.4: splitting "her voice never played on a real iPhone" in two
  // without ever hearing a real iPhone ourselves. A tone button proves (or
  // disproves) plain AudioContext output on the visitor's own phone,
  // separate from whether v86's SB16 emulation ever gets that far; a live
  // meter tapped after the real speaker output means one screenshot shows
  // both halves at once instead of a back-and-forth over text.
  var audioMeterState = { analyser: null, data: null, rms: 0, peak: 0 };
  var audioWorkletState = { moduleRequested: false, moduleAdded: false, nodeCreated: false };
  var audioDebugToneCtx = null; // only created if v86's own context doesn't exist yet
  if (AUDIO_DEBUG) {
    // Tap whatever actually reaches an AudioContext's real output. v86's
    // speaker adapter (and the tone button below) both eventually call
    // .connect(ctx.destination); wrapping AudioNode.prototype.connect is
    // the only way to insert an AnalyserNode on that path without touching
    // v86's own vendored code. One analyser per context, reused across
    // every node that connects to that context's destination.
    var origConnect = AudioNode.prototype.connect;
    AudioNode.prototype.connect = function () {
      var dest = arguments[0];
      var ctx = this.context;
      try {
        if (ctx && dest === ctx.destination) {
          if (!ctx.__jtDebugAnalyser) {
            var analyser = ctx.createAnalyser();
            analyser.fftSize = 2048;
            ctx.__jtDebugAnalyser = analyser; // set before connecting: connect() below re-enters this same wrapper
            origConnect.call(this, analyser);
            origConnect.call(analyser, ctx.destination);
            audioMeterState.analyser = analyser;
            audioMeterState.data = new Float32Array(analyser.fftSize);
          } else {
            origConnect.call(this, ctx.__jtDebugAnalyser);
          }
        }
      } catch (e) {}
      return origConnect.apply(this, arguments);
    };
    // AudioWorklet status: did dac-processor's module ever get requested/
    // resolved, and was a node for it ever constructed. Both answer "is
    // the worklet path even running" without needing devtools on a phone.
    if (window.AudioWorklet && AudioWorklet.prototype.addModule) {
      var origAddModule = AudioWorklet.prototype.addModule;
      AudioWorklet.prototype.addModule = function () {
        audioWorkletState.moduleRequested = true;
        var p = origAddModule.apply(this, arguments);
        p.then(function () { audioWorkletState.moduleAdded = true; }).catch(function () {});
        return p;
      };
    }
    if (window.AudioWorkletNode) {
      var OrigAudioWorkletNode = window.AudioWorkletNode;
      window.AudioWorkletNode = function () {
        audioWorkletState.nodeCreated = true;
        return Reflect.construct(OrigAudioWorkletNode, arguments, new.target || window.AudioWorkletNode);
      };
      window.AudioWorkletNode.prototype = OrigAudioWorkletNode.prototype;
    }
    // Meter polls fast (rAF) so a 0.5s test tone isn't missed between
    // ticks; the text render below stays on its own slower interval.
    var meterTick = function () {
      var m = audioMeterState;
      if (m.analyser && m.data) {
        m.analyser.getFloatTimeDomainData(m.data);
        var sum = 0;
        for (var i = 0; i < m.data.length; i++) sum += m.data[i] * m.data[i];
        var rms = Math.sqrt(sum / m.data.length);
        m.rms = rms;
        if (rms > m.peak) m.peak = rms;
      }
      requestAnimationFrame(meterTick);
    };
    requestAnimationFrame(meterTick);

    audioDebugEl = document.createElement("div");
    audioDebugEl.id = "audio-debug-overlay";
    audioDebugEl.style.cssText = "position:fixed;top:8px;left:8px;z-index:99999;background:#fff;color:#111;border:1px solid #111;padding:8px 10px;font:12px/1.5 -apple-system,Helvetica,Arial,sans-serif;pointer-events:none;max-width:80vw;";
    var audioDebugText = document.createElement("div");
    audioDebugText.style.cssText = "white-space:pre;";
    var toneButton = document.createElement("button");
    toneButton.id = "audio-debug-tone-btn";
    toneButton.textContent = "Play test tone";
    toneButton.style.cssText = "pointer-events:auto;display:block;width:100%;margin-top:8px;padding:12px 10px;font:13px/1 -apple-system,Helvetica,Arial,sans-serif;background:#111;color:#fff;border:1px solid #111;cursor:pointer;";
    var toneStatus = document.createElement("div");
    toneStatus.style.cssText = "white-space:pre-wrap;margin-top:4px;";
    audioDebugEl.appendChild(audioDebugText);
    audioDebugEl.appendChild(toneButton);
    audioDebugEl.appendChild(toneStatus);
    document.body.appendChild(audioDebugEl);

    // Plays through whatever context v86 will actually use (creating one
    // early, with a note, if the emulator hasn't started yet). If the
    // visitor hears this but not Samantha, the problem is v86/dac-processor,
    // not iOS's audio policy; if this is silent too, it never was v86 at
    // all.
    var playTestTone = function () {
      var ac = emulator && emulator.speaker_adapter && emulator.speaker_adapter.audio_context;
      var createdNew = false;
      if (!ac) {
        try {
          audioDebugToneCtx = audioDebugToneCtx || new (window.AudioContext || window.webkitAudioContext)();
          ac = audioDebugToneCtx;
          createdNew = true;
        } catch (e) {
          toneStatus.textContent = "tone: could not create an AudioContext - " + e.message;
          return;
        }
      }
      try {
        if (ac.state !== "running") ac.resume().catch(function () {});
        var osc = ac.createOscillator();
        osc.type = "sine";
        osc.frequency.value = 440;
        var gain = ac.createGain();
        gain.gain.value = 0.2;
        osc.connect(gain);
        gain.connect(ac.destination);
        var now = ac.currentTime;
        osc.start(now);
        osc.stop(now + 0.5);
        toneStatus.textContent = "tone: playing 440hz for 0.5s" +
          (createdNew ? " (v86's own context did not exist yet, created a new one)" : " (same context v86's speaker uses)");
      } catch (e) {
        toneStatus.textContent = "tone: error - " + e.message;
      }
    };
    toneButton.addEventListener("click", playTestTone);
    toneButton.addEventListener("touchend", function (ev) { ev.preventDefault(); playTestTone(); }, { passive: false });

    var renderAudioDebug = function () {
      var ac = emulator && emulator.speaker_adapter && emulator.speaker_adapter.audio_context;
      var lastSpeak = /speak: status=\d+[^\n]*/g;
      var speakMatches = serialLog.match(lastSpeak);
      var lines = [
        "audio debug",
        "context state: " + (ac ? ac.state : "no speaker_adapter"),
        "sample rate: " + (ac ? ac.sampleRate : "-"),
        "base latency: " + (ac && ac.baseLatency !== undefined ? ac.baseLatency : "-"),
        "output latency: " + (ac && ac.outputLatency !== undefined ? ac.outputLatency : "-"),
        "audioSession.type: " + (navigator.audioSession ? navigator.audioSession.type : "n/a"),
        "unlock attempts: " + audioDebugState.unlockAttempts + " (last: " + audioDebugState.lastUnlockEvent + ")",
        "dac chunks received: " + audioDebugState.chunkCount,
        "last non-zero sample: " + audioDebugState.lastLevel,
        "worklet module: requested=" + audioWorkletState.moduleRequested + " added=" + audioWorkletState.moduleAdded,
        "worklet node created: " + audioWorkletState.nodeCreated,
        "output meter: rms=" + audioMeterState.rms.toFixed(4) + " peak=" + audioMeterState.peak.toFixed(4),
        "last serial speak line: " + (speakMatches ? speakMatches[speakMatches.length - 1] : "none yet")
      ];
      audioDebugText.textContent = lines.join("\n");
    };
    setInterval(renderAudioDebug, 200);
    renderAudioDebug();
  }

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
  // v0.82.x: still correct now that `new V86()` itself is deferred (see
  // startEmulator below) -- "before v86's own listener" only ever meant
  // "before v86 attaches ITS listener", not "before this script finishes
  // running". v86 has no listener to race against until its own
  // constructor actually executes, whenever that turns out to be, and this
  // one is still registered here, eagerly, at parse time, so it's always
  // first in `window`'s capture-phase FIFO order no matter how much later
  // construction happens.
  window.addEventListener("keydown", function (ev) {
    if (ev.key === "Escape" || ev.keyCode === 27 || ev.code === "Escape") {
      ev.preventDefault();
      ev.stopImmediatePropagation();
      // Shift+Escape is the visitor's way out of the demo (WCAG 2.1.2, no
      // keyboard trap): it gives the keyboard back to the page. Plain Escape
      // still never reaches the kernel.
      if (ev.shiftKey && focused) releaseKeyboard();
    }
  }, true); // capture phase, BEFORE v86's own global listener (both on window, FIFO order)

  // Deferred, real construction: everything below used to run right here,
  // unconditionally, at parse time. It's now wrapped in startEmulator(),
  // called for real once (see the IntersectionObserver right after it,
  // and the `emulator` v0.82.x comment above `kernelElfFetch` for the
  // real report and root cause this whole gate exists for).
  var emulator = null;
  var adaptersReady = false;
  var bootStart = 0; // set inside startEmulator, not here: the boot-detection setInterval's own "stuck in text mode" fallback measures elapsed time since boot actually STARTED, and boot no longer starts at page load
  var emulatorStarting = false;
  function startEmulator() {
    if (emulatorStarting) return; // idempotent: construction now waits on the kernel fetch, so `emulator` stays null for a moment
    emulatorStarting = true;
    bootStart = Date.now();
    // The kernel is fetched exactly once, gzipped, and handed to v86 as a
    // buffer. It used to be fetched twice (here, and again by v86 from
    // multiboot.url), 7MB on any browser that doesn't share the in-flight
    // request, and served uncompressed either way.
    kernelElfFetch = loadKernel().then(function (buf) {
      kernelElfBuffer = buf;
      return buf;
    }).catch(function () { return null; });
    kernelElfFetch.then(function (buf) {

    emulator = new V86({
    wasm_path: "v86/v86.wasm",
    memory_size: 64 * 1024 * 1024, // 1.6.14: 32MB left no room for a full spoken reply once her face frames were loaded
    vga_memory_size: 16 * 1024 * 1024, // v41: 1600x1200x32bpp is 7.68MB, 8 was one bad rounding away from failing
    screen_container: screenContainer,
    multiboot: buf ? { buffer: buf.slice(0) } : { url: "v86/kernel.elf" },
    // facehost= is always on, portfolio mode is still opt-in via ?portfolio:
    // kernel/chat_face.h only turns Samantha's Chat face on when this exact
    // token is present, and the frames it fetches (idle-0..3, talk-0..7 under
    // /face/) are served by this same Cloudflare deploy at
    // joshuatree.heyitsmejosh.com (landing/face/*.png, wrangler.toml's
    // [assets] binding), reached through the guest's plain-HTTP stack over
    // the same cors_proxy relay below -- worker.js's handleProxy has a
    // matching /face/ path exception for that host, same shape as its
    // existing /api/stocks|/api/quotes|/api/deals allowance.
    // ?samantha, same opt-in shape as ?portfolio right above: kernel.c's
    // boot_to_samantha reads this exact token and skips the desktop for
    // Chat's full-screen avatar view (kernel/chat.h's chat_boot_samantha_open).
    // IS_PHONE (narrow viewport, no ?param needed) always adds "phone ",
    // which itself forces boot_to_samantha in the kernel -- a phone
    // visitor gets her portrait view unconditionally, desktop visitors
    // are untouched and still need ?samantha to opt in.
    cmdline: BOOT_CMDLINE, // kmain reads this and puts Joshua's own apps on the dock
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
    window.__joshuaTreeEmulator = emulator; // for debugging from the console, harmless to leave; moved here (was a bottom-of-file assignment) since construction itself now happens in here, not synchronously as this script parses
    if (AUDIO_DEBUG && emulator.bus) {
      // libv86.js's SB16 device (`D.prototype.dac_send`) fires this bus
      // event with two shifted sample blocks every time it hands real
      // audio to the mixer/AudioContext side; counting it (and the last
      // non-zero sample it carried) is the only way to tell, on a phone we
      // can't attach devtools to, whether the kernel/SB16 side ever sent
      // anything at all versus the AudioContext just being locked.
      emulator.bus.register("dac-send-data", function (data) {
        audioDebugState.chunkCount++;
        var block = data && data[0];
        if (block) {
          for (var i = block.length - 1; i >= 0; i--) {
            if (block[i]) { audioDebugState.lastLevel = block[i]; break; }
          }
        }
      });
    }

    // keyboard_adapter/mouse_adapter aren't attached synchronously: V86's
    // constructor kicks off the wasm load and only wires them up once the
    // CPU is actually built, so touching them right after `new V86()` throws.
    // "emulator-ready" fires once they exist.
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
    // 2.0.0: the guest fetching through /api/proxy is an app at work (Samantha's
    // 72 face frames take longer than 15s to arrive and need no click), so it
    // counts as the visitor still being here. Without this the kiosk reset
    // below rebooted the demo mid-load, every time, with no kernel fault at all
    // (docs/wip/samantha-reset.md). v86's "fetch" relay goes through window.fetch.
    if (typeof window.fetch === "function" && !window.__jtFetchWrapped) {
      var realFetch = window.fetch.bind(window);
      window.__jtFetchWrapped = true;
      window.fetch = function (input, init) {
        var u = typeof input === "string" ? input : (input && input.url) || "";
        if (u.indexOf("/api/proxy") !== -1) lastInteractionTime = Date.now();
        return realFetch(input, init);
      };
    }
    var serialLine = "";
    emulator.add_listener("serial0-output-byte", function (b) {
      // Head + rolling tail: the first 16KB (boot probes) are kept forever, the
      // rest is a window over the newest ~48-112KB. A hard 64KB cap used to
      // freeze the log mid-boot on the syscall trace, so every later
      // chattool=/chatreply= marker never appeared.
      serialLog += String.fromCharCode(b);
      if (serialLog.length > 131072) serialLog = serialLog.slice(0, 16384) + serialLog.slice(-49152);
      // Samantha answering or speaking counts as the visitor still being
      // here: without this the 15s kiosk reset rebooted the demo in the
      // middle of her spoken reply, since listening involves no clicks.
      if (b === 10) {
        var m = /^(?:syscall: write\(1\) from ring 3: )?speak: status=200 bytes=(\d+)/.exec(serialLine); // ring-3 Samantha's writes arrive behind the kernel's syscall trace prefix
        if (m) { speakCount++; lastSpeakBytes = Number(m[1]); }
        if (/samface: idle ready/.test(serialLine)) faceReady = true;
        if (m) lastInteractionTime = Date.now() + Math.ceil(Number(m[1]) / 16); // 16000 samples/s = 16 per ms
        else if (/^(?:syscall: write\(1\) from ring 3: )?(?:chatreply=|chattool=)/.test(serialLine)) {
          lastInteractionTime = Date.now();
          var tm = /^(?:syscall: write\(1\) from ring 3: )?chattool=([a-z_]+):/.exec(serialLine);
          if (tm) toolCounts[tm[1]] = (toolCounts[tm[1]] || 0) + 1;
        }
        serialLine = "";
      } else if (b !== 13 && serialLine.length < 80) serialLine += String.fromCharCode(b);
    });
    });
  }

  // Gzipped kernel through DecompressionStream, raw ELF when the browser
  // lacks it or the .gz is missing. The magic-byte check covers a host
  // that already decoded it via Content-Encoding.
  function loadKernel() {
    function raw() { return fetch("v86/kernel.elf").then(function (r) { return r.arrayBuffer(); }); }
    if (typeof DecompressionStream !== "function") return raw();
    return fetch("v86/kernel.elf.gz").then(function (r) {
      if (!r.ok) throw new Error("no gz");
      return r.arrayBuffer();
    }).then(function (buf) {
      var b = new Uint8Array(buf, 0, 2);
      if (b[0] !== 0x1f || b[1] !== 0x8b) return buf;
      return new Response(new Blob([buf]).stream().pipeThrough(new DecompressionStream("gzip"))).arrayBuffer();
    }).catch(raw);
  }

  // Start a little before the visitor actually scrolls to it (600px
  // rootMargin, roughly a full extra phone-screen of lead time), not the
  // instant it's 100% in view -- a visitor scrolling normally should
  // already have a booting kernel by the time the demo settles on screen,
  // not a blank box that only then starts fetching. On the common case
  // here (the demo fills the hero, the very top of the page -- see
  // index.html's #v86-embed) this observer's first callback fires within
  // the same frame it starts observing, so nothing here delays the demo
  // at all when it's already the first thing a visitor sees; the gate
  // only ever matters for whatever pushed the container further down (a
  // narrower/taller hero variant, a deep link, etc). Same real,
  // already-established pattern index.html's own reveal-on-scroll uses
  // for its sections (IntersectionObserver, "don't do the expensive thing
  // until it's relevant"), applied here to the actual heavy work instead
  // of a CSS class toggle.
  // ?full is the portfolio frame: the machine is the whole page, so there is
  // nothing to scroll to and no reason to wait. Boot now. Real report: X's
  // in-app browser never started it, and an observer inside a cross-origin
  // iframe is the one piece that depends on the host app's WebView.
  // setTimeout, not a direct call: startEmulator reads vars declared further
  // down this script, same as the observer callback always did.
  if (/[?&]full\b/.test(location.search)) {
    setTimeout(startEmulator, 0);
  } else if ("IntersectionObserver" in window) {
    var startObserver = new IntersectionObserver(function (entries) {
      for (var oi = 0; oi < entries.length; oi++) {
        if (entries[oi].isIntersecting) {
          startEmulator();
          startObserver.disconnect();
          break;
        }
      }
    }, { rootMargin: "600px 0px" });
    startObserver.observe(container);
  } else {
    startEmulator(); // no IntersectionObserver support: fail open rather than never booting the demo at all
  }

  var focused = false;
  var idleRestartTimeout = 0;
  var KIOSK_IDLE_MS = 60000; // was 15s: a visitor reading her reply got rebooted to the boot logo mid-conversation
  var lastInteractionTime = Date.now();
  function resetIdleRestart() {
    if (idleRestartTimeout) clearTimeout(idleRestartTimeout);
    if (!focused) return; // only auto-reset if visitor has taken control
    idleRestartTimeout = setTimeout(async function () {
      // Retail-kiosk style: after 60 seconds of inactivity, close windows and restart the tour.
      // Only trigger if KIOSK_IDLE_MS has passed since the last user interaction (click, movement, key).
      var timeSinceActivity = Date.now() - lastInteractionTime;
      if (focused && !tourRunning && timeSinceActivity < KIOSK_IDLE_MS) { idleRestartTimeout = 0; resetIdleRestart(); return; } // still busy (e.g. she's talking): check again later
      if (focused && !tourRunning && timeSinceActivity >= KIOSK_IDLE_MS) { // only if still focused, tour not running, and truly idle
        // Trigger a soft reset: close any open windows by rebooting the emulator
        // then restart the tour
        if (bootLogo) bootLogo.hidden = false;
        if (!kernelElfBuffer) { try { kernelElfBuffer = await kernelElfFetch; } catch (e) {} }
        if (emulator.v86 && emulator.v86.cpu && emulator.v86.cpu.reset_memory) emulator.v86.cpu.reset_memory();
        emulator.restart();
        reinjectKernel();
        // Reset focused and tourArmed to allow the idle tour to restart
        focused = false;
        tourArmed = false;
      }
      idleRestartTimeout = 0;
    }, KIOSK_IDLE_MS);
  }
  function focusIn() {
    if (focused || !adaptersReady) return;
    focused = true;
    if (introVideo) introVideo.pause(), introVideo.dispatchEvent(new Event("fade"));   // a visitor took over: the OS, not the recording
    emulator.keyboard_adapter.emu_enabled = true;
    emulator.mouse_adapter.emu_enabled = true;
    // Browsers block audio until a real user gesture. v86 builds
    // speaker_adapter (and its AudioContext) unconditionally in its
    // constructor (libv86.js: `b.disable_speaker||(this.speaker_adapter=new
    // jb(this.bus))`, never passed here, so it's always on) and already
    // calls audio_context.resume() on its own "emulator-started" event, but
    // that fires from autostart, not from a click, so a real browser leaves
    // the context "suspended" and Chat's speak.c audio never comes out.
    // This is the actual user gesture (focusIn only ever runs from a real
    // mousedown/touchstart/keydown), so resume it here too; a second
    // resume() on an already-running context is a harmless no-op.
    if (emulator.speaker_adapter && emulator.speaker_adapter.audio_context
        && emulator.speaker_adapter.audio_context.state !== "running") {
      emulator.speaker_adapter.audio_context.resume().catch(function () {});
    }
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
  // 1.7.6: iPhone audio. focusIn's resume() runs once, from touchstart,
  // which iOS does not count as a gesture that may start audio (touchend
  // and click do), so the context stayed suspended and focusIn never tried
  // again. Retry on every real tap until it runs. audioSession "playback"
  // (Safari 16.4+) keeps her voice audible with the silent switch on, the
  // way a video would be.
  // 1.7.14: still silent on a real iPhone after 1.7.6. Two more gaps:
  // (1) the listeners were on `container`, but a visitor's first real tap
  // often lands on `overlay` (the "click to start" layer) or the
  // "Full screen" button, both of which sit on top of/beside container and
  // never bubble a click to it -- listen on `document` instead, in the
  // capture phase, so every tap anywhere on the page counts, whatever it
  // hits. (2) resume() alone doesn't always stick on iOS Safari: the
  // standard unlock idiom plays one frame of silence through the same
  // context, which is what actually flips WebKit's internal "this context
  // was unlocked by a gesture" bit. (3) iOS can drop a running context back
  // to "interrupted" (a phone call, Siri, switching apps) with no gesture
  // to resume it on -- catch that on visibilitychange/pageshow too.
  function silentPrime(ac) {
    try {
      var buf = ac.createBuffer(1, 1, ac.sampleRate);
      var src = ac.createBufferSource();
      src.buffer = buf;
      src.connect(ac.destination);
      src.start(0);
    } catch (e) {}
  }
  function unlockAudio(ev) {
    audioDebugState.unlockAttempts++;
    audioDebugState.lastUnlockEvent = ev ? ev.type : "?";
    try { if (navigator.audioSession) navigator.audioSession.type = "playback"; } catch (e) {}
    var ac = emulator && emulator.speaker_adapter && emulator.speaker_adapter.audio_context;
    if (!ac) return;
    if (ac.state !== "running") {
      ac.resume().catch(function () {});
      silentPrime(ac);
    }
  }
  document.addEventListener("pointerup", unlockAudio, { capture: true, passive: true });
  document.addEventListener("touchend", unlockAudio, { capture: true, passive: true });
  document.addEventListener("click", unlockAudio, { capture: true, passive: true });
  document.addEventListener("keydown", unlockAudio, { capture: true, passive: true });
  document.addEventListener("visibilitychange", function () {
    if (document.visibilityState !== "visible") return;
    var ac = emulator && emulator.speaker_adapter && emulator.speaker_adapter.audio_context;
    if (ac && ac.state === "interrupted") ac.resume().catch(function () {});
  });
  window.addEventListener("pageshow", function () {
    var ac = emulator && emulator.speaker_adapter && emulator.speaker_adapter.audio_context;
    if (ac && ac.state !== "running") ac.resume().catch(function () {});
  });
  // 1.8.x: tap to talk. iPhone plays no sound before a tap, and the phone
  // intro used to have Samantha answer before anyone touched the page, so her
  // first line was thrown away (measured in the iOS Simulator: context
  // "interrupted", speak 200, zero DAC chunks). On phones the intro now waits
  // for this button; the tap unlocks audio (the document listeners above run
  // first) and she speaks right after. The button eats its own touch so the
  // tap doesn't count as a visitor takeover (focusIn would stop the tour).
  function audioRunning() {
    var ac = emulator && emulator.speaker_adapter && emulator.speaker_adapter.audio_context;
    return !!ac && ac.state === "running";
  }
  var tapTalkBtn = null, tapTalkResolve = null, introVideo = null;
  var tapTalkPromise = new Promise(function (r) { tapTalkResolve = r; });
  if (IS_PHONE || /[?&]portfolio\b/.test(location.search)) {   // PORTFOLIO_MODE is assigned further down, still undefined here
    tapTalkBtn = document.createElement("button");
    tapTalkBtn.type = "button";
    tapTalkBtn.id = "tap-to-talk";
    tapTalkBtn.textContent = /[?&]portfolio\b/.test(location.search) ? "Tap to hear Joshua" : "Tap to hear Samantha";
    tapTalkBtn.hidden = true;
    tapTalkBtn.style.cssText = "position:absolute;left:50%;bottom:" + (IS_PHONE ? 64 : 120) + "px;transform:translateX(-50%);z-index:7;" +
      (/[?&]portfolio\b/.test(location.search) ? "background:#fff;color:#000;" : "background:var(--fg);color:var(--bg);") + "border:none;border-radius:999px;padding:14px 22px;min-height:44px;" +   /* portfolio: his sweater is dark, a dark pill vanishes on it */
      "font:600 15px/1 -apple-system,Helvetica,Arial,sans-serif;letter-spacing:0.01em;cursor:pointer;" +
      "box-shadow:0 4px 18px rgba(0,0,0,0.18);white-space:nowrap;";
    tapTalkBtn.style.display = "none";   // the speaker button, top right, is how a visitor starts the sound; the first tap anywhere still counts
    ["touchstart", "mousedown", "pointerdown"].forEach(function (t) {
      tapTalkBtn.addEventListener(t, function (ev) { ev.stopPropagation(); }, { passive: true });
    });
    tapTalkBtn.addEventListener("click", function (ev) {
      ev.stopPropagation();
      unlockAudio(ev);
      tapTalkBtn.hidden = true;
      tapTalkResolve();
    });
    container.appendChild(tapTalkBtn);
    // Any first click or key anywhere counts as the tap; the button is just the hint.
    // That first gesture is swallowed (600 ms covers its mousedown, mouseup and click):
    // reaching the container it read as a visitor taking over, which stopped the intro
    // before he said a word.
    // Portfolio intro: one continuous lip-synced take of his 30 s line, played muted on a
    // loop over the booting kernel until the first tap, then from the top with sound. The
    // kernel's frame-stitched face ghosted and jumped between clips on a line this long;
    // it still answers live once the video hands over to the OS.
    if (/[?&]portfolio\b/.test(location.search)) {
      introVideo = document.createElement("video");
      introVideo.src = "face-joshua/intro.mp4";
      introVideo.muted = introVideo.loop = introVideo.autoplay = introVideo.playsInline = true;
      introVideo.style.cssText = "position:absolute;inset:0;width:100%;height:100%;object-fit:cover;z-index:6;background:#e9e2d4;transition:opacity .6s;";
      container.appendChild(introVideo);
      introVideo.addEventListener("error", function () { introVideo.dispatchEvent(new Event("fade")); });   // missing or unplayable: the live kernel face takes the intro as before
      introVideo.addEventListener("fade", function () {   // the intro scene fires this once the desktop is underneath; a natural end just holds the last frame
        var v = introVideo; if (!v) return; introVideo = null;
        v.style.opacity = "0"; setTimeout(function () { v.remove(); }, 700);
      });
    }
    var firstTapAt = 0, repaintMute = null;
    // The first gesture starts the sound: audio unlocked, the intro video restarted from the top with its own audio on.
    // The speaker button is that gesture too (on a phone it is the obvious thing to press), so it calls this as well.
    var startSound = function (ev) {
      if (firstTapAt) return;
      firstTapAt = Date.now(); unlockAudio(ev); tapTalkBtn.hidden = true; tapTalkResolve(); if (repaintMute) repaintMute();
      if (introVideo) { introVideo.loop = false; introVideo.currentTime = 0; introVideo.muted = false; introVideo.play().catch(function () {}); }
    };
    ["pointerdown", "mousedown", "mouseup", "click", "touchstart", "touchend", "keydown", "keyup"].forEach(function (t) {
      document.addEventListener(t, function (ev) {
        if (ev.target && ev.target.closest && ev.target.closest("#jt-mute")) return;   // the mute button is its own control, not the first tap
        startSound(ev);
        if (Date.now() - firstTapAt < 600) ev.stopPropagation();
      }, { capture: true, passive: true });
    });
  }
  // Portfolio voice is on by default; this is the way out. Top right, a round
  // glass button that mutes v86's master volume (mixer.set_volume 0/1), kept
  // across visits. A browser will not play sound before a gesture, so the first
  // tap or key anywhere (or the "Tap to hear" button) is what actually starts it.
  var muteBtn = null, muted = false;
  function applyMute() {
    var mx = emulator && emulator.speaker_adapter && emulator.speaker_adapter.mixer;
    if (mx) mx.set_volume(muted ? 0 : 1, 2);
    return !!mx;
  }
  if (/[?&]portfolio\b/.test(location.search) || IS_PHONE) {
    try { muted = localStorage.getItem("jt-muted") === "1"; } catch (e) {}
    muteBtn = document.createElement("button");
    muteBtn.type = "button";
    muteBtn.id = "jt-mute";
    muteBtn.style.cssText = "position:absolute;top:10px;right:" + (/[?&]portfolio\b/.test(location.search) ? 10 : 56) + "px;z-index:8;width:36px;height:36px;padding:0;border:none;border-radius:50%;" +
      "display:flex;align-items:center;justify-content:center;background:rgba(28,28,30,0.78);color:#fff;cursor:pointer;" +
      "-webkit-backdrop-filter:blur(14px);backdrop-filter:blur(14px);box-shadow:0 1px 6px rgba(0,0,0,0.18);";
    var paintMute = function () {
      muteBtn.setAttribute("aria-pressed", muted ? "true" : "false");
      muteBtn.setAttribute("aria-label", muted ? "Unmute Joshua" : "Mute Joshua");
      muteBtn.title = muted ? "Unmute" : "Mute";
      muteBtn.innerHTML = '<svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true">' +
        '<path d="M11 5 6 9H3v6h3l5 4z" fill="currentColor"/>' +
        ((muted || !firstTapAt) ? '<path d="m16 9 5 6M21 9l-5 6"/>' : '<path d="M15.5 8.5a5 5 0 0 1 0 7M18.5 5.5a9 9 0 0 1 0 13"/>') + '</svg>';
    };
    paintMute(); repaintMute = paintMute;
    ["touchstart", "mousedown", "pointerdown"].forEach(function (t) {
      muteBtn.addEventListener(t, function (ev) { ev.stopPropagation(); }, { passive: true });
    });
    muteBtn.addEventListener("click", function (ev) {
      ev.stopPropagation();
      if (typeof startSound === "function" && !firstTapAt) { if (muted) { muted = false; try { localStorage.setItem("jt-muted", "0"); } catch (e) {} applyMute(); paintMute(); } startSound(ev); return; }   // a first press means "let me hear him", never "mute"
      muted = !muted;
      if (introVideo) introVideo.muted = muted;   // the intro video carries its own audio, the emulator's mixer is not it
      try { localStorage.setItem("jt-muted", muted ? "1" : "0"); } catch (e) {}
      applyMute(); paintMute();
    });
    container.appendChild(muteBtn);
    // The mixer exists once the emulator has booted; apply a remembered mute as soon as it does.
    var muteTries = 0, muteTimer = setInterval(function () { if (applyMute() || ++muteTries > 120) clearInterval(muteTimer); }, 500);
  }
  // Phones have no keyboard for the demo to listen to, and v86 only hears
  // `keydown` on window, so the Samantha, Notes and Terminal screens, which
  // all want typed keys, could be tapped but never typed into. On phones the
  // page shows a real chat bar under the demo (#demo-composer: a text field
  // and a Send button). A real field is what makes iOS and Android raise
  // their own keyboard. What is typed in it is mirrored into the kernel's
  // own input line as it changes, so the kernel shows it live, and Send is
  // the Enter key. The field is compared with what the kernel already holds,
  // so autocorrect, a pasted word or a cursor edit all work: only the
  // difference is sent, as Backspaces then new letters.
  // Phones, and touch tablets (an iPad runs the desktop layout but has no
  // keyboard either).
  if (IS_PHONE || (typeof matchMedia === "function" && matchMedia("(pointer: coarse)").matches)) {
    var composer = document.getElementById("demo-composer");
    var composeInput = document.getElementById("demo-compose-input");
    if (composer && composeInput) {
      composer.hidden = false;
      if (/[?&]portfolio\b/.test(location.search)) composeInput.placeholder = "Message Joshua";   // portfolio mode is his site
      var frameEl = document.getElementById("demo-frame");
      if (frameEl) frameEl.classList.add("has-composer");
      var mirrored = ""; // what the kernel's input line holds now
      // One queue, one key at a time at the tour's own 60ms spacing: sent
      // overlapped at 30ms the kernel dropped letters. (v86 already ignores
      // key events that come from a text field, so nothing is typed twice.)
      var typeQueue = Promise.resolve();
      var typeSend = function (fn) { typeQueue = typeQueue.then(fn).catch(function () {}); };
      var mirrorToKernel = function () {
        var v = composeInput.value, i = 0;
        while (i < mirrored.length && i < v.length && mirrored.charAt(i) === v.charAt(i)) i++;
        var del = mirrored.length - i, add = v.slice(i);
        mirrored = v;
        if (del) { var bs = []; for (var k = 0; k < del; k++) bs.push(8); typeSend(function () { return emulator.keyboard_send_keys(bs, 60); }); }
        if (add) typeSend(function () { return emulator.keyboard_send_text(add, 60); });
      };
      // The phone keyboard covers the bottom half of the page, and with it the
      // demo, so you could not see her while you typed. Focusing the bar puts
      // the demo in full screen, and while it is full screen the frame follows
      // the visible area (visualViewport), so the screen shrinks to fit above
      // the keyboard with the bar right on top of it.
      var demoFrameEl = document.getElementById("demo-frame");
      var syncViewport = function () {
        var v = window.visualViewport;
        if (!demoFrameEl || !v) return;
        demoFrameEl.style.setProperty("--demo-h", v.height + "px");
        demoFrameEl.style.setProperty("--demo-top", v.offsetTop + "px");
      };
      if (window.visualViewport) {
        window.visualViewport.addEventListener("resize", syncViewport);
        window.visualViewport.addEventListener("scroll", syncViewport);
      }
      composeInput.addEventListener("focus", function () {
        focusIn(); trackActivity();
        var toggle = document.getElementById("demo-exit");
        if (demoFrameEl && toggle && !demoFrameEl.classList.contains("demo-full")) toggle.click();
        syncViewport();
      });
      composeInput.addEventListener("input", function () {
        trackActivity();
        if (emulator && adaptersReady) mirrorToKernel();
      });
      composer.addEventListener("submit", function (ev) {
        ev.preventDefault();
        trackActivity();
        if (!emulator || !adaptersReady) return;
        focusIn();
        typeSend(function () { return emulator.keyboard_send_keys([13], 60); });
        mirrored = "";
        composeInput.value = "";
        composeInput.focus(); // keep the keyboard up for the next message
      });
    }
  }
  container.addEventListener("mousedown", focusIn);
  container.addEventListener("touchstart", focusIn, { passive: true });
  // Tab must still walk past the demo: only a key that means "type to it"
  // takes the keyboard over, and Shift+Escape gives it back (WCAG 2.1.2,
  // no keyboard trap); the release itself lives in the Escape listener near the
  // top of this file, which has to run before v86's own.
  container.addEventListener("keydown", function (ev) { if (ev.key === "Tab") return; focusIn(); });
  function releaseKeyboard() {
    focused = false;
    if (emulator && emulator.keyboard_adapter) emulator.keyboard_adapter.emu_enabled = false;
    if (emulator && emulator.mouse_adapter) emulator.mouse_adapter.emu_enabled = false;
    container.focus();
  }
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
    // v0.76.45: real bug, live-repro'd -- preventDefault only ran inside
    // the focused/enabled branch below, so a wheel event over the demo
    // while that state was still false (e.g. right after opening the Apps
    // folder, before a click had fully registered focus) fell through to
    // the browser's own native scroll and moved the whole PAGE instead of
    // the app grid. Suppress default unconditionally whenever the pointer
    // is over the demo; only the actual forwarding to the emulator stays
    // gated on focus/enabled.
    ev.preventDefault();
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
    }
  }, { capture: true, passive: false });

  // The headline used to sit on top of the demo, so a click into the demo
  // hid it. It has lived below the demo since v0.76.52, where it covers
  // nothing; the hide-on-click was a leftover that only made the text
  // vanish. Removed, the headline always stays.

  // A real QA hook, not debug scaffolding left behind: "apps don't open on
  // mobile" got reported and 'fixed' several times while every check was a
  // human squinting at a phone, because nothing here is observable from
  // outside this closure. mobiletest.mjs drives a real iPhone emulation
  // against the real page and needs to see whether input is actually armed
  // to tell "the tap missed" apart from "input was never enabled".
  window.__jt = {
    // v0.82.x: `emu` is a getter now, not a plain snapshot -- `emulator` is
    // deferred (see startEmulator/IntersectionObserver above) and is still
    // null at the moment this object literal is created whenever the demo
    // starts out off-screen; a plain `emu: emulator` would have frozen at
    // that null forever. A getter reads the live variable on every access,
    // same as `ready`/`focused` below already did.
    get emu() { return emulator; }, /* mobiletest.mjs reads the kernel's own serial log through this: the guest's klog output is the only view into what the kernel actually thinks happened */
    get ready() { return adaptersReady; },
    get focused() { return focused; },
    get mouseOn() { return !!(emulator && emulator.mouse_adapter && emulator.mouse_adapter.emu_enabled); },
    get absolute() { return absoluteMouse; }, /* v62: did the kernel enable v86's vmmouse backdoor */
    get serial() { return serialLog; },
    get toolCounts() { return toolCounts; }, /* demochat-check.mjs: running count per chattool=<tool>: marker, immune to the serial window rolling */
    get started() { return !!emulator || emulatorStarting; }, /* v0.82.x: true once startEmulator() has actually run (construction kicked off, not necessarily finished) -- lets a check script tell "gated, not yet started" apart from "started", the real signal lazy-boot-check.mjs asserts on */
    get audioState() { return emulator && emulator.speaker_adapter && emulator.speaker_adapter.audio_context ? emulator.speaker_adapter.audio_context.state : "no-speaker-adapter"; }, /* mobile-audio-check.mjs: real iPhone AudioContext unlock state, no ?audiodebug flag needed */
    get audioDebug() { return audioDebugState; }, /* mobile-audio-check.mjs: chunkCount/lastLevel from the dac-send-data hook, only populated under ?audiodebug */
    click: function () {
      if (!emulator) return;
      trackClick();
      emulator.bus.send("mouse-click", [true, false, false]);
      setTimeout(function () { if (emulator) emulator.bus.send("mouse-click", [false, false, false]); }, 60);
    },
    move: function (dx, dy) {
      if (!emulator) return;
      if (absoluteMouse) { sendAbsolute(trackedKx + dx, trackedKy + dy); return; }
      emulator.bus.send("mouse-delta", [dx, -dy]);
    },
    moveTo: function (kx, ky) { if (emulator) sendAbsolute(kx, ky); },
    glideTo: function (kx, ky) { return new Promise(function (res) { if (!emulator) return res(); moveCursorTo(kx, ky, res, false); }); } /* 1.1.4: the tour's own eased glide, for tools/checks/cursorglide-check.mjs */
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
  // 1.5.8: real report on a Retina Mac in Safari, "the live demo is still
  // pretty pixely." Root cause: the guest framebuffer is a fixed pixel
  // resolution (e.g. 1920x1080), but the code below used to size the
  // canvas's CSS box purely against the container's own CSS pixel box
  // (cover/contain), with no regard at all for the real DEVICE pixel grid
  // a Retina screen actually draws to. Landing on an arbitrary fractional
  // CSS size (e.g. ~1280 CSS px, 2560 device px on a 2x screen against a
  // 1920px source) forces the browser to resample by a non-integer factor,
  // and every glyph and line in the kernel's own bitmap UI smears or
  // stair-steps, exactly the reported symptom, worst on the crisp VGA-style
  // text the kernel draws.
  //
  // Fix: whenever the viewport allows it, size the canvas's CSS box so
  // canvas pixels map 1:1 to DEVICE pixels: cssWidth = w / dpr (960 CSS px
  // on a 2x screen for a 1920px source, 1920 CSS px on a 1x screen). If the
  // viewport is narrower than that, fall back to the largest INTEGER
  // divisor of that scale that still fits (w / (dpr * k)), still an exact
  // device-pixel-aligned mapping (no fractional device pixel splits a
  // source pixel), just smaller.
  //
  // image-rendering:pixelated is only correct at the exact k=1, ratio=1
  // mapping (or a true integer UPscale, which never happens here -- the
  // guest framebuffer is always at least as big as any CSS box this runs
  // in). Every k>1 case is a DOWNscale: nearest-neighbour sampling on a
  // downscale drops whole source rows/columns instead of averaging them,
  // which shreds text just as badly as the original bug, worse in some
  // cases. Caught before merge: the first version of this fix marked every
  // exact-integer case "pixelated", which looked right at ratio 1 (the
  // 2x-screen desktop case) but produced visibly broken glyphs at ratio 2+
  // (1x desktop, and both mobile cases, k=3/k=6). So only k===1 gets
  // "pixelated"; k>1 and the no-integer-fits fallback both get "auto"
  // (smooth), which correctly area-averages a downscale instead of
  // dropping data.
  var currentScale = 1;
  var lastRsW = -1, lastRsH = -1, lastRsCW = -1, lastRsCH = -1, lastDpr = -1, lastRsCss = "";
  function resizeCanvas() {
    if (!screenCanvas) return;
    var w = screenCanvas.width || 800, h = screenCanvas.height || 600;
    var cw = screenContainer.clientWidth, ch = screenContainer.clientHeight;
    var dpr = window.devicePixelRatio || 1;
    if (cw === lastRsW && ch === lastRsH && w === lastRsCW && h === lastRsCH && dpr === lastDpr && screenCanvas.style.width === lastRsCss) return; // nothing changed: skip forced layout + style writes (was every 200ms)
    lastRsW = cw; lastRsH = ch; lastRsCW = w; lastRsCH = h; lastDpr = dpr;
    var box = screenContainer.getBoundingClientRect();

    // Fill first: the 1.5.11 integer-step search shrank a 1920 framebuffer
    // to 960 CSS px inside a 1280 box on a Retina Mac, big black borders.
    // Every k>1 step was a smoothed downscale anyway, no crisper than a
    // fractional one. The exact 1:1 device-pixel size is still used when
    // it lands within 10% of the fill size, so a box close to native stays
    // pixel-sharp without leaving a visible margin.
    var coverScale = Math.max(box.width / w, box.height / h) || 1;
    var containScale = Math.min(box.width / w, box.height / h) || 1;
    var visibleFrac = Math.min(box.width / (w * coverScale), box.height / (h * coverScale));
    currentScale = visibleFrac >= 0.75 ? coverScale : containScale;
    var crisp = 1 / dpr <= currentScale && 1 / dpr >= currentScale * 0.9;
    if (crisp) currentScale = 1 / dpr;
    var exact = RES_TOKEN && dpr === 1 && w === RES_W && h === RES_H;
    if (exact) { currentScale = 1; crisp = true; }
    screenCanvas.style.width = Math.round(w * currentScale) + "px";
    screenCanvas.style.height = Math.round(h * currentScale) + "px";
    screenCanvas.style.imageRendering = crisp ? "pixelated" : "auto";
    lastRsCss = screenCanvas.style.width;
  }
  window.addEventListener("resize", resizeCanvas);
  window.addEventListener("orientationchange", resizeCanvas);
  // devicePixelRatio has no native "change" event; the standard trick is a
  // matchMedia query at the current ratio, which fires once the ratio no
  // longer matches (moving the window to a different-DPI display, an OS
  // zoom change) -- re-registered at the new ratio each time it fires so
  // it keeps catching every future change, not just the first one.
  if (window.matchMedia) {
    (function watchDpr() {
      var mq = window.matchMedia("(resolution: " + (window.devicePixelRatio || 1) + "dppx)");
      var onChange = function () { resizeCanvas(); watchDpr(); };
      if (mq.addEventListener) mq.addEventListener("change", onChange, { once: true });
      else if (mq.addListener) mq.addListener(onChange); // Safari < 14 fallback
    })();
  }
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
      // 1.1.4 (direct request: "it just teleports"): glide instead of
      // jumping. The absolute backdoor makes one send the whole move, so
      // the tour's cursor used to appear at the target instantly. Now the
      // move is split into ~60 Hz absolute sends along an ease-in-out
      // curve; duration grows with distance (a dock-to-dock hop is about
      // half a second, a nudge is near-instant). trackedKx/Ky follow via
      // sendAbsolute, so the relative fallback's shadow stays honest.
      var fromX = trackedKx, fromY = trackedKy;
      var dist = Math.sqrt((kx - fromX) * (kx - fromX) + (ky - fromY) * (ky - fromY));
      var ms = Math.min(GLIDE_MAX_MS, 120 + dist * 0.9);
      if (dist < 2 || ms < 32) { sendAbsolute(kx, ky); if (done) setTimeout(done, 32); return; }
      var start = Date.now();
      var tick = setInterval(function () {
        var t = Math.min(1, (Date.now() - start) / ms);
        var e = t < 0.5 ? 2 * t * t : 1 - Math.pow(-2 * t + 2, 2) / 2; // ease-in-out quad
        sendAbsolute(Math.round(fromX + (kx - fromX) * e), Math.round(fromY + (ky - fromY) * e));
        if (t >= 1) { clearInterval(tick); sendAbsolute(kx, ky); if (done) setTimeout(done, 32); }
      }, 16);
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
  var bootLogo = document.getElementById("boot-logo"); // cheap DOM ref, stays eager; `bootStart` itself is set inside startEmulator now, see its own comment
  setInterval(function () {
    // `emulator` may not exist yet (construction is deferred, see startEmulator/IntersectionObserver above): guard rather than assume.
    var vga = emulator && emulator.v86 && emulator.v86.cpu.devices.vga;
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
  // Notes/Chat/Terminal (unchanged since v51), 'c' + four Enter-
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
  // so tourLoop can order them freely instead
  // of running the whole array as one block after both rounds -- direct
  // feedback that leading with pure window-management read as "not much
  // interaction". TOUR_APPS keeps only the genuinely single-window-only
  // apps (Notes/Terminal/Chat; gui_multiwin_supported in kernel.c is
  // false for all three).
  var MAIL_APP = { name: 'Mail', slot: 2, script: [
    { type: 'keys', text: 'c', speed: 200 },
    { type: 'wait', ms: 500 },
    { type: 'keys', text: 'demo@jt.os\n', speed: 130 },
    { type: 'wait', ms: 350 },
    { type: 'keys', text: 'Joshua Tree\n', speed: 130 }, // From name
    { type: 'wait', ms: 350 },
    { type: 'keys', text: 'From scratch.\n', speed: 130 }, // Subject
    { type: 'wait', ms: 350 },
    { type: 'keys', text: 'Written to disk.\n', speed: 130 }, // Body; Enter on the last field sends and files it (mail: filed n=)
    { type: 'wait', ms: 2500 },
    { type: 'raw', codes: [27], speed: 80 } // Escape backs out of the inline compose sheet if it is still up; the scene's own Escape then closes Mail
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
  // 1.0.13: real live window-drag scene, direct request ("the tour should
  // show off the new title-bar drag, not just mention it exists"). Every
  // app window drags live by its title bar since 1.0.11
  // (gui_app_mouse_tick in kernel.c, tools/checks/windowdrag-check.py's
  // own QMP proof), but the tour itself never actually did it -- it only
  // ever sent the same open/type/dwell/close shape every other single-
  // window app gets. Notes gets a real 'drag' script step here (see
  // runScript's own handling of it, and dragWindow() below) using the
  // exact geometry windowdrag-check.py already proves against the real
  // kernel: gui_launch_from_dock's window rect is x=70,y=40,w=820,h=385,
  // so its title band (gui_app_mouse_tick's own press-arm test: y in
  // [win_y, win_y+30), x >= win_x+80) runs from (150,40) to (890,70).
  // Press at (400,52), comfortably inside it and clear of the traffic
  // lights, then a few small absolute moves (+50,+6, well inside the
  // kernel's own clamp: x in [0,140], y in [menubar height, dock band
  // top - 385]) so the window visibly slides right and down, then the
  // same drag run backwards so the close light is back at CLOSE_X/
  // CLOSE_Y (94,56) for runSoloApp's own close click below -- proves the
  // drag twice (there and back) instead of once, and never requires a
  // one-off "moved close position" special case. `dwell` is raised past
  // DWELL_MS (7000) since the typed sentence, the select/copy/clear beat
  // below (v1.2.0), and two real ~600ms drags and their settling pauses
  // all run past the default single-app budget, the same reasoning the
  // Chat entry below already uses for its own real network round trip.
  var NOTES_TITLE_X = 400, NOTES_TITLE_Y = 52; // inside win_y..win_y+30 (40..70) and >= win_x+80 (150)
  var NOTES_DRAG_DX = 50, NOTES_DRAG_DY = 6;   // safely inside gui_app_mouse_tick's own clamp
  // v1.2.0: Shift+Left and Ctrl+C are real MODIFIER CHORDS (one key held
  // down while another is pressed), which neither of libv86's higher-level
  // helpers can express: keyboard_send_text() only knows single plain
  // characters (simulate_char has no idea what "Shift" even is), and
  // keyboard_send_keys() presses and releases one JS keyCode at a time
  // (simulate_press), so a Shift held across several Left presses would
  // come back up before the next one. keyboard_send_scancodes() is the one
  // level below both of those this file already reaches for when a higher
  // helper can't say what's needed (see the Escape sends elsewhere in this
  // file) -- it puts raw XT set-1 bytes straight on the bus, in order, so a
  // modifier's own make code can sit un-released across several taps of
  // another key exactly like a real keyboard held down would. Bytes below
  // are the same ones kernel/kernel.c's own SC[]/kbd_ctrl table decodes:
  // 0x2A/0xAA Shift make/break, 0x1D/0x9D Ctrl make/break, 0x2E/0xAE 'C'
  // make/break, 0xE0 0x4B / 0xE0 0xCB Left arrow make/break (Left is an
  // "extended" key, always 0xE0-prefixed).
  var SC_SHIFT_DOWN = 0x2A, SC_SHIFT_UP = 0xAA;
  var SC_CTRL_DOWN = 0x1D, SC_CTRL_UP = 0x9D;
  var SC_LEFT_DOWN = [0xE0, 0x4B], SC_LEFT_UP = [0xE0, 0xCB];
  var SC_C_DOWN = 0x2E, SC_C_UP = 0xAE;
  function heldChord(modDown, modUp, taps) {
    var codes = [modDown];
    for (var i = 0; i < taps.length; i++) codes = codes.concat(taps[i]);
    codes.push(modUp);
    return codes;
  }
  function shiftLeftTimes(n) {
    var taps = [];
    for (var i = 0; i < n; i++) taps.push(SC_LEFT_DOWN.concat(SC_LEFT_UP));
    return heldChord(SC_SHIFT_DOWN, SC_SHIFT_UP, taps);
  }
  var CTRL_C_CODES = heldChord(SC_CTRL_DOWN, SC_CTRL_UP, [[SC_C_DOWN, SC_C_UP]]);
  // Shared with phoneSamanthaIntro below (the exact same first line the
  // Samantha scene's own script types), so a phone visitor's first
  // reminder request and a desktop visitor's are word-for-word the same
  // sentence -- only the delivery differs (see phoneSamanthaIntro's own
  // comment for why phone skips the leading 'n').
  var SAMANTHA_REMINDER_LINE = 'remind me to call mom at 5';
  // 2.0 tour: every app is a ring-3 compositor window now, so a scene is
  // just "click the dock tile, type or arrow around, press Escape". Escape
  // closes a window, a click inside one never does (user/*.c poll loops), so
  // runSoloApp ends every scene with a real Escape. Dock slots follow
  // GUI_DOCK_DEFAULT in kernel.c: Apps, Burrow, Mail, Calendar, Notes,
  // Reminders, Terminal, Samantha, Weather, Stocks, Trash. The other 17 apps
  // live in the Apps folder and get a short beat each (APPS_FOLDER_TOUR).
  var DOWN2 = downKeys(2);
  function downKeys(n) { var c = []; for (var i = 0; i < n; i++) c = c.concat([0xE0, 0x50, 0xE0, 0xD0]); return c; }
  var ENTER_KEY = { type: 'raw', codes: [13], speed: 80 };
  var ESC_KEY = { type: 'raw', codes: [27], speed: 80 };
  var BURROW_APP = { name: 'Burrow', slot: 1, dwell: 6500, script: [
    { type: 'wait', ms: 700 },
    // no arrow walk: Burrow lists folders first, so the cursor already sits on the demo's DOCS folder (kernel.c seeds it with two files)
    ENTER_KEY, // open the folder under the cursor
    { type: 'wait', ms: 1400 },
    { type: 'keys', text: '2', speed: 200 }, // icon view
    { type: 'wait', ms: 1200 },
    { type: 'keys', text: '1', speed: 200 } // back to the list
  ] };
  var NOTES_APP = { name: 'Notes', slot: 4, dwell: 11000, script: [
    { type: 'wait', ms: 600 },
    { type: 'scancodes', codes: downKeys(1), speed: 300 }, // pick a folder
    ENTER_KEY,
    { type: 'wait', ms: 500 },
    { type: 'keys', text: 'n', speed: 200 }, // a new note, filed in that folder
    { type: 'wait', ms: 500 },
    { type: 'keys', text: 'Kernel, GUI, browser, terminal, and a dozen real apps, none of it borrowed.', speed: 55 },
    { type: 'wait', ms: 500 },
    { type: 'scancodes', codes: shiftLeftTimes(9), speed: 90 }, // select the last word
    { type: 'wait', ms: 700 },
    { type: 'scancodes', codes: CTRL_C_CODES, speed: 90 },
    { type: 'wait', ms: 300 },
    ESC_KEY, // clears the selection, a second Escape leaves the editor
    { type: 'wait', ms: 700 },
    ESC_KEY,
    { type: 'wait', ms: 900 }
  ] };
  var TERMINAL_APP = { name: 'Terminal', slot: 6, dwell: 6000, script: [
    { type: 'wait', ms: 500 },
    { type: 'keys', text: 'ls\n', speed: 55 },
    { type: 'wait', ms: 900 },
    { type: 'keys', text: 'echo hello from joshua tree\n', speed: 55 },
    { type: 'wait', ms: 700 },
    { type: 'keys', text: 'uptime\n', speed: 55 }
  ] };
  var REMINDERS_APP = { name: 'Reminders', slot: 5, dwell: 7000, script: [
    { type: 'wait', ms: 500 },
    { type: 'keys', text: 'a', speed: 200 },
    { type: 'wait', ms: 500 },
    { type: 'keys', text: 'Ship the demo tour rework\n', speed: 55 },
    { type: 'wait', ms: 400 },
    { type: 'keys', text: 'a', speed: 200 },
    { type: 'wait', ms: 400 },
    { type: 'keys', text: 'Real hardware port of the kernel\n', speed: 55 }
  ] };
  var WEATHER_APP = { name: 'Weather', slot: 8, dwell: 4500, script: [] };
  var STOCKS_APP = { name: 'Stocks', slot: 9, dwell: 7500, script: [
    { type: 'wait', ms: 1800 }, // live quotes land
    { type: 'scancodes', codes: downKeys(3), speed: 450 }, // walk the watchlist
    { type: 'wait', ms: 600 },
    { type: 'scancodes', codes: [0xE0, 0x4D, 0xE0, 0xCD, 0xE0, 0x4D, 0xE0, 0xCD], speed: 500 } // widen the chart range
  ] };
  // Samantha is a ring-3 window: typed text lands straight in her input bar,
  // no leading hotkey, Enter sends, Escape closes her.
  var SAMANTHA_APP = { name: 'Samantha', slot: 7, dwell: 24000, script: [] }; // script filled per lap
  var TOUR_APPS = [BURROW_APP, NOTES_APP, TERMINAL_APP, REMINDERS_APP, WEATHER_APP, STOCKS_APP, SAMANTHA_APP];
  // The Apps folder: arrows walk the grid, Enter opens, Escape returns to the
  // grid with the same tile selected, so one 'd' then Enter steps through the
  // 18 slots after the dock's own eight. Stocks has its own dock scene.
  var APPS_FOLDER_TOUR = [
    { name: 'Curbfind', dwell: 1800 }, { name: 'Keyrate', dwell: 1800 }, { name: 'Bookrank', dwell: 1800 },
    { name: 'Quotes', dwell: 1800 }, { name: 'Lexly', dwell: 1800 },
    { name: 'Toroid', dwell: 1800 }, { name: 'Sparkjar', dwell: 1800 },
    { name: 'Fieldbook', dwell: 1800 }, { name: 'Contacts', dwell: 1800 }, { name: 'Calculator', dwell: 1800, keys: '12*7\n' },
    { skip: 'Stocks' }, { name: 'Search', dwell: 1800 }, { name: 'Epiphany', dwell: 2200 }, { name: 'Portfolio', dwell: 2200 },
    { name: 'Activity', dwell: 4200, down: 3 }, { name: 'Clock', dwell: 1800 }
  ];
  function appsFolderScript() {
    var s = [{ type: 'wait', ms: 1200 }, { type: 'keys', text: 'dddddddd', speed: 120 }, { type: 'wait', ms: 500 }]; // select Curbfind, tile 8
    APPS_FOLDER_TOUR.forEach(function (a, i) {
      if (i > 0) s.push({ type: 'keys', text: 'd', speed: 100 });
      if (a.skip) return;
      s.push({ type: 'wait', ms: 250 }, ENTER_KEY, { type: 'headline', name: a.name }, { type: 'wait', ms: 700 });
      if (a.keys) s.push({ type: 'keys', text: a.keys, speed: 80 });
      if (a.down) s.push({ type: 'scancodes', codes: downKeys(a.down), speed: 500 });
      s.push({ type: 'wait', ms: a.dwell }, ESC_KEY, { type: 'wait', ms: 500 });
    });
    return s;
  }
  var APPS_APP = { name: 'Apps', slot: 0, dwell: 1, script: appsFolderScript() };
  // v0.76.12: the real multi-window demo. Files (slot 1) and Weather
  // (slot 8) have no per-app keyboard interaction in this kernel (both are
  // gui_wait_close-only static viewers, confirmed by reading kernel.c --
  // scripting a fake keystroke into either would be exactly the dishonest
  // "simulated interaction" this tour has never done for any app), so
  // their entries carry no `script`; Reminders (slot 5) is real-input
  // capable and keeps the exact same add-a-reminder script the old
  // sequential entry used, just now run while a second window (Files) is
  // genuinely open alongside it, not before/after it.
  var MW_FILES = { name: 'Burrow', slot: 1 };
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
  // fix/demo-aplus-1 item 6: a visitor who watches two laps back to back used to
  // hear the exact same Samantha exchange twice. lapIndex increments once per
  // lap (tourLoop's own while loop, right before this scene runs) and picks a
  // different real chat_run_tool round trip each time, cycling every 3 laps --
  // each still ends on "open calculator" so the scene's real close (Chat
  // handing off to Calculator) is unchanged.
  var lapIndex = 0;
  var SAMANTHA_LAP_SCRIPTS = [
    [ // lap 0: reminder + note
      { type: 'keys', text: SAMANTHA_REMINDER_LINE + '\n', speed: 55 },
      { type: 'wait', ms: 3500 },
      { type: 'keys', text: 'note: pick up dry cleaning\n', speed: 55 },
      { type: 'wait', ms: 3500 }
    ],
    [ // lap 1: weather + calendar
      { type: 'keys', text: "what's the weather like\n", speed: 55 },
      { type: 'wait', ms: 3500 },
      { type: 'keys', text: "what's on my calendar today\n", speed: 55 },
      { type: 'wait', ms: 3500 }
    ],
    [ // lap 2: a real fact question, then a tool that opens another app
      { type: 'keys', text: "what's on my calendar today\n", speed: 55 },
      { type: 'wait', ms: 3500 },
      { type: 'keys', text: 'note: book flights for the launch\n', speed: 55 },
      { type: 'wait', ms: 3500 }
    ]
  ];
  // Escape (the scene's own close, runSoloApp) ends the scene; no scripted close line.
  var SAMANTHA_LAP_CLOSE = [{ type: 'wait', ms: 600 }];
  function samanthaScriptForLap(lap) { return SAMANTHA_LAP_SCRIPTS[lap % SAMANTHA_LAP_SCRIPTS.length].concat(SAMANTHA_LAP_CLOSE); }
  // Phone's already-open avatar box only gets one line (see
  // phoneSamanthaIntro below), so it cycles the same three real requests.
  var PHONE_LAP_LINES = [SAMANTHA_REMINDER_LINE, "what's on my calendar today", "note: pick up dry cleaning"];
  var phoneSaidFirst = false; // the avatar's own box takes the first line bare; the console after it needs 'n'
  // Every soft reboot re-injects the kernel. v86's own load_multiboot() hardcodes an
  // empty command line, so portfolio mode calls the same two steps it does (read from
  // the vendored libv86.js) with "portfolio" passed through, or the dock would reset
  // to the system set after the first reboot.
  function reinjectKernel() {
    var cpu = emulator.v86 && emulator.v86.cpu;
    if (!kernelElfBuffer || !cpu || !cpu.load_multiboot) return;
    if (cpu.load_multiboot_option_rom) { if (cpu.load_multiboot_option_rom(kernelElfBuffer, undefined, BOOT_CMDLINE)) cpu.reg32[0] = cpu.io.port_read32(244); }
    else cpu.load_multiboot(kernelElfBuffer);
  }
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
  // 1.0.13: real live title-bar drag, the same bus calls clickAt already
  // uses (mouse-absolute via moveCursorToAsync, then a raw mouse-click
  // bus send) but held down across several moves instead of one
  // press-release pair -- exactly the QMP sequence
  // tools/checks/windowdrag-check.py already proves live against the real
  // kernel (move to the title band, press, several small absolute moves,
  // release), never a new/unproven interaction shape. One press, N
  // absolute moves, one release: gui_app_mouse_tick (kernel.c) arms the
  // drag on the press if it lands in the title band and moves the window
  // on every later pointer move while the button stays down, so nothing
  // here needs to resend the click between moves. `gen`/`focused` are
  // checked between every move so a real visitor taking over mid-drag
  // doesn't fight them for the pointer with the button stuck down.
  async function dragWindow(from, to, steps, ms, gen) {
    var n = steps || 8, totalMs = ms || 600;
    await moveCursorToAsync(from[0], from[1], true);
    if (focused || tourGen !== gen) return;
    await sleep(150);
    trackClick();
    emulator.bus.send("mouse-click", [true, false, false]); // press inside the title band arms the drag
    await sleep(150);
    for (var s = 1; s <= n; s++) {
      if (focused || tourGen !== gen) break;
      var kx = from[0] + (to[0] - from[0]) * s / n;
      var ky = from[1] + (to[1] - from[1]) * s / n;
      await moveCursorToAsync(kx, ky, true); // every move while the button is down slides the window along, live
      await sleep(totalMs / n);
    }
    emulator.bus.send("mouse-click", [false, false, false]); // release ends the drag
    await sleep(150);
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
      // v1.2.0: a real modifier chord (Shift+Left, Ctrl+C), see the
      // heldChord/shiftLeftTimes/CTRL_C_CODES comment above the Notes
      // entry in TOUR_APPS -- raw scancode bytes, not JS keyCodes, sent
      // one at a time on the same bus path keyboard_send_keys uses.
      else if (step.type === "scancodes" && emulator.keyboard_send_scancodes) await emulator.keyboard_send_scancodes(step.codes, step.speed || 30);
      // 1.0.13: press/move.../release on a window's title band, data like
      // every other step (see the Notes entry in TOUR_APPS and
      // dragWindow's own comment above).
      else if (step.type === "headline") updateHeadline(step.name);
      else if (step.type === "drag") await dragWindow(step.from, step.to, step.steps, step.ms, gen);
    }
  }
  // Dock geometry in LOGICAL kernel pixels, the same arithmetic as
  // kernel.c's gui_dock_icon/gui_dock_x0/gui_slot_x: GUI_ICON_COUNT (11) slots, tiles
  // dock_scale_pct (default 7) percent of the height capped by the 740px
  // DOCK_BUDGET, gap 6, pad 10, bottom margin 24. A fresh v86 boot has no
  // SETTINGS.TXT, so the default scale is what's actually on screen.
  function dockSlotPos(slot) {
    var count = 11, gap = 6, pad = 10, marginBot = 24, budget = 740;
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
    var vga = emulator && emulator.v86 && emulator.v86.cpu.devices.vga; // guarded: only ever called from the tour, which never runs before emulator exists, but cheap to be defensive
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

    // Type out the variable part at 20ms (fast, so the H1 tracks the demo) per character
    typewriterInterval = setInterval(function() {
      if (index < chars.length) {
        element.textContent += chars[index];
        index++;
      } else {
        clearInterval(typewriterInterval);
        typewriterInterval = null;
        if (callback) callback();
      }
    }, 20);
  }

  // One line per app, so the eyebrow says something about the SAME app the
  // H1 just named instead of cycling through unrelated captions underneath
  // it. Direct request: the two headings should read as one thought.
  var APP_CAPTION = {
    'Burrow': 'Real FAT16, real reads and writes',
    'Weather': 'Live data over its own network stack',
    'Mail': 'A real mailbox on a real filesystem',
    'Calendar': 'Real events, persisted across reboots',
    'Notes': 'Real text selection, drag it by its title bar.',
    'Reminders': 'A checklist that survives a reboot',
    'Terminal': 'A real shell, talking to a real kernel',
    'Samantha': 'Talk to the machine, natively',
    'Trash': 'Deleted files, really recoverable',
    'Contacts': 'Its own records, its own format',
    'Calculator': 'Small, and it actually adds up'
  };
  function setEyebrow(appName) {
    if (!window.jtEyebrow) return;
    var caption = APP_CAPTION[appName];
    if (caption) window.jtEyebrow.hold(caption);
    else window.jtEyebrow.resume();
  }

  function updateHeadline(appName) {
    // v0.76.30: dynamic headline with typewriter animation.
    // Types out "Introducing <AppName>." when app opens, creating a real sense
    // of discovery rather than instant replacement. Lightweight effect, no external deps.
    var h1Link = document.querySelector('h1');
    if (h1Link) {
      typewriterEffect(h1Link, appName + '.');
    }
    setEyebrow(appName);
  }
  // The H1 that ships in the HTML is the brand line, and the tour resets to
  // exactly that, so nothing a visitor reads on load is replaced a second
  // later by something different. The roadmap's **Latest** announcement now
  // leads the eyebrow's own cycle instead of living here, which is what
  // removed the swap for real rather than papering over it.
  var DEFAULT_HEADLINE = (function () {
    var h1 = document.querySelector('h1');
    var text = h1 ? h1.textContent.trim() : '';
    return text.indexOf(HEADLINE_PREFIX) === 0
      ? text.slice(HEADLINE_PREFIX.length)
      : 'Joshua Tree.';
  })();
  function resetHeadline() {
    // Default headline when no tour is running or before the tour starts.
    // Uses typewriter effect for visual consistency.
    var h1Link = document.querySelector('h1');
    if (h1Link) {
      typewriterEffect(h1Link, DEFAULT_HEADLINE);
    }
    if (window.jtEyebrow) window.jtEyebrow.resume();
  }

  async function runSoloApp(gen, app) {
    if (focused || tourGen !== gen || !adaptersReady) return;
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
    // Direct request: the headline should say what the demo is actually
    // showing. It used to be typed before the click had even travelled, so
    // it named an app that was not on screen yet and led the demo by about
    // a second. Now it lands once the app's first frame is really drawn.
    updateHeadline(app.name);
    await runScript(app.script, gen);
    if (focused || tourGen !== gen) return;
    var remaining = (app.dwell || DWELL_MS) - (Date.now() - dwellStart);
    if (remaining > 0) await sleep(remaining);
    if (focused || tourGen !== gen) return;
    if (emulator.keyboard_send_keys) await emulator.keyboard_send_keys([27], 80); // Escape closes a ring-3 window; a click inside one never does
    if (focused || tourGen !== gen) return;
    resetHeadline(); // the app is gone, so stop announcing it over an empty desktop
    await sleep(1200); // a beat before the next app opens, reads as a real transition not a jump-cut
  }
  // Real bug, reproduced headless at iPhone size and confirmed by
  // intercepting the actual POST body: on a phone, kernel.c's boot_to_phone
  // + boot_to_samantha (embed.js's own cmdline above, "phone samantha ...")
  // lands the kernel straight in Chat's full-screen avatar view
  // (kernel/chat.h's chat_boot_samantha_open) BEFORE any dock/desktop ever
  // shows -- her input box there is already open and reading keys directly
  // into the message buffer (see chat_boot_samantha_open's own `for (;;)`
  // loop, `if (k >= 32 && k < 127) msg[n++] = k`), unlike the windowed
  // console every OTHER tour scene drives, where 'n' is a hotkey that opens
  // a compose prompt first. runSoloApp's shared script shape sends that
  // leading 'n' unconditionally; on the phone boot avatar there is no
  // hotkey to catch it, so it becomes the literal first character of the
  // message. Confirmed live: intercepting the demo's own /api/proxy POST
  // showed q:"nremind me to call mom at 5" -- Turing's picker can't
  // classify that, /api/chat falls back to the real LLM, and its honest
  // answer to a nonsense sentence is the canned "I couldn't find anything
  // on that" decline line. That decline line was the very first thing a
  // phone visitor (and Joshua, on his own iPhone) ever saw her say.
  //
  // Also: the very first tour action every lap (runSoloApp(MAIL_APP)) is a
  // dock-tile click, but on phone there is no dock yet -- the avatar screen
  // is still up, and ANY click there (chat_boot_samantha_open's own
  // `k == KEY_CLICK` case) abandons it straight into the windowed console,
  // so the rest of that first scene's script would already be typing into
  // the wrong view. Real fix: give phone its own first scene that types
  // directly into the still-open avatar (no dock click, no leading 'n'),
  // waits for her real reply, then closes the console with Escape -- the
  // same unconditional `KEY_ESC -> return` gui_launch_chat_app already
  // honors -- landing cleanly on the normal desktop the rest of this lap's
  // dock-based tour already assumes.
  async function phoneSamanthaIntro(gen) {
    if (!IS_PHONE && !PORTFOLIO_MODE) return;   // portfolio: boots into his face on every device, so this scene runs there too
    if (focused || tourGen !== gen || !adaptersReady) return;
    emulator.mouse_adapter.emu_enabled = true;
    emulator.keyboard_adapter.emu_enabled = true;
    var start = Date.now();
    while (serialLog.indexOf('samfocus') === -1) {
      if (focused || tourGen !== gen) return; // a real visitor took over
      if (Date.now() - start > (PORTFOLIO_MODE ? 60000 : 8000)) return; // didn't see the avatar boot at all (portfolio: his 72 frames load first, slower than her 60) (unexpected cmdline) -- bail, the normal dock tour below still runs as-is
      await sleep(150);
    }
    if (focused || tourGen !== gen) return;
    if (tapTalkBtn && !audioRunning()) {
      // Wait for the tap so her reply is audible; give up after 20s and run
      // silently so the demo still moves (the button stays up for later).
      tapTalkBtn.hidden = false;
      await Promise.race([tapTalkPromise, new Promise(function (r) { setTimeout(r, PORTFOLIO_MODE ? 120000 : 5000); })]); // portfolio: he waits for the first click so his voice is heard, not spoken into a suspended AudioContext
      if (focused || tourGen !== gen) return;
      await new Promise(function (r) { setTimeout(r, 400); }); // let resume() settle
    }
    updateHeadline(PORTFOLIO_MODE ? 'Joshua' : 'Samantha');
    if (PORTFOLIO_MODE) {
      // Portfolio: his face is the whole screen on every device. One line
      // ("show me around"), his reply spoken, then on desktop Escape drops to
      // the dock so the app tour below can run; a phone stays on his face.
      // (the tap wait above already unlocked audio, so the mouth hears samples)
      if (introVideo) {
        // The recorded intro is his first line; wait for it to end, fade it, drop to the dock.
        introVideo.loop = false;
        if (introVideo.paused) introVideo.play().catch(function () {});
        var vStart = Date.now();
        while (introVideo && !introVideo.ended && Date.now() - vStart < 45000) {
          if (focused || tourGen !== gen) return;
          await sleep(200);
        }
        if (focused || tourGen !== gen) return;
        // The video fades into the live full-bleed face (same shot), which holds a few seconds so the
        // visitor sees him in the OS; then Escape drops to the dock for the tour. A phone stays on his face.
        for (var fw = 0; !faceReady && fw < 150; fw++) { if (focused || tourGen !== gen) return; await sleep(200); }   // the last frame holds while his frames finish loading (30 s at most)
        if (introVideo) introVideo.dispatchEvent(new Event("fade"));
        await sleep(IS_PHONE ? 3500 : 4500);
        if (focused || tourGen !== gen) return;
        if (emulator.keyboard_send_keys) { await emulator.keyboard_send_keys([27], 80); await sleep(700); }   // Escape drops to the dock (desktop) or the home grid (phone), where the tour starts
        resetHeadline();
        return;
      }
      var pSeen = speakCount;
      // A phone stays on his face forever, so each pass says the next line: life, Vancouver, the work.
      var pLine = IS_PHONE ? PORTFOLIO_LINES[portfolioLine++ % PORTFOLIO_LINES.length] : PORTFOLIO_INTRO_LINE;
      if (IS_PHONE && emulator.keyboard_send_keys) { await emulator.keyboard_send_keys([8], 80); await sleep(200); }   // a reply still playing eats the next key (typing skips her speech): Backspace is that key, and a no-op on an empty bar
      await emulator.keyboard_send_text(pLine + '\n', 55);
      var pStart = Date.now(), pMs = 0;
      while (Date.now() - pStart < 60000) {   // the reply comes over a slow relay: 15 s cut him off before he spoke and dropped to the OS
        if (focused || tourGen !== gen) return;
        if (speakCount > pSeen) { pMs = Math.min(40000, Math.round(lastSpeakBytes / 16)) + 1200; break; }   // the intro is a ~30 s script
        await sleep(200);
      }
      // A long reply is spoken in pieces, one fetch each: keep waiting while a new piece starts.
      var pDone = speakCount, pWait = pMs || 3000;
      for (;;) {
        await sleep(pWait);
        if (focused || tourGen !== gen) return;
        if (speakCount === pDone) break;
        pDone = speakCount; pWait = Math.min(40000, Math.round(lastSpeakBytes / 16)) + 600;
      }
      if (!IS_PHONE && emulator.keyboard_send_keys) await emulator.keyboard_send_keys([27], 80);
      await sleep(800);
      resetHeadline();
      return;
    }
    // Phones stay with her: every line, one after another, forever. The old
    // lap asked one thing, hit Escape to the home grid, then clicked desktop
    // dock coordinates on a phone grid (it opened Mail's About page), which
    // read as "tries one prompt, then restarts". The first line goes straight
    // into the avatar's open box; after that she is the Chat console, where
    // 'n' starts a new message, same as the desktop lap scripts.
    for (var li = 0; li < PHONE_LAP_LINES.length; li++) {
      if (focused || tourGen !== gen) return;
      var speakSeen = speakCount;
      if (phoneSaidFirst && emulator.keyboard_send_keys) { await emulator.keyboard_send_keys([8], 80); await sleep(200); }   // Samantha is a ring-3 app now: there is no 'n' console, and a key pressed over her speaking only skips the speech, so Backspace (a no-op on an empty bar) goes first
      phoneSaidFirst = true;
      await emulator.keyboard_send_text(PHONE_LAP_LINES[li] + '\n', 55);
      // Wait for her real reply to finish speaking. The kernel logs
      // "speak: status=N bytes=M" and plays pcm8 at 16 kHz (drivers/speak.h),
      // so the clip lasts M/16000 s.
      var waitStart = Date.now(), speakMs = 0;
      while (Date.now() - waitStart < 15000) {
        if (focused || tourGen !== gen) return;
        if (speakCount > speakSeen) { speakMs = Math.min(12000, Math.round(lastSpeakBytes / 16)) + 800; break; }
        await sleep(200);
      }
      await sleep(speakMs || 3000);
    }
  }
  // The phone's version of the app tour. The phone has a home grid, not a dock: tap each app's icon, run the same
  // script the desktop tour uses, press Escape (the phone's back), and finish by opening him again.
  // Grid maths from kernel/phone_home.h: 5 columns of 86 px, rows 96 px apart, the first row starts 56 px down.
  var PHONE_GRID = { Samantha: 6, Curbfind: 8, Keyrate: 9, Bookrank: 10, Quotes: 11, Lexly: 12, Toroid: 13, Sparkjar: 14, Calculator: 17, Epiphany: 20 };
  function phoneIconPos(name) { var i = PHONE_GRID[name]; return [(i % 5) * 86 + 43, 56 + Math.floor(i / 5) * 96 + 30]; }
  async function phonePortfolioTour(gen) {
    if (!adaptersReady) return;
    emulator.mouse_adapter.emu_enabled = true;
    emulator.keyboard_adapter.emu_enabled = true;
    await sleep(900);   // the home grid has to be drawn before the first tap
    for (var p = 0; p < PORTFOLIO_TOUR.length; p++) {
      if (focused || tourGen !== gen) return;
      var app = PORTFOLIO_TOUR[p], pos = phoneIconPos(app.name);
      await clickAt(pos[0], pos[1]);
      if (focused || tourGen !== gen) return;
      var t0 = Date.now();
      await sleep(700);
      updateHeadline(app.name);
      await runScript(app.script, gen);
      if (focused || tourGen !== gen) return;
      var rest = Math.min(app.dwell || DWELL_MS, 6000) - (Date.now() - t0);
      if (rest > 0) await sleep(rest);
      if (focused || tourGen !== gen) return;
      if (emulator.keyboard_send_keys) await emulator.keyboard_send_keys([27], 80);
      resetHeadline();
      await sleep(1000);
    }
    if (focused || tourGen !== gen) return;
    var him = phoneIconPos("Samantha");   // in portfolio mode that icon opens him
    await clickAt(him[0], him[1]);
  }
  async function tourLoop(gen) {
    tourRunning = true;
    // Portfolio mode: the dock is Joshua's own apps (GUI_DOCK_PORTFOLIO in kernel.c,
    // same slot order), so the show is just that, each one opened from its real dock
    // tile and closed by its real X. No reboot between laps, nothing here writes state.
    while (PORTFOLIO_MODE && !focused && tourGen === gen) {
      await phoneSamanthaIntro(gen);   // his face first; the scene ends with Escape, which drops to the dock the tour below drives
      if (focused || tourGen !== gen) return;
      if (IS_PHONE) { await phonePortfolioTour(gen); while (!focused && tourGen === gen) await sleep(1000); return; }   // the phone pokes around the OS like the desktop does, then stays on him; leaving the loop would let the idle watchdog start a new lap that types into his chat
      for (var p = 0; p < PORTFOLIO_TOUR.length; p++) {
        if (focused || tourGen !== gen || !adaptersReady) return;
        await runSoloApp(gen, PORTFOLIO_TOUR[p]);
      }
    }
    while (!focused && tourGen === gen) {
      lapIndex++; // fix/demo-aplus-1 item 6: picks this lap's Samantha exchange below
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
      // Phone boots straight into Chat's full-screen avatar (see
      // phoneSamanthaIntro's own header comment) -- handle that screen
      // for real before the dock-based tour below, which assumes the
      // normal desktop is already showing, ever clicks anything. A no-op
      // on desktop (IS_PHONE false).
      await phoneSamanthaIntro(gen);
      if (focused || tourGen !== gen) return;
      if (IS_PHONE) continue; // phones are Samantha only: the dock tour below clicks desktop dock coordinates
      // 2.0 order: the daily-driver core first (Mail composing inline, Burrow
      // opening a folder, Calendar, Notes writing into a folder, Reminders),
      // then the shell and Samantha running her tools, then the live-data
      // pair (Weather, Stocks), then the Apps folder for the other 17.
      var lapScenes = [MAIL_APP, BURROW_APP, CALENDAR_APP, NOTES_APP, REMINDERS_APP, TERMINAL_APP, SAMANTHA_APP, WEATHER_APP, STOCKS_APP, APPS_APP];
      for (var i = 0; i < lapScenes.length; i++) {
        if (focused || tourGen !== gen || !adaptersReady) return;
        var app = lapScenes[i];
        // item 1: Terminal reads as dead time on phone (no visible result to a visitor who can't read a shell prompt at that size)
        if (IS_PHONE && app.name === 'Terminal') continue;
        if (app.name === 'Samantha') app = Object.assign({}, app, { script: samanthaScriptForLap(lapIndex) }); // item 6: a different real exchange each lap
        await runSoloApp(gen, app);
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
      window.dispatchEvent(new Event('jt-tour-lap-done')); // index.html scrolls on from here, first lap only
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
      reinjectKernel();
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
  // Portfolio mode (?portfolio in the URL, wired up from os.html): the
  // kernel gets "portfolio" on its command line (see the V86 config above)
  // and boots with Joshua's own apps on the dock. The generic kiosk tour is
  // the wrong demo there, so tourLoop runs PORTFOLIO_TOUR instead, four idle seconds in.
  var PORTFOLIO_MODE = /[?&]portfolio\b/.test(location.search);
  var PORTFOLIO_INTRO_LINE = 'show me around';
  var PORTFOLIO_LINES = ['show me around', "what's Vancouver like", 'tell me about your life', 'what are you building right now', 'what should I look at first', 'what do you do for fun'], portfolioLine = 0;
  // Real keypresses per app (the same scripted path the main tour uses),
  // so each one is used on camera, not just opened. Direct report
  // (2026-09-26): "demo apps have no interaction".
  var SC_DOWN_TAP = [0xE0, 0x50, 0xE0, 0xD0];
  function downTimes(n) { var c = []; for (var i = 0; i < n; i++) c = c.concat(SC_DOWN_TAP); return c; }
  function browseList(n) { return [{ type: 'wait', ms: 700 }, { type: 'scancodes', codes: downTimes(n), speed: 350 }]; }
  var PORTFOLIO_SCRIPTS = {
    Epiphany: [
      { type: 'wait', ms: 900 },
      { type: 'scancodes', codes: downTimes(6), speed: 220 }, // walk the live watchlist
      { type: 'wait', ms: 600 },
      { type: 'keys', text: '2', speed: 200 }, // Portfolio tab
      { type: 'wait', ms: 700 },
      { type: 'keys', text: '+++', speed: 320 }, // buy three more shares, P/L moves
      { type: 'wait', ms: 700 },
      { type: 'keys', text: '3', speed: 200 }, // Simulator
      { type: 'wait', ms: 2200 },
      { type: 'keys', text: 'b', speed: 200 },
      { type: 'wait', ms: 1600 },
      { type: 'keys', text: 'b', speed: 200 },
      { type: 'wait', ms: 1600 },
      { type: 'keys', text: 's', speed: 200 },
      { type: 'wait', ms: 900 },
      { type: 'keys', text: '4', speed: 200 } // Situation
    ],
    Curbfind: browseList(4),
    Bookrank: browseList(4),
    Sparkjar: browseList(2).concat([{ type: 'keys', text: 'u', speed: 200 }, { type: 'wait', ms: 500 }, { type: 'scancodes', codes: downTimes(2), speed: 350 }, { type: 'keys', text: 'u', speed: 200 }]), // upvote two ideas
    Keyrate: [{ type: 'wait', ms: 700 }, { type: 'keys', text: 'A real OS, from scratch, and every app on it. ', speed: 70 }],
    Calculator: [{ type: 'wait', ms: 700 }, { type: 'keys', text: '12*7', speed: 80 }, { type: 'wait', ms: 500 }, { type: 'keys', text: '\n', speed: 200 }, { type: 'wait', ms: 800 }]
  };
  var PORTFOLIO_DWELL = { Epiphany: 16000, Curbfind: 7000, Bookrank: 6000, Sparkjar: 7000, Keyrate: 8000, Calculator: 4000 };
  var PORTFOLIO_TOUR = ['Epiphany', 'Curbfind', 'Bookrank', 'Lexly', 'Sparkjar', 'Quotes', 'Keyrate', 'Toroid', 'Calculator']
    // Lexly, Quotes and Toroid are click-only cards in the kernel, so they get a short beat instead of seconds of blank window.
    .map(function (name, i) { return { name: name, slot: i + 2, script: PORTFOLIO_SCRIPTS[name] || [], dwell: PORTFOLIO_DWELL[name] || 3000 }; }); // slot 1 is the Portfolio list, the show opens the apps themselves, never the list
  // Boot takes a few seconds; the tour waits for graphical mode plus a
  // beat, and never starts at all once the visitor has focused. Also respects
  // prefers-reduced-motion: autoplay motion should not start if the visitor
  // has requested reduced motion.
  var tourArmed = false;
  var prefersReducedMotion = window.matchMedia && window.matchMedia('(prefers-reduced-motion: reduce)').matches;
  setInterval(function () {
    if (tourArmed || focused || prefersReducedMotion) return;
    // `emulator` doesn't exist until startEmulator() has actually run (deferred, see above); this interval is itself
    // part of what naturally waits for that, same as the boot-detection interval's own guard.
    var vga = emulator && emulator.v86 && emulator.v86.cpu.devices.vga;
    if (vga && vga.graphical_mode) {
      tourArmed = true;
      tourTimer = setTimeout(startTourWhenReady, PORTFOLIO_MODE ? 4000 : 2500); // 6s of a still desktop read as "still loading"
    }
  }, 500);
})();
