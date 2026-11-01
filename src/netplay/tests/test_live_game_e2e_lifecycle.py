
from __future__ import annotations

import os
from pathlib import Path
import tempfile
from types import SimpleNamespace
import unittest

import live_game_e2e as harness


class LiveGameE2eLifecycleTests(unittest.TestCase):
    def make_owned_run(
        self, output_root: Path, name: str, roles: list[str]
    ) -> Path:
        run = output_root / name
        run.mkdir()
        harness.write_harness_marker(run, output_root, roles)
        for role in roles:
            game = run / role / "g"
            game.mkdir(parents=True)
            (game / "sentinel.bin").write_bytes(b"disposable")
            (run / role / "qoh99caster.exe").write_bytes(b"caster")
        return run

    def test_replay_artifact_check_descends_into_set_folders(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            folder = root / "replays" / "netplay" / "set"
            folder.mkdir(parents=True)
            (folder / "completed.q99r").touch()
            (folder / "rematch.q99r").touch()

            def inspect(args, cwd, timeout):
                terminal = 1 if Path(args[-1]).name == "completed.q99r" else 4
                return 0, ("status      : ok\nclass       : sealed game\n"
                           "footer      : present\nframes: 10\n"
                           f"terminal    : {terminal} (test)\n")

            run = harness.LiveGameHarness.__new__(harness.LiveGameHarness)
            run.args = SimpleNamespace(full_match_rematch=True, game_count=2)
            run.roles = {"host": SimpleNamespace(
                root=root, caster_exe=Path("caster.exe"),
                host=SimpleNamespace(glob=lambda p, pattern: sorted(p.glob(pattern)),
                                     run=inspect))}
            run.report = {}
            run._display_path = lambda role, path: str(path)
            run.event = lambda *args, **kwargs: None
            run.validate_replay_artifacts()
            self.assertEqual(len(run.report["replay_artifacts"]["host"]), 2)
            (folder / "torn.q99r.part").touch()
            with self.assertRaisesRegex(harness.HarnessFailure, "torn replay artifacts"):
                run.validate_replay_artifacts()

    def test_unmarked_and_outside_paths_are_never_cleanup_targets(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            output_root = root / "output"
            output_root.mkdir()
            legacy = output_root / "legacy"
            legacy_game = legacy / "h" / "g"
            legacy_game.mkdir(parents=True)
            (legacy_game / "keep.bin").write_bytes(b"legacy")
            outside = root / "outside"
            outside_game = outside / "h" / "g"
            outside_game.mkdir(parents=True)
            (outside_game / "keep.bin").write_bytes(b"outside")

            self.assertEqual(
                harness.remove_owned_game_clones(legacy, output_root)["deleted"],
                [],
            )
            self.assertEqual(
                harness.remove_owned_game_clones(outside, output_root)["deleted"],
                [],
            )
            self.assertTrue((legacy_game / "keep.bin").is_file())
            self.assertTrue((outside_game / "keep.bin").is_file())

    def test_owned_run_keeps_compact_evidence_and_removes_only_payloads(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            output_root = root / "output"
            output_root.mkdir()
            run = self.make_owned_run(output_root, "owned", ["h", "c"])
            (run / "report.json").write_text("{}", encoding="utf-8")
            (run / "h" / "caster.stdout.log").write_text(
                "compact log", encoding="utf-8"
            )

            clone_result = harness.remove_owned_game_clones(run, output_root)
            caster_result = harness.remove_owned_caster_copies(run, output_root)

            self.assertFalse(clone_result["errors"])
            self.assertFalse(caster_result["errors"])
            self.assertFalse((run / "h" / "g").exists())
            self.assertFalse((run / "c" / "g").exists())
            self.assertFalse((run / "h" / "qoh99caster.exe").exists())
            self.assertTrue((run / "report.json").is_file())
            self.assertTrue((run / "h" / "caster.stdout.log").is_file())

    def test_bounded_prune_preserves_only_newest_marker_owned_clone(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            output_root = root / "output"
            output_root.mkdir()
            old = self.make_owned_run(output_root, "old", ["h"])
            new = self.make_owned_run(output_root, "new", ["h"])
            unmarked = output_root / "legacy"
            legacy_game = unmarked / "h" / "g"
            legacy_game.mkdir(parents=True)
            (legacy_game / "keep.bin").write_bytes(b"legacy")
            os.utime(old, ns=(1_000_000_000, 1_000_000_000))
            os.utime(new, ns=(2_000_000_000, 2_000_000_000))

            result = harness.prune_stale_owned_game_clones(output_root, 1)

            self.assertFalse(result["errors"])
            self.assertFalse((old / "h" / "g").exists())
            self.assertTrue((new / "h" / "g").is_dir())
            self.assertTrue((legacy_game / "keep.bin").is_file())

    def test_prior_access_ready_cannot_be_mistaken_for_current_run_output(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            output_root = root / "output"
            output_root.mkdir()
            prior = self.make_owned_run(output_root, "prior", ["h"])
            current = self.make_owned_run(output_root, "current", ["h"])
            prior_stdout = prior / "h" / "caster.stdout.log"
            prior_stdout.write_text(
                "[spectator-contract] v=1 state=access-ready "
                "transport=direct-udp receiveOnly=1 port=7501 session=7\n",
                encoding="ascii",
            )
            current_stdout = current / "h" / "caster.stdout.log"
            current_stdout.write_text(
                "[spectator-contract] v=1 state=unavailable session=8\n",
                encoding="ascii",
            )

            self.assertIsNotNone(
                harness.CASTER_SPECTATOR_ACCESS_READY_RE.search(
                    prior_stdout.read_text(encoding="ascii")
                )
            )
            self.assertIsNone(
                harness.CASTER_SPECTATOR_ACCESS_READY_RE.search(
                    current_stdout.read_text(encoding="ascii")
                )
            )
            self.assertNotEqual(prior.resolve(), current.resolve())


if __name__ == "__main__":
    unittest.main()
