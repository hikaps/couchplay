# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2025 CouchPlay Contributors

from appium.webdriver.common.appiumby import AppiumBy

from helpers.base_test import BaseTest

# Deterministic option/label text (i18nc source strings; the green smoke tier
# runs in a C locale, so these are the exact accessible names).
OPTION_STREAMING = "Moonlight Stream"
OPTION_PHYSICAL = "Physical Display"


class TestStreaming(BaseTest):
    """Covers the per-instance output-mode selector and the streaming-specific
    controls (resolution, frame rate, bitrate, codec) on SessionSetupPage.

    These are UI/setup-tier tests: they exercise the conditional reveal of the
    streaming fields without starting a session, so they need no D-Bus helper.
    """

    def test_output_mode_selector_present(self, driver):
        self.navigate_to_session_setup(driver)
        combo = self.wait_for_element(driver, AppiumBy.ACCESSIBILITY_ID, "comboOutputMode")
        assert combo.is_displayed()

    def test_streaming_controls_absent_in_physical_mode(self, driver):
        # Default output mode is "physical" -> streaming fields must not render.
        self.navigate_to_session_setup(driver)
        assert self.wait_for_absence(driver, AppiumBy.ACCESSIBILITY_ID, "comboStreamResolution")
        assert self.wait_for_absence(driver, AppiumBy.ACCESSIBILITY_ID, "comboCodec")

    def test_selecting_streaming_reveals_stream_controls(self, driver):
        self.navigate_to_session_setup(driver)
        self.select_combo_option(driver, "comboOutputMode", OPTION_STREAMING)
        # All streaming-only controls should now be present.
        self.wait_for_element(driver, AppiumBy.ACCESSIBILITY_ID, "comboStreamResolution")
        self.wait_for_element(driver, AppiumBy.ACCESSIBILITY_ID, "comboFrameRate")
        self.wait_for_element(driver, AppiumBy.ACCESSIBILITY_ID, "comboCodec")
        self.wait_for_element(driver, AppiumBy.ACCESSIBILITY_ID, "sliderStreamBitrate")

    def test_streaming_codec_options_present(self, driver):
        # Opening the codec combo should expose H.264 / H.265 / AV1.
        self.navigate_to_session_setup(driver)
        self.select_combo_option(driver, "comboOutputMode", OPTION_STREAMING)
        self.select_combo_option(driver, "comboCodec", "H.265")
        # Re-open and assert a different option is selectable (round-trip).
        self.select_combo_option(driver, "comboCodec", "AV1")

    def test_switching_back_to_physical_hides_stream_controls(self, driver):
        self.navigate_to_session_setup(driver)
        self.select_combo_option(driver, "comboOutputMode", OPTION_STREAMING)
        # Sanity: streaming field is now visible.
        self.wait_for_element(driver, AppiumBy.ACCESSIBILITY_ID, "comboStreamResolution")
        # Switch back to physical -> streaming field must disappear again.
        self.select_combo_option(driver, "comboOutputMode", OPTION_PHYSICAL)
        assert self.wait_for_absence(driver, AppiumBy.ACCESSIBILITY_ID, "comboStreamResolution")


    def test_stream_resolution_selectable(self, driver):
        # The resolution combo's onActivated writes back to InstanceConfig;
        # selecting a different option must succeed without breaking the UI.
        self.navigate_to_session_setup(driver)
        self.select_combo_option(driver, "comboOutputMode", OPTION_STREAMING)
        self.select_combo_option(driver, "comboStreamResolution", "1280x720")
        combo = self.wait_for_element(driver, AppiumBy.ACCESSIBILITY_ID, "comboStreamResolution")
        assert combo.is_displayed()

    def test_stream_frame_rate_selectable(self, driver):
        self.navigate_to_session_setup(driver)
        self.select_combo_option(driver, "comboOutputMode", OPTION_STREAMING)
        self.select_combo_option(driver, "comboFrameRate", "30")
        combo = self.wait_for_element(driver, AppiumBy.ACCESSIBILITY_ID, "comboFrameRate")
        assert combo.is_displayed()