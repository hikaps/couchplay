# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2025 CouchPlay Contributors

import os
import subprocess

import pytest

from helpers.base_test import BaseTest

# Container-only: sunshine is installed in the #27 image, not on dev machines.
pytestmark = pytest.mark.requires_helper

GENERATOR = "/src/couchplay/build/bin/sunshine_config_generator"


class TestSunshineIntegration(BaseTest):
    """Feeds REAL SunshineConfig output to the real Sunshine binary (installed in
    the #27 image) and asserts Sunshine accepts it.

    This is the test the unit suite cannot be: it catches drift between
    SunshineConfig and what Sunshine actually parses -- the class of silent
    failure (wrong key name / value shape) that would make pairing or streaming
    fail in the field. It does NOT prove end-to-end streaming (no Moonlight
    client / hardware encoder); it proves config acceptance + that Sunshine runs.
    """

    def test_sunshine_accepts_sunshine_config_output(self, driver):
        # 1. Emit a real SunshineConfig config set (sunshine.conf + apps.json +
        #    credentials.json) via the generator binary built into the image.
        gen = subprocess.run(
            [GENERATOR, "/tmp/cp-sun-int", "0"], capture_output=True, text=True
        )
        assert gen.returncode == 0, f"generator failed: {gen.stderr}"
        conf = gen.stdout.strip()
        assert conf.endswith("sunshine.conf"), conf

        # 2. Run real Sunshine against it for a short, bounded window.
        env = os.environ.copy()
        run = subprocess.run(
            ["timeout", "10", "sunshine", conf],
            capture_output=True,
            text=True,
            env=env,
        )
        combined = run.stdout + run.stderr

        # 3. Sunshine actually PARSED the generated config -- assert VALUES
        #    unique to SunshineConfig's output, not just key names. Sunshine's
        #    own built-in defaults contain the same keys, so a key-only check
        #    could pass even if it ignored the positional config argument; only
        #    a value unique to our generated config proves it was read.
        assert "config: 'sunshine_name' = CouchPlay Player 0" in combined, (
            "Sunshine did not read the generated sunshine_name"
        )
        assert "config: 'credentials_file' = /tmp/cp-sun-int/credentials.json" in combined, (
            "Sunshine did not read the generated credentials_file path"
        )

        # 4. No config-parse rejection. The authoritative check is #3 (each key
        #    logged as "config: 'key' = value" means Sunshine accepted it); a
        #    renamed/rejected key would simply not appear there. This narrow
        #    guard catches an explicit parse error without matching benign
        #    probe noise (Sunshine logs "unknown" for absent GPU/encoder
        #    hardware, which is not a config problem).
        assert "fatal error" not in combined.lower() and "could not parse" not in combined.lower(), (
            "Sunshine failed to parse the SunshineConfig output"
        )

        # 5. Sunshine stayed up until the timeout -- it did not crash on the
        #    config. (124 == timeout's exit code.)
        assert run.returncode == 124, (
            f"Sunshine exited early (rc={run.returncode}); output:\n{combined[:600]}"
        )
