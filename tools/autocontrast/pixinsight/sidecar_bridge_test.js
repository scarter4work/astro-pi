/*
 * AutoContrast — PixInsight <-> Python sidecar bridge smoke test (design §3.1).
 *
 * Proves that PixInsight (PJSR) can launch the Python sidecar as a localhost
 * subprocess and round-trip via the file protocol: PJSR writes a request JSON,
 * runs `<venv>/bin/python -m autocontrast.sidecar req.json resp.json`, then reads
 * and parses the response JSON. No network, no NetworkTransfer dependency.
 *
 * Run headless:
 *   PixInsight.sh -n --automation-mode -r=<this file> --force-exit
 *
 * Writes a final PASS/FAIL line to STATUS_FILE so a shell wrapper can assert the
 * outcome regardless of PixInsight's own exit code.
 */

#feature-id    AutoContrast > SidecarBridgeTest

var PROJECT      = "/home/scarter4work/projects/autocontrast";
var PYTHON       = PROJECT + "/.venv/bin/python";
var TEST_IMAGE   = PROJECT + "/data/references/eso1103a.jpg";
var WORK         = "/tmp/autocontrast_bridge";
var STATUS_FILE  = WORK + "/status.txt";

function writeText(path, text) {
   var f = new File;
   f.createForWriting(path);
   f.write(ByteArray.stringToUTF8(text));
   f.close();
}

function readText(path) {
   // Static File.readFile returns a ByteArray of the whole file.
   return File.readFile(path).utf8ToString();
}

function runSidecar(request) {
   var reqPath  = WORK + "/request.json";
   var respPath = WORK + "/response.json";
   writeText(reqPath, JSON.stringify(request));
   // Static, synchronous execute: returns the child's exit code.
   var code = ExternalProcess.execute(PYTHON,
                 ["-m", "autocontrast.sidecar", reqPath, respPath]);
   if (code != 0)
      console.warningln("sidecar exit code = " + code);
   return JSON.parse(readText(respPath));
}

function main() {
   if (!File.directoryExists(WORK))
      File.createDirectory(WORK, true);

   console.writeln("<b>AutoContrast bridge test</b>");
   console.writeln("python: " + PYTHON);

   // 1) ping
   var ping = runSidecar({ op: "ping" });
   console.writeln("ping -> ok=" + ping.ok +
                   "  version=" + (ping.result ? ping.result.version : "?"));
   if (!ping.ok || !ping.result.pong)
      throw new Error("ping failed");

   // 2) fingerprint a real reference render
   var fpResp = runSidecar({
      op: "fingerprint",
      image: TEST_IMAGE,
      pixel_scale_arcsec: 1.0,
      psf_fwhm_arcsec: 2.0,
      palette_class: "RGB",
      n_scales: 6,
      max_dim: 800
   });
   if (!fpResp.ok)
      throw new Error("fingerprint failed: " + fpResp.error);
   var fp = fpResp.result;
   console.writeln("fingerprint -> palette=" + fp.palette_class +
                   "  tonal_quantiles=" + fp.tonal_quantiles.length +
                   "  energy_bins=" + fp.energy_spectrum.energy.length);

   // 3) distance of the fingerprint to itself must be ~0
   var dResp = runSidecar({ op: "distance", reference: fp, target: fp });
   if (!dResp.ok)
      throw new Error("distance failed: " + dResp.error);
   console.writeln("distance(self,self) = " + dResp.result.distance);
   if (Math.abs(dResp.result.distance) > 1e-6)
      throw new Error("self-distance not ~0: " + dResp.result.distance);

   return { version: ping.result.version,
            energy_bins: fp.energy_spectrum.energy.length };
}

try {
   var r = main();
   writeText(STATUS_FILE, "PASS version=" + r.version +
                          " energy_bins=" + r.energy_bins + "\n");
   console.noteln("<b>BRIDGE TEST PASS</b>");
} catch (e) {
   writeText(STATUS_FILE, "FAIL " + e.message + "\n");
   console.criticalln("BRIDGE TEST FAIL: " + e.message);
}
