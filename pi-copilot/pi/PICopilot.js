#feature-id    PI Copilot : PICopilot
#feature-info  In-app AI chat that writes and runs PJSR against the active view.

#include <pjsr/Sizer.jsh>
#include <pjsr/FrameStyle.jsh>
#include <pjsr/TextAlign.jsh>
#include <pjsr/StdButton.jsh>
#include <pjsr/StdIcon.jsh>

#define SIDECAR  "http://localhost:8765"
#define SESSION  "pi-" + (new Date()).getTime()

// --- HTTP via NetworkTransfer (API verified against the live sidecar) ---------
// GET  -> setURL + onDownloadDataAvailable callback + download()
// POST -> setURL + setCustomHTTPHeaders + callback + post(<String>)
// The response body arrives through the callback; responseCode holds the HTTP
// status; the boolean return only signals transport success (a 500 returns true).
function httpJSON(method, path, bodyObj) {
   var t = new NetworkTransfer;
   t.setURL(SIDECAR + path);
   t.setCustomHTTPHeaders(["Content-Type: application/json"]);
   var buf = [];
   t.onDownloadDataAvailable = function (data) { buf.push(data.toString()); return true; };

   var ok = (method === "GET") ? t.download()
                               : t.post(bodyObj ? JSON.stringify(bodyObj) : "");
   if (!ok) {
      throw new Error("Sidecar unreachable at " + SIDECAR +
                      " — start it with: cd pi-copilot/sidecar && npm start");
   }

   var body = buf.join("");
   if (t.responseCode >= 400) {           // surface the real server error, never swallow
      var serverErr = body;
      try { serverErr = JSON.parse(body).error || body; } catch (e) {}
      throw new Error("Sidecar error " + t.responseCode + ": " + serverErr);
   }
   return JSON.parse(body);
}

// --- view context (read-only tool) -------------------------------------------
function getViewContext() {
   var w = ImageWindow.activeWindow;
   if (w.isNull) return { error: "No active image window." };
   var view = w.currentView;
   var img = view.image;
   var stats = [];
   var savedChannel = img.selectedChannel;   // restore after, don't mutate view state
   for (var c = 0; c < img.numberOfChannels; c++) {
      img.selectedChannel = c;
      stats.push({ median: img.median(), mad: img.MAD(), min: img.minimum(), max: img.maximum() });
   }
   img.selectedChannel = savedChannel;
   var kw = [];
   var keywords = w.keywords;
   for (var i = 0; i < keywords.length; i++) kw.push({ name: keywords[i].name, value: keywords[i].value });
   return {
      viewId: view.id,
      geometry: { width: img.width, height: img.height, channels: img.numberOfChannels },
      channelStats: stats,
      fitsKeywords: kw,
      history: []
   };
}

// --- run PJSR (gated, undo-wrapped) ------------------------------------------
function runPJSR(code) {
   var w = ImageWindow.activeWindow;
   if (w.isNull) return { ok: false, error: "No active image window." };
   var view = w.currentView;
   Console.show();
   Console.beginLog();
   var ok = true, errMsg = null;
   try {
      view.beginProcess();            // single undo step (design §6.2)
      // SECURITY: this evaluates model-generated PJSR — that IS the feature, and
      // PJSR offers no sandboxed eval. It is fenced by the design's three controls
      // (§6): the sidecar pjsr_parser-VALIDATES the code before it is ever offered,
      // the exact code is SHOWN verbatim above, and it only reaches here after the
      // user clicked Yes in the CONFIRM dialog. `code` is the approved string; the
      // model declares its own `view`, we also pass one for safety.
      (new Function("view", code))(view);
      view.endProcess();
   } catch (e) {
      ok = false; errMsg = String(e);
      try { view.endProcess(); } catch (e2) {}
   }
   var log = Console.endLog().toString();
   return { ok: ok, consoleOutput: log, error: errMsg };
}

// --- dialog ------------------------------------------------------------------
function CopilotDialog() {
   this.__base__ = Dialog; this.__base__();
   this.windowTitle = "PI Copilot";
   this.scaledMinWidth = 560;

   this.log = new TextBox(this);
   this.log.readOnly = true;
   this.log.setScaledMinSize(540, 360);

   this.input = new Edit(this);
   this.input.setScaledMinWidth(440);

   this.sendBtn = new PushButton(this);
   this.sendBtn.text = "Send";

   var self = this;
   function append(who, text) { self.log.text += "\n[" + who + "] " + text + "\n"; }

   // Render one Result; loop client-side until a message ends the turn.
   function handle(result) {
      if (result.type === "message") { append("copilot", result.text); return; }
      if (result.type === "tool_use" && result.tool === "get_view_context") {
         var ctx = getViewContext();
         handle(httpJSON("POST", "/continue", { sessionId: SESSION, toolResult: ctx }));
         return;
      }
      if (result.type === "tool_use" && result.tool === "run_pjsr") {
         append("copilot wants to run", "\n" + result.code);
         var go = (new MessageBox("Run this PJSR on " + ImageWindow.activeWindow.currentView.id + "?",
                   "PI Copilot", StdIcon_Question, StdButton_Yes, StdButton_No)).execute();
         if (go == StdButton_Yes) {
            var res = runPJSR(result.code);
            append(res.ok ? "ran ✓" : "error ✗", res.ok ? "(see Process Console)" : res.error);
            handle(httpJSON("POST", "/continue", { sessionId: SESSION, toolResult: res }));
         } else {
            handle(httpJSON("POST", "/continue", { sessionId: SESSION, toolResult: { ok: false, skipped: true } }));
         }
         return;
      }
      append("copilot", "Unexpected: " + JSON.stringify(result));
   }

   this.sendBtn.onClick = function () {
      var msg = self.input.text.trim();
      if (!msg) return;
      append("you", msg);
      self.input.text = "";
      try { handle(httpJSON("POST", "/chat", { sessionId: SESSION, message: msg })); }
      catch (e) { append("error", e.message); }
   };

   var row = new HorizontalSizer; row.spacing = 6; row.add(this.input, 100); row.add(this.sendBtn);
   this.sizer = new VerticalSizer; this.sizer.margin = 8; this.sizer.spacing = 6;
   this.sizer.add(this.log, 100); this.sizer.add(row);

   // Health check on open — warn, don't hang (design §8).
   try {
      var h = httpJSON("GET", "/health", null);
      append("system", "Connected to sidecar (" + h.backend + " / " + h.model + ").");
   } catch (e) {
      append("system", e.message);
   }
}
CopilotDialog.prototype = new Dialog;

function main() { (new CopilotDialog).execute(); }
main();
