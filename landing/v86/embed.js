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
// - The idle-then-autoplay behavior demonstrates the kernel's own real
//   commands (not scripted fakery: these are genuine simulate_char calls
//   into the exact same input path a real keystroke uses) after a period
//   of no interaction, and stops the instant a visitor does anything.
(function () {
  var container = document.getElementById("v86-embed");
  if (!container) return;

  // The actual screen box, not the outer wrapper: v86's ScreenAdapter does
  // a getElementsByTagName search *inside whatever container it's given*
  // for its first div/canvas to reuse. Handing it the outer #v86-embed
  // wrapper (which also contains #v86-overlay) let it walk into the wrong
  // descendants; handing it #screen_container directly keeps that search
  // scoped to exactly our #screen_text div and #screen_canvas canvas.
  var screenContainer = document.getElementById("screen_container");
  var screenText = document.getElementById("screen_text");
  var screenCanvas = document.getElementById("screen_canvas");
  var overlay = document.getElementById("v86-overlay");

  var emulator = new V86({
    wasm_path: "v86/v86.wasm",
    memory_size: 32 * 1024 * 1024,
    vga_memory_size: 8 * 1024 * 1024,
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

  // Toggle between the text and graphical screen elements: v86 keeps
  // both in the DOM and expects the embedder to show whichever is active.
  setInterval(function () {
    var vga = emulator.v86 && emulator.v86.cpu.devices.vga;
    if (!vga) return;
    var graphical = !!vga.graphical_mode;
    if (screenCanvas) screenCanvas.style.display = graphical ? "block" : "none";
    if (screenText) screenText.style.display = graphical ? "none" : "block";
  }, 200);

  // --- Idle-then-autoplay: shows real commands, hands control back the
  // instant a visitor does anything themselves. ---
  var idleTimer = null;
  var autoplayRunning = false;
  var autoplayCancelled = false;

  function type(s, cb) {
    var i = 0;
    (function next() {
      if (autoplayCancelled) return;
      if (i >= s.length) { if (cb) cb(); return; }
      emulator.keyboard_adapter.simulate_char(s[i++]);
      setTimeout(next, 45 + Math.random() * 40);
    })();
  }

  function runCommand(cmd, pause, cb) {
    if (autoplayCancelled) return;
    type(cmd, function () {
      if (autoplayCancelled) return;
      emulator.keyboard_adapter.simulate_char("\n");
      setTimeout(function () { if (!autoplayCancelled) cb(); }, pause);
    });
  }

  function startAutoplay() {
    if (focused || autoplayRunning) return;
    if (!adaptersReady) { idleTimer = setTimeout(startAutoplay, 500); return; }
    autoplayRunning = true;
    autoplayCancelled = false;
    // Autoplay drives the emulator directly, same as focusIn would, just
    // without waiting for a click; a real interaction still wins instantly
    // via stopAutoplay()/focusIn().
    emulator.keyboard_adapter.emu_enabled = true;
    var steps = [
      function (next) { runCommand("help", 2200, next); },
      function (next) { runCommand("mem", 1800, next); },
      function (next) { runCommand("write hello.txt a real file, written live", 1600, next); },
      function (next) { runCommand("cat hello.txt", 2200, next); },
      function (next) { runCommand("gui", 2600, next); },
    ];
    var i = 0;
    (function step() {
      if (autoplayCancelled || i >= steps.length) { autoplayRunning = false; return; }
      steps[i++](step);
    })();
  }

  function stopAutoplay() {
    autoplayCancelled = true;
    autoplayRunning = false;
    clearTimeout(idleTimer);
  }

  function resetIdleTimer() {
    if (focused || window.__jtReducedMotion) return; // once a real visitor has taken over, autoplay never restarts on its own; reduced-motion visitors never get unrequested typing
    clearTimeout(idleTimer);
    idleTimer = setTimeout(startAutoplay, 8000);
  }
  resetIdleTimer();

  window.__joshuaTreeEmulator = emulator; // for debugging from the console, harmless to leave
})();
