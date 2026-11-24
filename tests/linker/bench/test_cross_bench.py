# tests/linker/bench/test_cross_bench.py
import io, os, sys, tempfile, unittest
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cross_bench as cb

NOISY = """lua: MMAP: 0x76c90b179000, (nil), 0x1000, 3, 40, (null), -1, 0x0, mmap
lazily mapped a piece of pageindex at 0x0000410ed9200000
info view=proxy
bench divide: 0.310 us/call (200000 calls)
bench describe: 0.412 us/call (200000 calls)
====================================================
liballocs summary:
queries handled by heap case:                    302
"""
ABORT = """lua5.5: Warning: mapping of /x/points.so could not extend preceding bigalloc
Assertion failed at file /x/generic_malloc_index.h:937: INSERT_DESCRIBES_OBJECT(p_ins)
Aborting program lua5.5, pid 1, from address liballocs_preload.so`__assert_fail+0xc9, in 10 seconds
"""
DOC = {"tests": [
    {"name": "cross_bench_lua_py", "command": ["/usr/bin/lua5.5", "/t/app.lua"],
     "properties": [
        {"name": "ENVIRONMENT", "value": ["LINKPY_HOST_LANG=lua", "LINKSTUB_BUILD_CMD=g++ -O2 -DX=1 a.cpp"]},
        {"name": "WORKING_DIRECTORY", "value": "/b/bench_lua_py"}]},
    {"name": "not_built", "properties": []}]}

A = cb.Route("hand", "hand-written bridge", "bench_lua_py_hand", {})
B = cb.Route("polylink-ref", "PolyLink by reference", "cross_bench_lua_py_ref", {"view": "proxy"})
SPEC = cb.TestSpec(["x"], None, {})

class Parse(unittest.TestCase):
    def test_parse_ignores_noise(self):
        times, info = cb.parse_bench_output(NOISY)
        self.assertEqual(times, {"divide": 0.310, "describe": 0.412})
        self.assertEqual(info, {"view": "proxy"})
    def test_spec_from_ctest_json(self):
        s = cb.spec_from_ctest_json(DOC, "cross_bench_lua_py")
        self.assertEqual(s.command, ["/usr/bin/lua5.5", "/t/app.lua"])
        self.assertEqual(s.workdir, "/b/bench_lua_py")
        self.assertEqual(s.env["LINKSTUB_BUILD_CMD"], "g++ -O2 -DX=1 a.cpp")
    def test_spec_absent_or_not_built_is_none(self):
        self.assertIsNone(cb.spec_from_ctest_json(DOC, "nope"))
        self.assertIsNone(cb.spec_from_ctest_json(DOC, "not_built"))
    def test_note_prefers_assertion_line(self):
        self.assertIn("INSERT_DESCRIBES_OBJECT", cb.failure_note(ABORT))
    def test_note_skips_noise(self):
        self.assertEqual(cb.failure_note(NOISY + "lua: boom\n" + "lazily mapped x\n"), "lua: boom")
        self.assertEqual(cb.failure_note(""), "no output")

class Check(unittest.TestCase):
    def test_ok(self):
        r = cb.check_run(B, 0, NOISY)
        self.assertTrue(r.ok); self.assertEqual(r.times["describe"], 0.412)
    def test_nonzero_exit_fails(self):
        r = cb.check_run(B, 134, ABORT)
        self.assertFalse(r.ok); self.assertIn("Assertion failed", r.note)
    def test_missing_describe_fails(self):
        r = cb.check_run(A, 0, "bench divide: 0.3 us/call (10 calls)\n")
        self.assertFalse(r.ok); self.assertIn("describe", r.note)
    def test_info_mismatch_fails_route(self):
        r = cb.check_run(B, 0, NOISY.replace("view=proxy", "view=native"))
        self.assertFalse(r.ok); self.assertIn("view", r.note)

class Measure(unittest.TestCase):
    def run_with(self, script):          # script: {route id: [RunResult, ...]} consumed in order
        calls = []
        def runner(route, spec, n):
            calls.append((route.id, n)); return script[route.id].pop(0)
        return calls, runner
    def ok(self, d): return cb.RunResult(True, {"describe": d}, {}, "")
    def test_prime_then_interleaved_rounds(self):
        calls, runner = self.run_with({"hand": [self.ok(9), self.ok(0.2), self.ok(0.3)],
                                       "polylink-ref": [self.ok(9), self.ok(0.5), self.ok(0.4)]})
        data = cb.measure([A, B], {A.ctest: SPEC, B.ctest: SPEC}, 2, 5000, runner)
        self.assertEqual(calls, [("hand", 1000), ("polylink-ref", 1000),
                                 ("hand", 5000), ("polylink-ref", 5000),
                                 ("hand", 5000), ("polylink-ref", 5000)])
        self.assertEqual(data["hand"].samples["describe"], [0.2, 0.3])   # the priming run is not a sample
    def test_missing_route_is_reported(self):
        calls, runner = self.run_with({"hand": [self.ok(9), self.ok(0.2)]})
        data = cb.measure([A, B], {A.ctest: SPEC, B.ctest: None}, 1, 5000, runner)
        self.assertEqual(data["polylink-ref"].status, "missing")
        self.assertIn("cross_bench_lua_py_ref", data["polylink-ref"].note)
        self.assertEqual([c[0] for c in calls], ["hand", "hand"])
    def test_failed_route_is_not_rerun(self):
        bad = cb.RunResult(False, {}, {}, "Assertion failed at x")
        calls, runner = self.run_with({"hand": [self.ok(9), self.ok(0.2), self.ok(0.2)],
                                       "polylink-ref": [bad]})
        data = cb.measure([A, B], {A.ctest: SPEC, B.ctest: SPEC}, 2, 5000, runner)
        self.assertEqual(data["polylink-ref"].status, "failed")
        self.assertEqual(data["polylink-ref"].note, "Assertion failed at x")
        self.assertEqual([c[0] for c in calls].count("polylink-ref"), 1)
        self.assertEqual(data["hand"].status, "ok")

class Rows(unittest.TestCase):
    DATA = {"hand": cb.RouteData("ok", "", {"describe": [0.25, 0.2, 0.3]}, {}),
            "polylink-ref": cb.RouteData("failed", "Assertion failed at x", {}, {})}
    def test_rows(self):
        rs = cb.rows([A, B], self.DATA, 5000)
        self.assertEqual(rs[0], {"route": "hand", "label": "hand-written bridge", "workload": "describe",
            "status": "ok", "min_us": "0.200", "median_us": "0.250", "rel_to_hand": "1.000",
            "rounds": "3", "calls": "5000", "note": ""})
        self.assertEqual(rs[1]["status"], "failed"); self.assertEqual(rs[1]["min_us"], "")
        self.assertEqual(rs[1]["note"], "Assertion failed at x")
    def test_rel_empty_without_baseline(self):
        d = {"hand": cb.RouteData("missing", "no ctest bench_lua_py_hand", {}, {}),
             "polylink-ref": cb.RouteData("ok", "", {"describe": [0.4]}, {"view": "proxy"})}
        self.assertEqual(cb.rows([A, B], d, 10)[1]["rel_to_hand"], "")
    def test_csv_header(self):
        with tempfile.TemporaryDirectory() as t:
            p = os.path.join(t, "f.csv"); cb.write_csv(cb.rows([A, B], self.DATA, 5000), p)
            with open(p) as f:
                self.assertEqual(f.readline().strip(),
                    "route,label,workload,status,min_us,median_us,rel_to_hand,rounds,calls,note")

class Exit(unittest.TestCase):
    OK = cb.RouteData("ok", "", {"describe": [0.2]}, {})
    MISSING = cb.RouteData("missing", "no ctest x in the build", {}, {})
    FAILED = cb.RouteData("failed", "exit 1: boom", {}, {})
    def test_all_ok_is_zero(self):
        self.assertEqual(cb.exit_status({"a": self.OK}, allow_missing=False), 0)
    def test_missing_is_one_unless_allowed(self):
        self.assertEqual(cb.exit_status({"a": self.OK, "b": self.MISSING}, allow_missing=False), 1)
        self.assertEqual(cb.exit_status({"a": self.OK, "b": self.MISSING}, allow_missing=True), 0)
    def test_failed_is_one_even_when_missing_is_allowed(self):
        self.assertEqual(cb.exit_status({"a": self.OK, "b": self.FAILED}, allow_missing=True), 1)
    def test_signal_exit_is_named(self):
        r = cb.check_run(A, -6, "boom\n")
        self.assertFalse(r.ok); self.assertEqual(r.note, "killed by SIGABRT: boom")
    def test_traceback_lines_are_not_the_note(self):
        text = "lua5.5: benchloop: describe returned 8, expected 7\nstack traceback:\n\t[C]: in ?\n"
        self.assertEqual(cb.failure_note(text), "lua5.5: benchloop: describe returned 8, expected 7")

class Routes(unittest.TestCase):
    def test_table(self):
        self.assertEqual([(r.id, r.ctest) for r in cb.ROUTES], [
            ("hand", "bench_lua_py_hand"), ("cffi", "bench_lua_py_cffi"), ("lupa", "bench_lua_py_lupa"),
            ("polylink-value", "cross_bench_lua_py"), ("polylink-ref", "cross_bench_lua_py_ref"),
            ("polylua-native", "cross_bench_lua_py_native")])
        self.assertEqual(cb.ROUTES[4].expect_info, {"view": "proxy"})
        self.assertEqual(cb.ROUTES[5].expect_info, {"view": "native"})

if __name__ == "__main__":
    unittest.main()
