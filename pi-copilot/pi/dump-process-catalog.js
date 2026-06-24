// PI Copilot — headless process-catalog probe.
//
// Invocation (run in a DEDICATED new instance so a running GUI session is not
// yielded into and force-exited):
//   PixInsight.sh -n --automation-mode --force-exit --default-modules \
//     "-r=pi/dump-process-catalog.js,summaries=pi/process-summaries.json,out=sidecar/data/process-catalog.json"
//
// Why -n: without it, execution yields to an already-running instance and
//   --force-exit would terminate the user's GUI session (no data saved).
// Why key=value args: PJSR does not inherit shell env vars; tokens after the
//   -r= path arrive in `jsArguments`.
// Why write a file: Console output does not reach the shell under --automation-mode.

function parseArgs() {
   var out = { summaries: "", out: "" };
   if (typeof jsArguments === "undefined") return out;
   for (var i = 0; i < jsArguments.length; i++) {
      var kv = String(jsArguments[i]);
      var eq = kv.indexOf("=");
      if (eq < 0) continue;
      out[kv.substring(0, eq)] = kv.substring(eq + 1);
   }
   return out;
}

// Best-effort: try to discover process ids from the running core. PJSR exposes
// no documented "list all processes" call, so this is wrapped defensively and
// we fall back to the seeded id list (the summaries keys) on any failure.
function discoverProcessIds() {
   var ids = [];
   try {
      // ProcessInstance can be constructed from a process id string; the global
      // `Process` table is not reliably enumerable, so we only probe the seed
      // list below. If a future PI exposes enumeration, add it here.
   } catch (e) { /* ignore — seed list is the source of truth for the list */ }
   return ids;
}

function introspect(id) {
   // Each process is a global constructor (e.g. `new SCNR`). Construct it and
   // reflect its settable parameters + default values.
   var ctor = this[id];
   if (typeof ctor !== "function") return null;
   var inst;
   try { inst = new ctor; } catch (e) { return null; }

   var params = {};
   for (var k in inst) {
      if (k.charAt(0) === "_") continue;
      // Skip the process's enum CONSTANTS (e.g. SCNR.Green, SCNR.AverageNeutral):
      // they resolve to a defined value on the constructor prototype, whereas a
      // real instance parameter resolves to `undefined` there. Without this the
      // generated template would emit invalid `P.<Constant> = n;` assignments.
      try { if (ctor.prototype[k] !== undefined) continue; } catch (e) {}
      var v;
      try { v = inst[k]; } catch (e) { continue; }
      var t = typeof v;
      if (t === "function") continue;
      if (t === "number" || t === "boolean" || t === "string") {
         params[k] = { type: t, default: v };
      } else if (Array.isArray(v)) {
         params[k] = { type: "array", default: v };
      }
   }
   return params;
}

function piVersionString() {
   try {
      if (typeof CoreApplication.versionString === "string" && CoreApplication.versionString)
         return CoreApplication.versionString;
   } catch (e) {}
   try {
      if (typeof CoreApplication.versionMajor === "number")
         return CoreApplication.versionMajor + "." + CoreApplication.versionMinor +
                "." + CoreApplication.versionRelease;
   } catch (e) {}
   return "";
}

function buildTemplate(id, params) {
   var lines = ["var P = new " + id + ";"];
   for (var name in params) {
      var d = params[name]["default"];
      var lit = (typeof d === "string") ? JSON.stringify(d)
              : Array.isArray(d) ? JSON.stringify(d)
              : String(d);
      lines.push("P." + name + " = " + lit + ";");
   }
   lines.push("P.executeOn(view, false);");
   return lines.join("\n");
}

function main() {
   var args = parseArgs();
   var summaries = {};
   if (args.summaries && File.exists(args.summaries)) {
      summaries = JSON.parse(File.readTextFile(args.summaries));
   }

   var ids = discoverProcessIds();
   for (var key in summaries) if (ids.indexOf(key) < 0) ids.push(key);

   var catalog = { generatedAt: (new Date()).toISOString(),
                   piVersion: piVersionString(),
                   processes: {} };

   for (var i = 0; i < ids.length; i++) {
      var id = ids[i];
      var params = introspect.call(this, id);
      if (params === null) {
         Console.warningln("skip (not a process): " + id);
         continue;
      }
      catalog.processes[id] = {
         id: id,
         summary: summaries[id] || "",
         params: params,
         template: buildTemplate(id, params)
      };
      Console.writeln("ok: " + id + " (" + Object.keys(params).length + " params)");
   }

   var json = JSON.stringify(catalog, null, 2);
   if (args.out) {
      File.writeTextFile(args.out, json);
      Console.writeln("wrote " + Object.keys(catalog.processes).length + " processes -> " + args.out);
   } else {
      Console.writeln(json);
   }
}

main();
