"""Local bridge input must match the lock and must not fall back to an old ZIP member."""
import hashlib
import importlib.util
import io
import json
from contextlib import redirect_stderr
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch
import zipfile

REPO = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("stage_override_subject", REPO / "prepare_stage.py")
stage = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(stage)


class LocalBridge(unittest.TestCase):
    def setUp(self):
        scratch = REPO.parents[1] / "_scratch" / "dlss5-aio"
        self.temp = tempfile.TemporaryDirectory(prefix="stage-override-", dir=scratch)
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.batch = self.root / "batch"
        self.batch.mkdir()
        self.stage_root = self.batch / "candidate" / "staged"
        self.target = self.stage_root / "test"
        self.marker = self.stage_root / "stage-failure-test.json"
        self.args = ["prepare_stage.py", "--batch", str(self.batch), "--name", "test"]
        members = []
        zip_member = "files/zip-member.txt"
        zip_payload = b"locked zip member"
        for relative, key in stage.EXTRA.items():
            source = self.root / key
            source.write_bytes(key.encode())
            self.args += ["--" + key.replace("_", "-"), str(source)]
            members.append({"source_kind": "stage", "source": relative, "sha256": hashlib.sha256(source.read_bytes()).hexdigest()})
        members.append({"source_kind": "stage", "source": zip_member, "sha256": hashlib.sha256(zip_payload).hexdigest()})
        self.bridge = self.root / "bridge.bin"
        self.bridge.write_bytes(b"new bounded bridge")
        self.bridge_sha = hashlib.sha256(self.bridge.read_bytes()).hexdigest()
        members.append({"source_kind": "stage", "source": "standalone-dlssnr.addon32", "sha256": self.bridge_sha})
        self.lock = {"schema": 1, "members": members}
        (self.root / "payload-lock.json").write_text(json.dumps(self.lock), encoding="utf8")
        archive = self.root / "aio.zip"
        with zipfile.ZipFile(archive, "w") as stream:
            stream.writestr(zip_member, zip_payload)
            stream.writestr("standalone-dlssnr.addon32", b"old unbounded bridge")
        self.args += ["--aio-zip", str(archive)]
        for key, value in (("SCRATCH", self.root), ("MODULE", self.root), ("ZIP_SHA", hashlib.sha256(archive.read_bytes()).hexdigest())):
            context = patch.object(stage, key, value)
            context.start()
            self.addCleanup(context.stop)

    def run_stage(self, extra=None):
        with patch.object(sys, "argv", self.args + (extra or [])):
            return stage.main()

    def write_lock(self, members):
        (self.root / "payload-lock.json").write_text(json.dumps({"schema": 1, "members": members}), encoding="utf8")

    def test_locked_local_bridge_replaces_old_archive_member(self):
        self.run_stage(["--addon32", str(self.bridge)])
        self.assertEqual((self.target / "standalone-dlssnr.addon32").read_bytes(), self.bridge.read_bytes())
        record = json.loads((self.target / "stage-record.json").read_text(encoding="utf8"))
        self.assertEqual(record["state"], "prepared")
        self.assertEqual(record["member_count"], len(self.lock["members"]))
        self.assertEqual(record["addon32_sha256"], self.bridge_sha)

    def test_wrong_local_bridge_rejected_before_creating_output(self):
        self.bridge.write_bytes(b"unexpected binary")
        with redirect_stderr(io.StringIO()) as stderr, self.assertRaises(SystemExit):
            self.run_stage(["--addon32", str(self.bridge)])
        message = stderr.getvalue()
        self.assertIn("standalone-dlssnr.addon32", message)
        self.assertIn(f"expected sha256={self.bridge_sha}", message)
        self.assertIn("需要 --addon32", message)
        self.assertFalse(self.target.exists())
        self.assertFalse((self.batch / "candidate").exists())

    def test_omitting_local_bridge_does_not_accept_old_archive_binary(self):
        with redirect_stderr(io.StringIO()) as stderr, self.assertRaises(SystemExit):
            self.run_stage()
        message = stderr.getvalue()
        self.assertIn("standalone-dlssnr.addon32", message)
        self.assertIn(f"expected sha256={self.bridge_sha}", message)
        self.assertIn("需要 --addon32", message)
        self.assertFalse(self.target.exists())
        self.assertFalse((self.batch / "candidate").exists())

    def test_archive_component_mismatch_reports_expected_hash(self):
        members = [dict(member) for member in self.lock["members"]]
        for member in members:
            if member["source"] == "files/zip-member.txt":
                member["sha256"] = "0" * 64
        self.write_lock(members)
        with redirect_stderr(io.StringIO()) as stderr, self.assertRaises(SystemExit):
            self.run_stage(["--addon32", str(self.bridge)])
        message = stderr.getvalue()
        self.assertIn("files/zip-member.txt", message)
        self.assertIn("0" * 64, message)
        self.assertIn("需要 --addon32", message)
        self.assertFalse(self.target.exists())

    def test_archive_missing_component_reports_expected_hash(self):
        members = [dict(member) for member in self.lock["members"]]
        absent_sha = hashlib.sha256(b"absent member").hexdigest()
        members.append({"source_kind": "stage", "source": "files/absent.txt", "sha256": absent_sha})
        self.write_lock(members)
        with redirect_stderr(io.StringIO()) as stderr, self.assertRaises(SystemExit):
            self.run_stage(["--addon32", str(self.bridge)])
        message = stderr.getvalue()
        self.assertIn("files/absent.txt", message)
        self.assertIn(absent_sha, message)
        self.assertFalse(self.target.exists())

    def test_changed_runtime_input_reports_component_and_expected_hash(self):
        expected_sha = next(member["sha256"] for member in self.lock["members"]
                            if member["source"] == "host64/nvngx_dlssnr.dll")
        (self.root / "nr").write_bytes(b"changed runtime")
        with redirect_stderr(io.StringIO()) as stderr, self.assertRaises(SystemExit):
            self.run_stage(["--addon32", str(self.bridge)])
        message = stderr.getvalue()
        self.assertIn("host64/nvngx_dlssnr.dll", message)
        self.assertIn(expected_sha, message)
        self.assertFalse(self.target.exists())
        self.assertFalse((self.batch / "candidate").exists())

    def test_injected_write_failure_cleans_target_and_retry_succeeds(self):
        real_sha = stage.sha
        calls = {"count": 0}

        def flaky_sha(path):
            if Path(path).is_relative_to(self.target):
                calls["count"] += 1
                if calls["count"] == 3:
                    raise OSError("injected mid-copy failure")
            return real_sha(path)

        with patch.object(stage, "sha", flaky_sha), self.assertRaises(OSError):
            self.run_stage(["--addon32", str(self.bridge)])
        self.assertGreaterEqual(calls["count"], 3)
        self.assertFalse(self.target.exists())
        self.assertTrue(self.marker.is_file())
        self.assertEqual(self.marker.parent, self.stage_root)
        self.assertFalse(self.marker.is_relative_to(self.target))
        failed = json.loads(self.marker.read_text(encoding="utf8"))
        self.assertEqual(failed["state"], "preparation_incomplete")
        self.assertIn("injected mid-copy failure", failed["error"])
        # 同名命令直接重试必须成功，无需手工删除目录。
        self.run_stage(["--addon32", str(self.bridge)])
        self.assertEqual((self.target / "standalone-dlssnr.addon32").read_bytes(), self.bridge.read_bytes())
        record = json.loads((self.target / "stage-record.json").read_text(encoding="utf8"))
        self.assertEqual(record["state"], "prepared")
        self.assertEqual(record["addon32_sha256"], self.bridge_sha)
        self.assertFalse(self.marker.exists())


if __name__ == "__main__":
    unittest.main()
