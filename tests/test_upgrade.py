from c0.storage import Storage as StorageC0
from c.storage import Storage as StorageC

from . import common

# Strings for testing ChaCha20 encryption.
chacha_strings = [
    b"Short string.",
    b"",
    b"Although ChaCha20 is a stream cipher, it operates on blocks of 64 bytes. This string is over 152 bytes in length so that we test multi-block encryption.",
    b"This string is exactly 64 bytes long, that is exactly one block.",
]


def set_values(s):
    s.set(0xBEEF, b"Hello")
    s.set(0xCAFE, b"world!  ")
    s.set(0xDEAD, b"How\n")
    s.change_pin(1, 1222)
    s.set(0xAAAA, b"are")
    s.set(0x0901, b"you?")
    s.set(0x0902, b"Lorem")
    s.set(0x0903, b"ipsum")
    s.change_pin(1222, 199)
    s.set(0xDEAD, b"A\n")
    s.set(0xDEAD, b"AAAAAAAAAAA")
    s.set(0x2200, b"BBBB")
    for i, string in enumerate(chacha_strings):
        s.set(0x0301 + i, string)


def check_values(s):
    assert s.unlock(199)
    assert s.get(0xAAAA) == b"are"
    assert s.get(0x0901) == b"you?"
    assert s.get(0x0902) == b"Lorem"
    assert s.get(0x0903) == b"ipsum"
    assert s.get(0xDEAD) == b"AAAAAAAAAAA"
    assert s.get(0x2200) == b"BBBB"
    for i, string in enumerate(chacha_strings):
        assert s.get(0x0301 + i) == string


def test_upgrade():
    sc0 = StorageC0()
    sc0.init()
    assert sc0.unlock(1)
    set_values(sc0)
    for _ in range(10):
        assert not sc0.unlock(3)

    sc1 = StorageC()
    sc1._set_flash_buffer(sc0._get_flash_buffer())
    sc1.init(common.test_uid)
    assert sc1.get_pin_rem() == 6
    check_values(sc1)
