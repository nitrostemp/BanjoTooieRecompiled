"""Disposable checks for generation and SDL provenance gates."""
import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import codegen_provenance as provenance


class ProvenanceTests(unittest.TestCase):
    def test_receipt_rejects_input_output_and_file_set_drift(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / 'tools').mkdir()
            (root / 'generated').mkdir()
            source = root / 'tools/instrument_continuous.py'
            source.write_text('original')
            output = root / 'generated/funcs_0.c'
            output.write_text('generated')
            provenance.write_receipt(root, [source], {'generation': 'fixture'})
            provenance.validate(root, required_inputs=['tools/instrument_continuous.py'])
            source.write_text('changed')
            with self.assertRaisesRegex(ValueError, 'input'):
                provenance.validate(root, required_inputs=['tools/instrument_continuous.py'])
            source.write_text('original')
            output.write_text('changed')
            with self.assertRaisesRegex(ValueError, 'output'):
                provenance.validate(root, required_inputs=['tools/instrument_continuous.py'])
            output.write_text('generated')
            (root / 'generated/extra.c').write_text('extra')
            with self.assertRaisesRegex(ValueError, 'output'):
                provenance.validate(root, required_inputs=['tools/instrument_continuous.py'])

    def test_missing_receipt_and_missing_required_input_fail(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            with self.assertRaises(ValueError):
                provenance.validate(root)
            (root / 'generated').mkdir()
            provenance.write_receipt(root, [], {'generation': 'fixture'})
            with self.assertRaisesRegex(ValueError, 'required input'):
                provenance.validate(root)

    def test_sdl_pin_and_explicit_exact_override(self):
        with tempfile.TemporaryDirectory() as temp:
            dll = Path(temp) / 'SDL2.dll'
            dll.write_bytes(b'pinned')
            pinned = provenance.sha(dll)
            provenance.validate_sdl(dll, pinned)
            dll.write_bytes(b'observer')
            with self.assertRaises(ValueError):
                provenance.validate_sdl(dll, pinned)
            provenance.validate_sdl(dll, pinned, provenance.sha(dll))
            with self.assertRaises(ValueError):
                provenance.validate_sdl(dll, pinned, '0' * 64)

    def test_build_receipt_binds_executable_and_generation(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / 'generated').mkdir()
            inputs = []
            for name in provenance.REQUIRED_INPUTS:
                path = root / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(name)
                inputs.append(path)
            (root / 'generated/funcs_0.c').write_text('code')
            provenance.write_receipt(root, inputs, {'generation': 'fixture'})
            executable = root / 'game.exe'
            executable.write_bytes(b'executable')
            receipt = {'schema': 1, 'executable_sha256': provenance.sha(executable),
                       'codegen_receipt_sha256': provenance.sha(root / 'generated' / provenance.RECEIPT)}
            (root / 'codegen-build-receipt.json').write_text(json.dumps(receipt))
            provenance.validate_build(root, executable)
            executable.write_bytes(b'other executable')
            with self.assertRaisesRegex(ValueError, 'mismatch'):
                provenance.validate_build(root, executable)

    def test_replay_allows_hook_change_but_not_preparation_or_metadata_change(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / 'generated').mkdir()
            inputs = []
            for name in provenance.REQUIRED_INPUTS:
                path = root / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(name)
                inputs.append(path)
            metadata = root / 'generated/core1_metadata.hpp'
            metadata.write_text('metadata')
            provenance.write_receipt(root, inputs, {'generation': 'fixture'})
            (root / 'tools/instrument_continuous.py').write_text('new hooks')
            provenance.validate(root, replay=True)
            metadata.write_text('changed metadata')
            with self.assertRaisesRegex(ValueError, 'output'):
                provenance.validate(root, replay=True)
            metadata.write_text('metadata')
            (root / 'config/selected.syms.toml').write_text('changed config')
            with self.assertRaisesRegex(ValueError, 'input'):
                provenance.validate(root, replay=True)


if __name__ == '__main__':
    unittest.main()
