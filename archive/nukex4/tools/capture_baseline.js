// NukeX v4 — Phase B wall-time baseline capture harness.
//
// Invocation:
//   PixInsight.sh --automation-mode --force-exit \
//     -r=tools/capture_baseline.js,stack=<dir>[,log=<path>][,meta=<path>]
//
// Why key=value args (not env vars): PI's PJSR does NOT inherit shell environment
// variables — File.environmentVariable() always returns "". PJSR does expose
// `jsArguments` when the -r= path is followed by comma-separated tokens.
//
// Why a file, not stdout: PJSR Console.writeln output and PCL Console() emissions
// do NOT reach the launching shell's stdout under --automation-mode. We wrap the
// run in Console.beginLog() / Console.endLog() to capture the full Process Console
// buffer (including C++ "Phase B: N ms" emissions from stacking_engine.cpp) and
// persist it to a file.  Shell side then greps the file.
//
// NOTE on table column order:
//   NukeXProcess registers columns as [path, enabled] (NXLightFramePath first,
//   then NXLightFrameEnabled). PJSR reflects that registration order, so each
//   row must be [path, enabled], NOT [enabled, path].
//   See src/module/NukeXProcess.cpp lines 20-22.

function parseArgs() {
   var out = { stack: "", log: "/tmp/nukex_baseline_console.log",
               meta: "/tmp/nukex_baseline_meta.txt" };
   if (typeof jsArguments === "undefined") return out;
   for (var i = 0; i < jsArguments.length; i++) {
      var kv = String(jsArguments[i]);
      var eq = kv.indexOf("=");
      if (eq < 0) continue;
      var k = kv.substring(0, eq);
      var v = kv.substring(eq + 1);
      if (k === "stack") out.stack = v;
      else if (k === "log") out.log = v;
      else if (k === "meta") out.meta = v;
   }
   return out;
}

function collectLights(dir) {
   var pats = ["*.fit", "*.fits", "*.FIT", "*.FITS"];
   var all = [];
   for (var i = 0; i < pats.length; i++) {
      var found = searchDirectory(dir + "/" + pats[i], false);
      for (var j = 0; j < found.length; j++) all.push(found[j]);
   }
   return all;
}

function main() {
   var args = parseArgs();

   if (!args.stack) {
      File.writeTextFile(args.meta,
         "STATUS fail\nREASON stack=<dir> argument not provided\n");
      throw new Error("missing stack= argument");
   }

   var lights = collectLights(args.stack);
   if (lights.length === 0) {
      File.writeTextFile(args.meta,
         "STATUS fail\nREASON no FITS found\nSTACK_DIR " + args.stack + "\n");
      throw new Error("no lights in " + args.stack);
   }

   Console.beginLog();
   Console.writeln("Baseline dir: " + args.stack);
   Console.writeln("Baseline lights: " + lights.length);

   var P = new NukeX;

   // Column order matches NukeXProcess registration: [path, enabled].
   var arr = [];
   for (var i = 0; i < lights.length; i++) arr.push([lights[i], true]);
   P.lightFrames = arr;
   P.flatFrames = [];
   // primaryStretch enum: Auto=0, VeraLux=1, GHS=2, MTF=3, ArcSinh=4, Log=5, Lupton=6, CLAHE=7
   // finishingStretch enum: None=0
   // PJSR exposes enum constants on NukeX.prototype by name, but Auto and None both = 0
   // collide into a single NukeX.prototype.Auto entry. Integer literals side-step the
   // collision and make intent explicit.
   P.primaryStretch = 0;   // Auto
   P.finishingStretch = 0; // None
   P.enableGPU = true;
   P.cacheDirectory = "/tmp";

   var start = new Date().getTime();
   var ok = false;
   var execErr = "";
   try {
      ok = P.executeGlobal();
   } catch (e) {
      execErr = String(e);
   }
   var elapsed = new Date().getTime() - start;
   Console.writeln("TOTAL_MS " + elapsed);

   var bytes = Console.endLog();
   File.writeTextFile(args.log, bytes.toString());

   var meta = "STATUS " + (ok ? "ok" : "fail") + "\n" +
              "STACK_DIR " + args.stack + "\n" +
              "LIGHTS " + lights.length + "\n" +
              "TOTAL_MS " + elapsed + "\n" +
              "EXECUTE_OK " + ok + "\n" +
              "LOG_PATH " + args.log + "\n";
   if (execErr) meta += "EXECUTE_ERROR " + execErr + "\n";
   File.writeTextFile(args.meta, meta);

   if (!ok) throw new Error(execErr || "executeGlobal returned false");
}

main();
