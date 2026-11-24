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
    def test_prime_warm_then_rotated_rounds(self):
        calls, runner = self.run_with({"hand": [self.ok(9), self.ok(8), self.ok(0.2), self.ok(0.3)],
                                       "polylink-ref": [self.ok(9), self.ok(8), self.ok(0.5), self.ok(0.4)]})
        data = cb.measure([A, B], {A.ctest: SPEC, B.ctest: SPEC}, 2, 5000, runner)
        self.assertEqual(calls, [("hand", 1000), ("polylink-ref", 1000),      # priming
                                 ("hand", 5000), ("polylink-ref", 5000),      # full-length round, discarded
                                 ("hand", 5000), ("polylink-ref", 5000),      # round 1 starts at the first route
                                 ("polylink-ref", 5000), ("hand", 5000)])     # round 2 at the next
        self.assertEqual(data["hand"].samples["describe"], [0.2, 0.3])
        self.assertEqual(data["polylink-ref"].samples["describe"], [0.5, 0.4])
    def test_missing_route_is_reported(self):
        calls, runner = self.run_with({"hand": [self.ok(9), self.ok(8), self.ok(0.2)]})
        data = cb.measure([A, B], {A.ctest: SPEC, B.ctest: None}, 1, 5000, runner)
        self.assertEqual(data["polylink-ref"].status, "missing")
        self.assertIn("cross_bench_lua_py_ref", data["polylink-ref"].note)
        self.assertEqual([c[0] for c in calls], ["hand", "hand", "hand"])
    def test_failed_route_is_not_rerun(self):
        bad = cb.RunResult(False, {}, {}, "Assertion failed at x")
        calls, runner = self.run_with({"hand": [self.ok(9), self.ok(8), self.ok(0.2), self.ok(0.2)],
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
        rs = cb.rows([A, B], self.DATA, 5000, {"hand": 113, "polylink-ref": 31})
        self.assertEqual(rs[0], {"route": "hand", "label": "hand-written bridge", "workload": "describe",
            "status": "ok", "min_us": "0.200", "median_us": "0.250", "spread_pct": "20.0",
            "rel_to_hand": "1.000", "rounds": "3", "calls": "5000", "route_loc": "113", "note": ""})
        self.assertEqual(rs[1]["status"], "failed"); self.assertEqual(rs[1]["min_us"], "")
        self.assertEqual(rs[1]["note"], "Assertion failed at x")
        self.assertEqual(rs[1]["route_loc"], "31")      # the code exists although the run failed
    def test_route_loc_empty_when_not_counted(self):
        self.assertEqual(cb.rows([A, B], self.DATA, 5000)[0]["route_loc"], "")
    def test_rel_is_the_median_of_per_round_ratios(self):
        d = {"hand": cb.RouteData("ok", "", {"describe": [0.2, 0.4, 0.25]}, {}),
             "polylink-ref": cb.RouteData("ok", "", {"describe": [0.6, 0.4, 1.0]}, {"view": "proxy"})}
        self.assertEqual(cb.rows([A, B], d, 10)[1]["rel_to_hand"], "3.000")    # 3.0, 1.0, 4.0; min/min would be 2
    def test_rel_empty_when_a_baseline_sample_is_zero(self):
        d = {"hand": cb.RouteData("ok", "", {"describe": [0.0]}, {}),
             "polylink-ref": cb.RouteData("ok", "", {"describe": [0.4]}, {"view": "proxy"})}
        self.assertEqual(cb.rows([A, B], d, 1)[1]["rel_to_hand"], "")
    def test_rel_empty_without_baseline(self):
        d = {"hand": cb.RouteData("missing", "no ctest bench_lua_py_hand", {}, {}),
             "polylink-ref": cb.RouteData("ok", "", {"describe": [0.4]}, {"view": "proxy"})}
        self.assertEqual(cb.rows([A, B], d, 10)[1]["rel_to_hand"], "")
    def test_csv_header(self):
        with tempfile.TemporaryDirectory() as t:
            p = os.path.join(t, "f.csv"); cb.write_csv(cb.rows([A, B], self.DATA, 5000), p)
            with open(p) as f:
                self.assertEqual(f.readline().strip(),
                    "route,label,workload,status,min_us,median_us,spread_pct,rel_to_hand,rounds,calls,route_loc,note")

class Precision(unittest.TestCase):
    def ok(self, d): return cb.RunResult(True, {"describe": d}, {}, "")
    def runner(self, times):
        results = [self.ok(t) for t in times]
        return lambda route, spec, n: results.pop(0)
    def test_spread_is_the_interquartile_range_over_the_median(self):
        self.assertEqual(cb.spread([1.0, 1.0, 1.0, 1.0]), 0.0)
        self.assertEqual(cb.spread([0.9, 1.0, 1.0, 1.0, 1.1]), 0.0)     # one outlier each way does not count
        self.assertAlmostEqual(cb.spread([1.0, 2.0, 3.0, 4.0, 5.0]), 2 / 3)
        self.assertEqual(cb.spread([1.0]), 0.0)
    def test_rounds_are_added_until_the_spread_is_within_the_precision(self):
        # priming, discarded round, two requested rounds (noisy), then quiet ones
        runner = self.runner([9, 8, 0.2, 0.4, 0.2, 0.2, 0.2, 0.2, 0.2])
        data = cb.measure([A], {A.ctest: SPEC}, 2, 5000, runner, precision=0.05, max_rounds=9)
        self.assertEqual(data["hand"].samples["describe"], [0.2, 0.4, 0.2, 0.2, 0.2])
    def test_extra_rounds_stop_at_max_rounds(self):
        runner = self.runner([9, 8, 0.2, 0.4, 0.2, 0.4, 0.2, 0.4])
        data = cb.measure([A], {A.ctest: SPEC}, 2, 5000, runner, precision=0.05, max_rounds=4)
        self.assertEqual(len(data["hand"].samples["describe"]), 4)
    def test_noisy_rows_are_named(self):
        table = [{"route": "hand", "workload": "describe", "status": "ok", "spread_pct": "20.0"},
                 {"route": "cffi", "workload": "describe", "status": "ok", "spread_pct": "1.5"},
                 {"route": "lupa", "workload": "", "status": "failed", "spread_pct": ""}]
        self.assertEqual(cb.noisy_rows(table, 0.05), [("hand", "describe", 20.0)])

class Cpu(unittest.TestCase):
    FREQ = {0: 5200, 1: 5200, 2: 5200, 3: 5200, 4: 3900, 5: 3900}
    SIBLINGS = {0: {0, 1}, 1: {0, 1}, 2: {2, 3}, 3: {2, 3}, 4: {4}, 5: {5}}
    def test_a_fast_core_that_is_not_cpu0s(self):
        self.assertEqual(cb.pick_cpu({0, 1, 2, 3, 4, 5}, self.FREQ, self.SIBLINGS), 2)
    def test_uniform_machine_without_topology(self):
        self.assertEqual(cb.pick_cpu({0, 1, 2, 3}, {}, {}), 1)
    def test_single_cpu(self):
        self.assertEqual(cb.pick_cpu({0}, {}, {}), 0)
    def test_stays_inside_the_allowed_set(self):
        self.assertEqual(cb.pick_cpu({4, 5}, self.FREQ, self.SIBLINGS), 4)
    def test_cpu0s_core_when_it_is_the_only_fast_one(self):
        self.assertEqual(cb.pick_cpu({0, 1, 4}, self.FREQ, self.SIBLINGS), 0)

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
        self.assertEqual(cb.ROUTES[2].expect_info, {"python": "shared"})   # lupa must run the libpython the others embed
    def test_sources_exist_and_exclude_what_is_shared(self):
        for r in cb.ROUTES:
            self.assertTrue(r.sources, r.id)
            for path in r.sources:
                self.assertTrue(os.path.isfile(os.path.join(cb.REPO, path)), path)
                self.assertNotIn(os.path.basename(path), ("benchloop.lua", "shapes.py", "CMakeLists.txt"))

CLOC = """{"header" : {"cloc_version" : "2.06", "n_files" : 2},
"/r/tests/a.c" :{"blank": 15, "comment": 16, "code": 103, "language": "C"},
"/r/tests/app.lua" :{"blank": 2, "comment": 5, "code": 10, "language": "Lua"},
"SUM": {"blank": 17, "comment": 21, "code": 113, "nFiles": 2} }"""

class Loc(unittest.TestCase):
    R = cb.Route("hand", "hand-written bridge", "t", {}, ("tests/a.c", "tests/app.lua"))
    def test_parse_cloc_json(self):
        counts, version = cb.parse_cloc_json(CLOC, "/r")
        self.assertEqual(counts, {"tests/a.c": 103, "tests/app.lua": 10})
        self.assertEqual(version, "2.06")
    def test_route_loc_sums_a_routes_files(self):
        self.assertEqual(cb.route_loc([self.R], {"tests/a.c": 103, "tests/app.lua": 10}), {"hand": 113})
    def test_route_loc_unknown_when_a_file_was_not_counted(self):
        self.assertEqual(cb.route_loc([self.R], {"tests/a.c": 103}), {"hand": None})

class Figure(unittest.TestCase):
    DATA = {"hand": cb.RouteData("ok", "", {"describe": [0.25, 0.27, 0.26], "divide": [0.06, 0.06, 0.06]}, {}),
            "polylink-ref": cb.RouteData("failed", "Assertion failed at x", {}, {})}
    def bars(self):
        return cb.figure_bars([A, B], self.DATA, {"hand": 113, "polylink-ref": 31})
    def test_bars_are_median_describe_and_loc(self):
        self.assertEqual(self.bars(), [cb.Bar("hand", "hand-written bridge", 0.26, 113),
                                       cb.Bar("polylink-ref", "PolyLink by reference", None, 31)])
    def test_tikz_carries_values_placeholders_and_provenance(self):
        t = cb.render_tikz(self.bars(), ["generated by a test"])
        self.assertTrue(t.startswith("% generated by a test\n"))
        self.assertIn("\\begin{tikzpicture}", t)
        self.assertTrue(t.rstrip().endswith("\\end{tikzpicture}"))
        for piece in ("{0.26}", "{113}", "{31}", "{?}", "hand-written\\\\bridge", "PolyLink\\\\by reference"):
            self.assertIn(piece, t)
    def test_tikz_without_line_counts_or_measurements(self):
        t = cb.render_tikz([cb.Bar("hand", "hand-written bridge", None, None)], [])
        self.assertIn("{?}", t); self.assertNotIn("None", t); self.assertNotIn("nan", t)
    def test_tallest_bar_fills_the_plot(self):
        self.assertEqual(cb.bar_height(1.04, 1.04), cb.PLOT_HEIGHT)
        self.assertEqual(cb.bar_height(0.52, 1.04), cb.PLOT_HEIGHT / 2)
        self.assertEqual(cb.bar_height(5, 0), 0)
    def test_axis_ticks(self):
        self.assertEqual(cb.axis_ticks(1.04), [0.5, 1.0])
        self.assertEqual(cb.axis_ticks(0.43), [0.2, 0.4])
        self.assertEqual(cb.axis_ticks(113), [50, 100])
        self.assertEqual(cb.axis_ticks(31), [10, 20, 30])
        self.assertEqual(cb.axis_ticks(0), [])

class Provenance(unittest.TestCase):
    def test_cmake_cache_values(self):
        text = "// comment\nPYTHON_LIB:FILEPATH=/usr/lib/libpython3.14.so\nCMAKE_BUILD_TYPE:STRING=RelWithDebInfo\nX:BOOL=ON\n"
        self.assertEqual(cb.cmake_cache_values(text, ["PYTHON_LIB", "CMAKE_BUILD_TYPE", "ABSENT"]),
                         {"PYTHON_LIB": "/usr/lib/libpython3.14.so", "CMAKE_BUILD_TYPE": "RelWithDebInfo", "ABSENT": None})

if __name__ == "__main__":
    unittest.main()
