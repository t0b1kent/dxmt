"""The ordinary HRESULT ABI must not inherit an aggregate-return thunk."""
import unittest
from arm64ec_public_abi import thunk_contract


class PublicEntryContract(unittest.TestCase):
    def test_scalar_reference_preserves_arguments_and_result(self):
        # Save the nonvolatile vector registers, invoke x9, forward x0 to x8,
        # and dispatch. Captured from the passing reference implementation.
        thunk_contract([0xadba9fe6, 0xad0127e8, 0xa90a7bfd, 0x910283fd,
                        0xd63f0120, 0x90000668, 0xf943c501, 0xaa0003e8,
                        0xa94a7bfd, 0xacc69fe6, 0xd61f0020])

    def test_indirect_return_argument_shift_is_rejected(self):
        # The failing release thunk shifts all ordinary arguments one slot.
        with self.assertRaisesRegex(ValueError, 'shifts a Windows argument'):
            thunk_contract([0xaa0003f3, 0xaa0003e8, 0xaa0103e0,
                            0xaa0203e1, 0xaa0303e2, 0xd63f0120,
                            0xaa1303e8, 0xd61f0000])

    def test_saved_return_pointer_cannot_replace_hresult(self):
        with self.assertRaisesRegex(ValueError, 'saved struct pointer'):
            thunk_contract([0xd63f0120, 0xaa1303e8, 0xd61f0000])

    def test_missing_native_call_or_result_forwarding_is_rejected(self):
        for words in [[0xaa0003e8, 0xd61f0020], [0xd63f0120, 0xd61f0020]]:
            with self.subTest(words=words), self.assertRaises(ValueError):
                thunk_contract(words)


if __name__ == '__main__':
    unittest.main()
