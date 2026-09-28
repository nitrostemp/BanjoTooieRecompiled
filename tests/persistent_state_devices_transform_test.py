import importlib.util
import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "prepare_persistent_devices", ROOT / "tools" / "prepare_persistent_devices.py")


class DeviceMaterializerTests(unittest.TestCase):
    def test_vi_parks_before_consumers(self):
        coordinator = (ROOT / "src" / "persistent_state_devices.cpp").read_text()
        self.assertIn("consumer && (worker_parked & vi_worker_bit) == 0", coordinator)
        self.assertIn("gfx_dp_freeze_wait", coordinator)
        self.assertIn("blocked_samples >= 2", coordinator)
        self.assertIn("gfx_phase=%u gfx_queue=%llu dp_status=0x%08X", coordinator)

    def test_prepared_runtime_gets_only_experimental_device_hooks(self):
        module = importlib.util.module_from_spec(SPEC)
        SPEC.loader.exec_module(module)
        prepared = ROOT / "build" / "windows-dev" / "runtime-continuous"
        for name, expected in (
            ("events", "tooie_persistent_events_export"),
            ("timer", "tooie_persistent_timer_export"),
            ("pi", "tooie_persistent_pi_export"),
        ):
            with self.subTest(name=name):
                original = (prepared / f"{name}.cpp").read_text()
                transformed = module.transform(name, original)
                self.assertIn(expected, transformed)
                self.assertIn("persistent_state_devices.hpp", transformed)
                if name == "events":
                    self.assertIn("tooie_persistent_events_renderer_action", transformed)
                    self.assertIn("tooie_persistent_events_graphics_diagnostic", transformed)
                    self.assertIn("tooie_device_gfx_phase.store(4", transformed)
                    self.assertIn("Worker::Vi)) return;\n                const auto deadline", transformed)
                    self.assertIn("terminal_abort_requested()) break", transformed)
                if name == "timer":
                    self.assertIn("tooie_device_active_timers", transformed)
                    self.assertIn("ostime_offset = in->os_time_offset", transformed)
                if name == "pi":
                    self.assertIn("private in-memory write", transformed)
                    self.assertIn("seed_private_eeprom(\n            ultramodern::get_save_file_path()", transformed)
                    self.assertLess(
                        transformed.index("seed_private_eeprom("),
                        transformed.index("create_directories(save_file_path.parent_path())"))
                self.assertNotEqual(original, transformed)
                with self.assertRaises(ValueError):
                    module.transform(name, transformed)


if __name__ == "__main__":
    unittest.main()
