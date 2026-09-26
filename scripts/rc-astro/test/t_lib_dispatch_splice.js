#include "../RCAstroLib.jsh"
// Unit test for RCAstro._dispatchLine() -- the NDJSON line parser split out
// of runCli() so it can be exercised directly, with synthetic lines that
// simulate a stderr fragment spliced ahead of the first '{' on an otherwise
// well-formed stdout NDJSON line (see the hardening comment above
// _dispatchLine() in RCAstroLib.jsh for why this is defended against even
// though it was not observed in the empirical repro that motivated it).
//
// Result goes to a FILE — console.* does not reach stdout under
// --automation-mode. Same pattern as test/t_lib_runcli.js.
var RESULT_LOG = "/tmp/rc_t_dispatch_splice_result.log";
function assert(c, m){ if(!c){ throw new Error(m); } }

function main() {
   let log = new File;
   log.createForWriting(RESULT_LOG);
   function W(s){ log.outTextLn(s); log.flush(); console.noteln(s); }

   try {
      // --- Case 1: clean line, no splice -- must still work (no regression). ---
      {
         let state = { errorMsg: "", strayText: "" };
         let events = [];
         RCAstro._dispatchLine('{"event":"progress","percent":50}', state, function(ev){ events.push(ev); });
         assert(events.length == 1, "clean line: expected 1 event, got " + events.length);
         assert(events[0].percent == 50, "clean line: expected percent 50, got " + events[0].percent);
         assert(state.strayText == "", "clean line: strayText must stay empty, got: " + state.strayText);
         W("PASS: clean line parses normally");
      }

      // --- Case 2: stderr fragment spliced ahead of a progress event's '{'. ---
      {
         let state = { errorMsg: "", strayText: "" };
         let events = [];
         let spliced = 'ml-onnx.cpp:877 - failed at step 1{"event":"progress","percent":75}';
         RCAstro._dispatchLine(spliced, state, function(ev){ events.push(ev); });
         assert(events.length == 1, "spliced progress: expected the embedded event to be recovered, got " + events.length + " events");
         assert(events[0].percent == 75, "spliced progress: expected percent 75, got " + events[0].percent);
         assert(state.strayText.indexOf("ml-onnx.cpp:877 - failed at step 1") >= 0,
                "spliced progress: stray prefix must be kept in strayText, got: " + state.strayText);
         W("PASS: spliced progress event recovered, prefix kept in strayText");
      }

      // --- Case 3: stderr fragment spliced ahead of an ERROR event -- this is ---
      // the case that matters most: losing it would degrade the real failure
      // reason a caller sees in errorMsg.
      {
         let state = { errorMsg: "", strayText: "" };
         let events = [];
         let spliced = "terminate called after throwing an instance of 'rcastro::Error'" +
                       '{"event":"error","message":"boom"}';
         RCAstro._dispatchLine(spliced, state, function(ev){ events.push(ev); });
         assert(state.errorMsg == "boom", "spliced error: expected errorMsg 'boom', got: '" + state.errorMsg + "'");
         assert(events.length == 1 && events[0].event == "error", "spliced error: onEvent must still fire for the recovered error event");
         assert(state.strayText.indexOf("rcastro::Error") >= 0,
                "spliced error: stray prefix must be kept in strayText, got: " + state.strayText);
         W("PASS: spliced ERROR event recovered into state.errorMsg, prefix kept in strayText");
      }

      // --- Case 4: pure stray line, no '{' anywhere -- must not throw, no event. ---
      {
         let state = { errorMsg: "", strayText: "" };
         let events = [];
         RCAstro._dispatchLine("terminate called after throwing an instance of 'rcastro::Error'", state, function(ev){ events.push(ev); });
         assert(events.length == 0, "pure stray line: no event should fire");
         assert(state.strayText.indexOf("rcastro::Error") >= 0, "pure stray line: must be kept in strayText");
         W("PASS: pure stray line (no brace) handled, no event fired");
      }

      // --- Case 5: malformed JSON after the first '{' -- must fold into ---
      // strayText and not throw, same as the pre-existing malformed-JSON path.
      {
         let state = { errorMsg: "", strayText: "" };
         let events = [];
         RCAstro._dispatchLine('{"event":"progress", percent: }', state, function(ev){ events.push(ev); });
         assert(events.length == 0, "malformed JSON: no event should fire");
         assert(state.strayText.length > 0, "malformed JSON: must be folded into strayText, not dropped");
         W("PASS: malformed JSON after '{' folds into strayText without throwing");
      }

      // --- Case 6: the stray prefix itself contains a '{' (review of 6b9b1d8e). ---
      // Taking the FIRST '{' made JSON.parse fail on "{12}{...}" and the real
      // error event was lost to strayText. The parser must try each '{' left to
      // right and accept the first suffix that parses to an object with "event".
      {
         let state = { errorMsg: "", strayText: "" };
         let events = [];
         RCAstro._dispatchLine('offset {12}{"event":"error","message":"boom"}', state, function(ev){ events.push(ev); });
         assert(state.errorMsg == "boom", "brace in prefix: expected errorMsg 'boom', got: '" + state.errorMsg + "'");
         assert(events.length == 1 && events[0].event == "error", "brace in prefix: the error event must be dispatched exactly once, got " + events.length);
         assert(state.strayText.indexOf("offset {12}") >= 0, "brace in prefix: prefix 'offset {12}' must be kept in strayText, got: " + state.strayText);
         assert(state.strayText.indexOf("boom") < 0, "brace in prefix: the recovered event must not also be dumped into strayText, got: " + state.strayText);
         W("PASS: brace inside the stray prefix does not hide the real event");
      }

      // --- Case 7: a nested object in a clean event must not be split off. ---
      {
         let state = { errorMsg: "", strayText: "" };
         let events = [];
         RCAstro._dispatchLine('{"event":"info","topic":"device","detail":{"event":"nested"}}', state, function(ev){ events.push(ev); });
         assert(events.length == 1 && events[0].event == "info" && events[0].detail.event == "nested",
                "nested object: expected the whole outer event, got " + JSON.stringify(events));
         assert(state.strayText == "", "nested object: strayText must stay empty, got: " + state.strayText);
         W("PASS: nested object inside a clean event is parsed as one event");
      }

      W("PASS t_lib_dispatch_splice");
   } catch (e) {
      W("FAIL: " + e.message);
   }
   log.close();
}
main();
