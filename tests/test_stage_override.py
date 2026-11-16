"""Local bridge input must match the lock and must not fall back to an old ZIP member."""
import hashlib
import importlib.util
import json
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
        self.args = ["prepare_stage.py", "--batch", str(self.batch), "--name", "test"]
        members = []
        for relative, key in stage.EXTRA.items():
            source = self.root / key
            source.write_bytes(key.encode())
            self.args += ["--" + key.replace("_", "-"), str(source)]
            members.append({"source_kind": "stage", "source": relative, "sha256": hashlib.sha256(source.read_bytes()).hexdigest()})
        self.bridge = self.root / "bridge.bin"
        self.bridge.write_bytes(b"new bounded bridge")
        members.append({"source_kind": "stage", "source": "standalone-dlssnr.addon32", "sha256": hashlib.sha256(self.bridge.read_bytes()).hexdigest()})
        (self.root / "payload-lock.json").write_text(json.dumps({"schema": 1, "members": members}), encoding="utf8")
        archive = self.root / "aio.zip"
        with zipfile.ZipFile(archive, "w") as stream:
            stream.writestr("standalone-dlssnr.addon32", b"old unbounded bridge")
        self.args += ["--aio-zip", str(archive)]
        for key, value in (("SCRATCH", self.root), ("MODULE", self.root), ("ZIP_SHA", hashlib.sha256(archive.read_bytes()).hexdigest())):
            context = patch.object(stage, key, value)
            context.start()
            self.addCleanup(context.stop)

    def run_stage(self, extra):
        with patch.object(sys, "argv", self.args + extra):
            stage.main()

    def test_locked_local_bridge_replaces_old_archive_member(self):
        self.run_stage(["--addon32", str(self.bridge)])
        target = self.batch / "candidate/staged/test"
        self.assertEqual((target / "standalone-dlssnr.addon32").read_bytes(), self.bridge.read_bytes())
        record = json.loads((target / "stage-record.json").read_text(encoding="utf8"))
        self.assertEqual(record["addon32_sha256"], hashlib.sha256(self.bridge.read_bytes()).hexdigest())

    def test_wrong_local_bridge_rejected_before_creating_output(self):
        self.bridge.write_bytes(b"unexpected binary")
        with self.assertRaises(SystemExit):
            self.run_stage(["--addon32", str(self.bridge)])
        self.assertFalse((self.batch / "candidate/staged/test").exists())

    def test_omitting_local_bridge_does_not_accept_old_archive_binary(self):
        with self.assertRaises(RuntimeError):
            self.run_stage([])


if __name__ == "__main__":
    unittest.main()
